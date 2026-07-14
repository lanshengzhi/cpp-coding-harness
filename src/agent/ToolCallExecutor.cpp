#include "ToolCallExecutor.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

[[nodiscard]] std::vector<ai::ToolCallContent> tool_calls(
    const ai::AssistantMessage& message) {
    std::vector<ai::ToolCallContent> calls;
    for (const auto& block : message.content) {
        if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
            calls.push_back(*call);
        }
    }
    return calls;
}

[[nodiscard]] ai::ToolResultMessage error_tool_result(
    const ai::ToolCallContent& call,
    std::string message) {
    ai::ToolResultMessage result;
    result.tool_call_id = call.id;
    result.tool_name = call.name;
    result.content.emplace_back(ai::TextContent{std::move(message), std::nullopt});
    result.is_error = true;
    return result;
}

[[nodiscard]] util::Expected<util::JsonValue> arguments_for_call(
    const ai::ToolCallContent& call) {
    if (!call.arguments_valid) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "invalid tool arguments",
            call.argument_error.value_or("tool arguments were malformed")));
    }
    if (call.arguments) {
        return *call.arguments;
    }
    if (call.raw_arguments.empty()) {
        return util::read_json<util::JsonValue>("{}");
    }
    return util::read_json<util::JsonValue>(call.raw_arguments);
}

