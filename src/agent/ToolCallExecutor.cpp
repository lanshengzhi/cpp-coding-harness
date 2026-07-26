#include "ToolCallExecutor.hpp"

#include "ExecutionShared.hpp"
#include "ToolArgumentPreparation.hpp"
#include "util/ExpectedMacros.hpp"
#include <cch/ai/Content.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

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

    const auto calls = tool_calls_from(request.assistant_message);
    if (calls.empty()) {
        co_return ToolCallBatchResult{};
    }

    if (parallel == nullptr || parallel->max_in_flight == 1) {
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
        CCH_TRY_VOID(emit_agent_event(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

        ai::ToolResultMessage tool_result;
        bool executed_successfully = false;
        bool call_terminate = false;
        auto* tool = registry_.find(call.name);
        if (tool == nullptr) {
            tool_result = error_tool_result(
                call,
                bounded_tool_argument_diagnostic(
                    "unknown tool: " +
                    bounded_tool_argument_component(call.name, 512) +
                    " (argument location: root)"));
        } else {
            auto arguments = prepare_tool_arguments(tool->definition(), call);
            if (!arguments) {
                tool_result = error_tool_result(call, arguments.error().detail);
            } else {
                ToolInvocation invocation{call.id, call.name, std::move(*arguments), call.raw_arguments};
                bool blocked = false;

                if (options_.before_tool_call) {
                    BeforeToolCallContext hook_context{
                        request.assistant_message, call, invocation.arguments, request.context};
                    auto before_result = co_await invoke_agent_hook(
                        "beforeToolCall",
                        *options_.before_tool_call,
                        std::move(hook_context),
                        options_.stop_token);
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
                                auto after_result = co_await invoke_agent_hook(
                                    "afterToolCall",
                                    *options_.after_tool_call,
                                    std::move(hook_context),
                                    options_.stop_token);
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

        AsyncToolExecutionResult execution_result;
        execution_result.content = tool_result.content;
        execution_result.details = tool_result.details;
        execution_result.is_error = tool_result.is_error;
        execution_result.terminate = call_terminate;
        CCH_TRY_VOID(emit_agent_event(sink, ToolExecutionEndEvent{
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
        std::size_t source_index{};
        ai::ToolCallContent tool_call;
        AsyncAgentTool* tool{};
        util::JsonValue arguments;
    };

    std::vector<PreparedToolCall> prepared;
    prepared.reserve(calls.size());
    std::vector<FinalizedToolCall> finalized_calls(calls.size());

    auto complete_immediate = [&](std::size_t source_index,
                                  const ai::ToolCallContent& call,
                                  ai::ToolResultMessage result) -> util::ExpectedVoid {
        AsyncToolExecutionResult execution_result;
        execution_result.content = result.content;
        execution_result.details = result.details;
        execution_result.is_error = result.is_error;
        auto emitted = emit_agent_event(sink, ToolExecutionEndEvent{
            call.id, call.name, std::move(execution_result), result.is_error});
        if (!emitted) {
            return emitted;
        }
        finalized_calls[source_index] = FinalizedToolCall{std::move(result), false};
        return {};
    };

    for (std::size_t source_index = 0; source_index < calls.size(); ++source_index) {
        const auto& call = calls[source_index];
        CCH_TRY_VOID(emit_agent_event(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

        auto* tool = registry_.find(call.name);
        if (tool == nullptr) {
            CCH_TRY_VOID(complete_immediate(
                source_index,
                call,
                error_tool_result(
                    call,
                    bounded_tool_argument_diagnostic(
                        "unknown tool: " +
                        bounded_tool_argument_component(call.name, 512) +
                        " (argument location: root)"))));
            continue;
        }

        auto arguments = prepare_tool_arguments(tool->definition(), call);
        if (!arguments) {
            CCH_TRY_VOID(complete_immediate(
                source_index,
                call,
                error_tool_result(call, arguments.error().detail)));
            continue;
        }

        if (options_.before_tool_call) {
            BeforeToolCallContext hook_context{
                request.assistant_message, call, *arguments, request.context};
            auto before_result = co_await invoke_agent_hook(
                "beforeToolCall",
                *options_.before_tool_call,
                std::move(hook_context),
                options_.stop_token);
            if (!before_result) {
                co_return std::unexpected(before_result.error());
            }
            if (before_result->block) {
                CCH_TRY_VOID(complete_immediate(
                    source_index,
                    call,
                    error_tool_result(
                        call, before_result->reason.value_or("Tool execution was blocked"))));
                continue;
            }
        }

        prepared.push_back({source_index, call, tool, std::move(*arguments)});
    }

    struct ParallelState {
        std::mutex callback_mutex;
        std::mutex error_mutex;
        std::optional<util::Error> emit_error;
        std::optional<util::Error> fatal_error;
    };

    auto executor = co_await boost::asio::this_coro::executor;
    using HookPermitChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code)>;
    std::shared_ptr<HookPermitChannel> after_hook_permits;
    if (options_.after_tool_call) {
        after_hook_permits = std::make_shared<HookPermitChannel>(executor, 1);
        if (!after_hook_permits->try_send(boost::system::error_code{})) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Tool,
                "afterToolCall hook serialization failed"));
        }
    }

    auto state = std::make_shared<ParallelState>();
    auto prepared_calls = std::make_shared<std::vector<PreparedToolCall>>(std::move(prepared));
    auto assistant_snapshot = std::make_shared<ai::AssistantMessage>(request.assistant_message);
    auto context_snapshot = std::make_shared<ai::AiContext>(request.context);
    auto next_index = std::make_shared<std::atomic_size_t>(0);
    auto finalized = std::make_shared<std::vector<FinalizedToolCall>>(std::move(finalized_calls));

    const bool all_prepared_tools_parallel_safe = std::all_of(
        prepared_calls->begin(), prepared_calls->end(), [](const PreparedToolCall& call) {
            return call.tool->concurrency() == ToolConcurrency::ParallelSafe;
        });
    const std::size_t execution_limit =
        all_prepared_tools_parallel_safe ? max_in_flight : std::size_t{1};
    const std::size_t worker_count = std::min(execution_limit, prepared_calls->size());

    auto parallel_emit = [state, sink_ptr = &sink](const AgentLifecycleEvent& event) -> util::ExpectedVoid {
        std::lock_guard callback_lock(state->callback_mutex);
        auto result = emit_agent_event(*sink_ptr, event);
        if (!result) {
            std::lock_guard error_lock(state->error_mutex);
            if (!state->emit_error) {
                state->emit_error = result.error();
            }
        }
        return result;
    };

    AfterToolCallHook* const after_hook = options_.after_tool_call;
    const std::stop_token stop_token = options_.stop_token;

    auto worker_body = [after_hook,
                        after_hook_permits,
                        stop_token,
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
                            const auto [permit_error] = co_await after_hook_permits->async_receive(
                                boost::asio::as_tuple(boost::asio::use_awaitable));
                            if (permit_error) {
                                std::lock_guard error_lock(state->error_mutex);
                                if (!state->fatal_error) {
                                    state->fatal_error = util::make_error(
                                        util::ErrorCode::Tool,
                                        "afterToolCall hook serialization failed",
                                        permit_error.message());
                                }
                                outcome.result = error_tool_result(
                                    prepared_call.tool_call,
                                    "afterToolCall hook failed");
                                outcome.call_terminate = false;
                            } else {
                                AfterToolCallContext hook_context{
                                    *assistant_snapshot,
                                    prepared_call.tool_call,
                                    invocation.arguments,
                                    *executed,
                                    false,
                                    *context_snapshot};
                                auto after_result = co_await invoke_agent_hook(
                                    "afterToolCall",
                                    *after_hook,
                                    std::move(hook_context),
                                    stop_token);
                                const bool released = after_hook_permits->try_send(
                                    boost::system::error_code{});
                                if (!released) {
                                    std::lock_guard error_lock(state->error_mutex);
                                    if (!state->fatal_error) {
                                        state->fatal_error = util::make_error(
                                            util::ErrorCode::Tool,
                                            "afterToolCall hook serialization failed");
                                    }
                                    outcome.result = error_tool_result(
                                        prepared_call.tool_call,
                                        "afterToolCall hook failed");
                                    outcome.call_terminate = false;
                                } else if (!after_result) {
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
                (*finalized)[prepared_call.source_index] = std::move(outcome);
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
    if (worker_count > 0) {
        std::vector<decltype(worker_operation)> workers(worker_count, worker_operation);
        co_await boost::asio::experimental::make_parallel_group(workers).async_wait(
            boost::asio::experimental::wait_for_all(),
            boost::asio::use_awaitable);
    }

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