[[nodiscard]] util::Expected<BeforeToolCallResult> invoke_before_hook(
    BeforeToolCallHook& hook,
    const BeforeToolCallContext& context) {
    try {
        return hook(context);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "beforeToolCall hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "beforeToolCall hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::Expected<AfterToolCallResult> invoke_after_hook(
    AfterToolCallHook& hook,
    const AfterToolCallContext& context) {
    try {
        return hook(context);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "afterToolCall hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "afterToolCall hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::ExpectedVoid emit(AgentEventSink& sink, const AgentLifecycleEvent& event) {
    if (!sink) {
        return {};
    }
    try {
        return sink(event);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "agent event sink failed",
            e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "agent event sink failed",
            "unknown exception"));
    }
}

[[nodiscard]] util::ExpectedVoid emit_tool_result_message(
    AgentEventSink& sink,
    const ai::ToolResultMessage& message) {
    if (auto r = emit(sink, MessageStartEvent{ai::MessageVariant{message}}); !r) {
        return r;
    }
    return emit(sink, MessageEndEvent{ai::MessageVariant{message}});
}

struct FinalizedToolCall {
    ai::ToolResultMessage result;
    bool call_terminate{false};
};

void apply_after_result(
    ai::ToolResultMessage& tool_result,
    bool& call_terminate,
    AfterToolCallResult after_result,
    bool default_terminate) {
    if (after_result.content) {
        tool_result.content = std::move(*after_result.content);
    }
    if (after_result.details) {
        tool_result.details = std::move(*after_result.details);
    }
    if (after_result.is_error) {
        tool_result.is_error = *after_result.is_error;
    }
    call_terminate = after_result.terminate.value_or(default_terminate);
}

[[nodiscard]] ToolCallBatchResult make_batch_result(
    std::vector<FinalizedToolCall> finalized) {
    const bool terminate = !finalized.empty() &&
                           std::all_of(finalized.begin(), finalized.end(), [](const FinalizedToolCall& call) {
                               return call.call_terminate && !call.result.is_error;
                           });

    std::vector<ai::ToolResultMessage> results;
    results.reserve(finalized.size());
    for (auto& call : finalized) {
        results.push_back(std::move(call.result));
    }
    return ToolCallBatchResult{std::move(results), terminate};
}

} // namespace

ToolCallExecutor::ToolCallExecutor(
    const AsyncToolRegistry& registry,
    ToolCallExecutorOptions options)
    : registry_(registry), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute(
    ToolCallBatchRequest request,
    AgentEventSink& sink) {
    const auto* parallel = std::get_if<BoundedParallelToolExecution>(&options_.execution);
    if (parallel != nullptr && parallel->max_in_flight == 0) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid bounded parallel tool execution policy",
            "max_in_flight must be greater than zero"));
    }

    const auto calls = tool_calls(request.assistant_message);
    if (calls.empty()) {
        co_return ToolCallBatchResult{};
    }

    const bool all_parallel_safe = std::all_of(
        calls.begin(), calls.end(), [this](const ai::ToolCallContent& call) {
            const auto* tool = registry_.find(call.name);
            return tool != nullptr && tool->concurrency() == ToolConcurrency::ParallelSafe;
        });

    if (parallel == nullptr || parallel->max_in_flight == 1 || !all_parallel_safe) {
        co_return co_await execute_sequential(request, calls, sink);
    }

    co_return co_await execute_parallel(request, calls, parallel->max_in_flight, sink);
}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_sequential(
    ToolCallBatchRequest request,
    const std::vector<ai::ToolCallContent>& calls,
    AgentEventSink& sink) {
    std::vector<FinalizedToolCall> finalized;
    finalized.reserve(calls.size());

    for (const auto& call : calls) {
        CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

        ai::ToolResultMessage tool_result;
        bool executed_successfully = false;
        bool call_terminate = false;
        auto* tool = registry_.find(call.name);
        if (tool == nullptr) {
            tool_result = error_tool_result(call, "unknown tool: " + call.name);
        } else {
            auto arguments = arguments_for_call(call);
            if (!arguments) {
                tool_result = error_tool_result(call, arguments.error().detail);
            } else {
                ToolInvocation invocation{call.id, call.name, std::move(*arguments), call.raw_arguments};
                bool blocked = false;

                if (options_.before_tool_call) {
                    BeforeToolCallContext hook_context{
                        request.assistant_message, call, invocation.arguments, request.context};
                    auto before_result = invoke_before_hook(*options_.before_tool_call, hook_context);
                    if (!before_result) {
                        co_return std::unexpected(before_result.error());
                    }
                    if (before_result->block) {
                        blocked = true;
                        tool_result = error_tool_result(
                            call, before_result->reason.value_or("Tool execution was blocked"));
                    }
                }

                if (!blocked) {
                    std::optional<util::Expected<AsyncToolExecutionResult>> executed;
                    try {
                        executed = co_await tool->execute(invocation);
                    } catch (...) {
                        tool_result = error_tool_result(call, "Tool execution failed.");
                    }

                    if (executed) {
                        if (!*executed) {
                            tool_result = error_tool_result(
                                call,
                                executed->error().detail.empty() ? executed->error().message
                                                                 : executed->error().detail);
                        } else {
                            executed_successfully = true;
                            tool_result.tool_call_id = call.id;
                            tool_result.tool_name = call.name;
                            tool_result.content = (*executed)->content;
                            tool_result.details = (*executed)->details;
                            tool_result.is_error = (*executed)->is_error;

                            if (!(*executed)->is_error && options_.after_tool_call) {
                                AfterToolCallContext hook_context{
                                    request.assistant_message,
                                    call,
                                    invocation.arguments,
                                    **executed,
                                    false,
                                    request.context};
                                auto after_result = invoke_after_hook(*options_.after_tool_call, hook_context);
                                if (!after_result) {
                                    co_return std::unexpected(after_result.error());
                                }
                                apply_after_result(
                                    tool_result,
                                    call_terminate,
                                    std::move(*after_result),
                                    (*executed)->terminate);
                            } else {
                                call_terminate = (*executed)->terminate;
                            }
                        }
                    }
                }
            }
        }

        if (!executed_successfully || tool_result.is_error) {
            call_terminate = false;
        }

        const auto tool_text = ai::text_from_content(tool_result.content);
        (void)tool_text;
        AsyncToolExecutionResult execution_result;
        execution_result.content = tool_result.content;
        execution_result.details = tool_result.details;
        execution_result.is_error = tool_result.is_error;
        execution_result.terminate = call_terminate;
        CCH_TRY_VOID(emit(sink, ToolExecutionEndEvent{
            call.id, call.name, std::move(execution_result), tool_result.is_error}));
        CCH_TRY_VOID(emit_tool_result_message(sink, tool_result));
        finalized.push_back(FinalizedToolCall{std::move(tool_result), call_terminate});
    }

    co_return make_batch_result(std::move(finalized));
}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_parallel(
    ToolCallBatchRequest request,
    const std::vector<ai::ToolCallContent>& calls,
    std::size_t max_in_flight,
    AgentEventSink& sink) {
    struct PreparedToolCall {
        ai::ToolCallContent tool_call;
        AsyncAgentTool* tool{};
        util::JsonValue arguments;
        bool blocked{false};
        ai::ToolResultMessage blocked_result;
    };

    std::vector<PreparedToolCall> prepared;
    prepared.reserve(calls.size());

    for (const auto& call : calls) {
        CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

        auto* tool = registry_.find(call.name);
        if (tool == nullptr) {
            prepared.push_back({call, nullptr, {}, true, error_tool_result(call, "unknown tool: " + call.name)});
            continue;
        }

        auto arguments = arguments_for_call(call);
        if (!arguments) {
            prepared.push_back({call, nullptr, {}, true, error_tool_result(call, arguments.error().detail)});
            continue;
        }

        bool blocked = false;
        ai::ToolResultMessage blocked_result;
        if (options_.before_tool_call) {
            BeforeToolCallContext hook_context{
                request.assistant_message, call, *arguments, request.context};
            auto before_result = invoke_before_hook(*options_.before_tool_call, hook_context);
            if (!before_result) {
                co_return std::unexpected(before_result.error());
            }
            if (before_result->block) {
                blocked = true;
                blocked_result = error_tool_result(
                    call, before_result->reason.value_or("Tool execution was blocked"));
            }
        }

        prepared.push_back({call, tool, std::move(*arguments), blocked, std::move(blocked_result)});
    }

    struct ParallelState {
        std::mutex callback_mutex;
        std::mutex error_mutex;
        std::optional<util::Error> emit_error;
        std::optional<util::Error> fatal_error;
    };

    auto executor = co_await boost::asio::this_coro::executor;
    auto state = std::make_shared<ParallelState>();
    auto prepared_calls = std::make_shared<std::vector<PreparedToolCall>>(std::move(prepared));
    auto assistant_snapshot = std::make_shared<ai::AssistantMessage>(request.assistant_message);
    auto context_snapshot = std::make_shared<ai::AiContext>(request.context);
    auto next_index = std::make_shared<std::atomic_size_t>(0);
    auto finalized = std::make_shared<std::vector<FinalizedToolCall>>(prepared_calls->size());

    const std::size_t worker_count = std::min(max_in_flight, prepared_calls->size());

    auto parallel_emit = [state, sink_ptr = &sink](const AgentLifecycleEvent& event) -> util::ExpectedVoid {
        std::lock_guard callback_lock(state->callback_mutex);
        auto result = emit(*sink_ptr, event);
        if (!result) {
            std::lock_guard error_lock(state->error_mutex);
            if (!state->emit_error) {
                state->emit_error = result.error();
            }
        }
        return result;
    };

    AfterToolCallHook* after_hook = options_.after_tool_call ? &*options_.after_tool_call : nullptr;

    auto worker_body = [after_hook,
                        prepared_calls,
                        assistant_snapshot,
                        context_snapshot,
                        next_index,
                        finalized,
                        parallel_emit,
                        state]() -> boost::asio::awaitable<void> {
        try {
            while (true) {
                const auto index = next_index->fetch_add(1);
                if (index >= prepared_calls->size()) {
                    break;
                }

                auto& prepared_call = (*prepared_calls)[index];
                FinalizedToolCall outcome;

                try {
                    if (prepared_call.blocked) {
                        outcome.result = std::move(prepared_call.blocked_result);
                    } else {
                        ToolInvocation invocation{
                            prepared_call.tool_call.id,
                            prepared_call.tool_call.name,
                            std::move(prepared_call.arguments),
                            prepared_call.tool_call.raw_arguments};
                        auto executed = co_await prepared_call.tool->execute(invocation);

                        if (!executed) {
                            outcome.result = error_tool_result(
                                prepared_call.tool_call,
                                executed.error().detail.empty() ? executed.error().message
                                                                : executed.error().detail);
                        } else {
                            outcome.result.tool_call_id = prepared_call.tool_call.id;
                            outcome.result.tool_name = prepared_call.tool_call.name;
                            outcome.result.content = executed->content;
                            outcome.result.details = executed->details;
                            outcome.result.is_error = executed->is_error;
                            outcome.call_terminate = executed->terminate;

                            if (!executed->is_error && after_hook) {
                                AfterToolCallContext hook_context{
                                    *assistant_snapshot,
                                    prepared_call.tool_call,
                                    invocation.arguments,
                                    *executed,
                                    false,
                                    *context_snapshot};
                                auto after_result = [&]() {
                                    std::lock_guard callback_lock(state->callback_mutex);
                                    return invoke_after_hook(*after_hook, hook_context);
                                }();
                                if (!after_result) {
                                    std::lock_guard error_lock(state->error_mutex);
                                    if (!state->fatal_error) {
                                        state->fatal_error = after_result.error();
                                    }
                                    outcome.result = error_tool_result(
                                        prepared_call.tool_call,
                                        "afterToolCall hook failed");
                                    outcome.call_terminate = false;
                                } else {
                                    apply_after_result(
                                        outcome.result,
                                        outcome.call_terminate,
                                        std::move(*after_result),
                                        executed->terminate);
                                }
                            }
                        }
                    }
                } catch (...) {
                    outcome.result = error_tool_result(
                        prepared_call.tool_call,
                        "Tool execution failed.");
                    outcome.call_terminate = false;
                }

                if (outcome.result.is_error) {
                    outcome.call_terminate = false;
                }

                const auto tool_text = ai::text_from_content(outcome.result.content);
                (void)tool_text;
                AsyncToolExecutionResult execution_result;
                execution_result.content = outcome.result.content;
                execution_result.details = outcome.result.details;
                execution_result.is_error = outcome.result.is_error;
                execution_result.terminate = outcome.call_terminate;
                (void)parallel_emit(ToolExecutionEndEvent{
                    prepared_call.tool_call.id,
                    prepared_call.tool_call.name,
                    std::move(execution_result),
                    outcome.result.is_error});
                (*finalized)[index] = std::move(outcome);
            }
        } catch (...) {
            std::lock_guard error_lock(state->error_mutex);
            if (!state->fatal_error) {
                state->fatal_error = util::make_error(
                    util::ErrorCode::Tool,
                    "bounded parallel tool worker failed");
            }
        }
        co_return;
    };

    auto worker_operation = [executor, worker_body](auto token) mutable {
        return boost::asio::co_spawn(executor, worker_body(), std::move(token));
    };
    std::vector<decltype(worker_operation)> workers(worker_count, worker_operation);
    co_await boost::asio::experimental::make_parallel_group(workers).async_wait(
        boost::asio::experimental::wait_for_all(),
        boost::asio::use_awaitable);

    {
        std::lock_guard error_lock(state->error_mutex);
        if (state->emit_error) {
            co_return std::unexpected(*state->emit_error);
        }
        if (state->fatal_error) {
            co_return std::unexpected(*state->fatal_error);
        }
    }

    for (const auto& finalized_call : *finalized) {
        CCH_TRY_VOID(emit_tool_result_message(sink, finalized_call.result));
    }

    co_return make_batch_result(std::move(*finalized));
}

} // namespace cch::agent
