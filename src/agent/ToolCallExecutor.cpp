#include "ToolCallExecutor.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/util/Error.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <cstring>
#include <utility>
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

[[nodiscard]] util::Expected<util::JsonValue> arguments_for_call(const ai::ToolCallContent& call) {
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

void erase_first(std::vector<std::string>& values, const std::string& value) {
    const auto found = std::find(values.begin(), values.end(), value);
    if (found != values.end()) {
        values.erase(found);
    }
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

util::ExpectedVoid emit(AgentEventSink& sink, const AgentLifecycleEvent& event) {
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

struct FinalizedToolCall {
    ai::ToolCallContent tool_call;
    ai::ToolResultMessage result;
    bool is_error{false};
    bool call_terminate{false};
};

} // namespace

ToolCallExecutor::ToolCallExecutor(const AsyncToolRegistry& registry, ToolCallExecutorOptions options)
    : registry_(registry), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute(
    int turn,
    const ai::AssistantMessage& assistant_message,
    const std::vector<ai::ToolCallContent>& calls,
    ai::AiContext& context,
    AgentState& state,
    AgentEventSink& sink) {
    const bool has_sequential_tool = std::any_of(
        calls.begin(), calls.end(), [this](const ai::ToolCallContent& call) {
            auto* tool = registry_.find(call.name);
            return tool == nullptr || tool->execution_mode() == ai::ToolExecutionMode::Sequential;
        });

    const bool use_sequential = options_.mode == ai::ToolExecutionMode::Sequential || has_sequential_tool ||
                                calls.size() > options_.max_parallel_tools;

    if (use_sequential) {
        co_return co_await execute_sequential(turn, assistant_message, calls, context, state, sink);
    }
    co_return co_await execute_parallel(turn, assistant_message, calls, context, state, sink);
}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_sequential(
    int turn,
    const ai::AssistantMessage& assistant_message,
    const std::vector<ai::ToolCallContent>& calls,
    ai::AiContext& context,
    AgentState& state,
    AgentEventSink& sink) {
    std::vector<ai::ToolResultMessage> messages;
    std::vector<FinalizedToolCall> finalized_calls;

    for (const auto& call : calls) {
        state.active_tool_names.push_back(call.name);
        CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{turn, call.id, call.name}));

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
                    BeforeToolCallContext hook_ctx{assistant_message, call, invocation.arguments, context};
                    auto before_result = invoke_before_hook(*options_.before_tool_call, hook_ctx);
                    if (!before_result) {
                        CCH_TRY_VOID(emit(sink, AgentEndEvent{false, before_result.error().message}));
                        co_return std::unexpected(before_result.error());
                    }
                    if (before_result->block) {
                        blocked = true;
                        tool_result = error_tool_result(
                            call, before_result->reason.value_or("Tool execution was blocked"));
                    }
                }

                if (!blocked) {
                    auto executed = co_await tool->execute(invocation);
                    if (!executed) {
                        tool_result = error_tool_result(
                            call,
                            executed.error().detail.empty() ? executed.error().message
                                                            : executed.error().detail);
                    } else {
                        executed_successfully = true;
                        tool_result.tool_call_id = call.id;
                        tool_result.tool_name = call.name;
                        tool_result.content = executed->content;
                        tool_result.details = executed->details;
                        tool_result.is_error = executed->is_error;

                        if (executed->is_error) {
                        } else if (options_.after_tool_call) {
                            AfterToolCallContext hook_ctx{
                                assistant_message, call, invocation.arguments, *executed, false, context};
                            auto after_result = invoke_after_hook(*options_.after_tool_call, hook_ctx);
                            if (!after_result) {
                                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, after_result.error().message}));
                                co_return std::unexpected(after_result.error());
                            }
                            if (after_result->content) {
                                tool_result.content = std::move(*after_result->content);
                            }
                            if (after_result->details) {
                                tool_result.details = std::move(*after_result->details);
                            }
                            if (after_result->is_error) {
                                tool_result.is_error = *after_result->is_error;
                            }
                            call_terminate = after_result->terminate.value_or(executed->terminate);
                        } else {
                            call_terminate = executed->terminate;
                        }
                    }
                }
            }
        }

        if (executed_successfully) {
            if (tool_result.is_error) {
                call_terminate = false;
            }
        } else {
            call_terminate = false;
        }

        const auto tool_text = ai::text_from_content(tool_result.content);
        CCH_TRY_VOID(emit(sink, ToolExecutionEndEvent{turn, call.id, call.name, tool_result.is_error, tool_text}));
        erase_first(state.active_tool_names, call.name);
        erase_first(state.pending_tool_call_ids, call.id);
        messages.push_back(tool_result);
        finalized_calls.push_back(FinalizedToolCall{call, std::move(tool_result), false, call_terminate});
    }

    const bool terminate = !finalized_calls.empty() &&
                           std::all_of(finalized_calls.begin(), finalized_calls.end(), [](const FinalizedToolCall& f) {
                               return f.call_terminate;
                           });
    co_return ToolCallBatchResult{std::move(messages), terminate};
}

boost::asio::awaitable<util::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_parallel(
    int turn,
    const ai::AssistantMessage& assistant_message,
    const std::vector<ai::ToolCallContent>& calls,
    ai::AiContext& context,
    AgentState& state,
    AgentEventSink& sink) {
    struct PreparedToolCall {
        ai::ToolCallContent tool_call;
        agent::AsyncAgentTool* tool;
        util::JsonValue arguments;
        bool blocked{false};
        ai::ToolResultMessage blocked_result;
    };

    std::vector<PreparedToolCall> prepared;
    prepared.reserve(calls.size());

    for (const auto& call : calls) {
        state.active_tool_names.push_back(call.name);
        CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{turn, call.id, call.name}));

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
            BeforeToolCallContext hook_ctx{assistant_message, call, *arguments, context};
            auto before_result = invoke_before_hook(*options_.before_tool_call, hook_ctx);
            if (!before_result) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, before_result.error().message}));
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
        std::mutex emit_mutex;
        std::mutex hook_mutex;
        std::mutex error_mutex;
        std::optional<util::Error> emit_error;
        std::optional<util::Error> fatal_error;
    };

    struct SinkHandle {
        std::mutex mutex;
        AgentEventSink* sink{nullptr};
        bool active{true};
    };

    struct SinkGuard {
        std::shared_ptr<SinkHandle> handle;
        ~SinkGuard() {
            std::lock_guard lock(handle->mutex);
            handle->active = false;
            handle->sink = nullptr;
        }
    };

    auto exec = co_await boost::asio::this_coro::executor;
    auto parallel_state = std::make_shared<ParallelState>();
    auto sink_handle = std::make_shared<SinkHandle>();
    sink_handle->sink = &sink;
    SinkGuard sink_guard{sink_handle};

    auto parallel_emit = [parallel_state, sink_handle](const AgentLifecycleEvent& event) -> util::ExpectedVoid {
        std::lock_guard emit_lock(parallel_state->emit_mutex);
        util::ExpectedVoid result;
        {
            std::lock_guard sink_lock(sink_handle->mutex);
            if (!sink_handle->active || sink_handle->sink == nullptr) {
                return {};
            }
            result = emit(*sink_handle->sink, event);
        }
        if (!result) {
            std::lock_guard error_lock(parallel_state->error_mutex);
            if (!parallel_state->emit_error) {
                parallel_state->emit_error = result.error();
            }
        }
        return result;
    };

    AfterToolCallHook* after_hook = options_.after_tool_call ? &*options_.after_tool_call : nullptr;

    auto prepared_calls = std::make_shared<std::vector<PreparedToolCall>>(std::move(prepared));
    auto assistant_snapshot = std::make_shared<ai::AssistantMessage>(assistant_message);
    auto context_snapshot = std::make_shared<ai::AiContext>(context);
    auto completed = std::make_shared<boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>>(
        exec, prepared_calls->size());

    struct CompletionState {
        std::mutex mutex;
        std::vector<FinalizedToolCall> finalized;
    };

    auto completion = std::make_shared<CompletionState>();
    completion->finalized.resize(prepared_calls->size());

    for (std::size_t index = 0; index < prepared_calls->size(); ++index) {
        boost::asio::co_spawn(
            exec,
            [after_hook, prepared_calls, assistant_snapshot, context_snapshot, parallel_emit, parallel_state, completion, completed, index, turn]()
                -> boost::asio::awaitable<void> {
                auto& prep = (*prepared_calls)[index];
                FinalizedToolCall finalized;
                finalized.tool_call = prep.tool_call;

                try {
                    if (prep.blocked) {
                        finalized.result = std::move(prep.blocked_result);
                        finalized.is_error = true;
                        finalized.call_terminate = false;
                    } else {
                        ToolInvocation invocation{
                            prep.tool_call.id, prep.tool_call.name, std::move(prep.arguments), prep.tool_call.raw_arguments};
                        auto executed = co_await prep.tool->execute(invocation);

                        if (!executed) {
                            finalized.result = error_tool_result(
                                prep.tool_call,
                                executed.error().detail.empty() ? executed.error().message : executed.error().detail);
                            finalized.is_error = true;
                            finalized.call_terminate = false;
                        } else {
                            finalized.result.tool_call_id = prep.tool_call.id;
                            finalized.result.tool_name = prep.tool_call.name;
                            finalized.result.content = executed->content;
                            finalized.result.details = executed->details;
                            finalized.result.is_error = executed->is_error;

                            if (executed->is_error) {
                                finalized.is_error = true;
                                finalized.call_terminate = false;
                            } else if (after_hook) {
                                AfterToolCallContext hook_ctx{
                                    *assistant_snapshot, prep.tool_call, invocation.arguments, *executed, false, *context_snapshot};
                                auto after_result = [&]() {
                                    std::lock_guard lock(parallel_state->hook_mutex);
                                    return invoke_after_hook(*after_hook, hook_ctx);
                                }();
                                if (!after_result) {
                                    {
                                        std::lock_guard lock(parallel_state->error_mutex);
                                        if (!parallel_state->fatal_error) {
                                            parallel_state->fatal_error = after_result.error();
                                        }
                                    }
                                    finalized.result = error_tool_result(prep.tool_call, after_result.error().message);
                                    finalized.is_error = true;
                                    finalized.call_terminate = false;
                                } else {
                                    if (after_result->content) {
                                        finalized.result.content = std::move(*after_result->content);
                                    }
                                    if (after_result->details) {
                                        finalized.result.details = std::move(*after_result->details);
                                    }
                                    if (after_result->is_error) {
                                        finalized.result.is_error = *after_result->is_error;
                                    }
                                    finalized.is_error = finalized.result.is_error;
                                    finalized.call_terminate = after_result->terminate.value_or(executed->terminate);
                                }
                            } else {
                                finalized.is_error = executed->is_error;
                                finalized.call_terminate = executed->terminate;
                            }
                        }
                    }

                    if (finalized.result.is_error) {
                        finalized.is_error = true;
                        finalized.call_terminate = false;
                    }
                    const auto tool_text = ai::text_from_content(finalized.result.content);
                    (void)parallel_emit(ToolExecutionEndEvent{
                        turn, prep.tool_call.id, prep.tool_call.name, finalized.is_error, tool_text});
                } catch (const std::exception& e) {
                    finalized.result = error_tool_result(prep.tool_call, e.what());
                    finalized.is_error = true;
                    finalized.call_terminate = false;
                    const auto tool_text = ai::text_from_content(finalized.result.content);
                    (void)parallel_emit(ToolExecutionEndEvent{
                        turn, prep.tool_call.id, prep.tool_call.name, finalized.is_error, tool_text});
                } catch (...) {
                    finalized.result = error_tool_result(prep.tool_call, "unknown exception");
                    finalized.is_error = true;
                    finalized.call_terminate = false;
                    const auto tool_text = ai::text_from_content(finalized.result.content);
                    (void)parallel_emit(ToolExecutionEndEvent{
                        turn, prep.tool_call.id, prep.tool_call.name, finalized.is_error, tool_text});
                }

                {
                    std::lock_guard lock(completion->mutex);
                    completion->finalized[index] = std::move(finalized);
                }
                completed->try_send(boost::system::error_code{});
                co_return;
            },
            boost::asio::detached);
    }

    for (std::size_t index = 0; index < prepared_calls->size(); ++index) {
        boost::system::error_code ec;
        co_await completed->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Tool,
                "parallel tool execution failed",
                ec.message()));
        }
    }

    {
        std::lock_guard lock(parallel_state->error_mutex);
        if (parallel_state->emit_error) {
            co_return std::unexpected(*parallel_state->emit_error);
        }
        if (parallel_state->fatal_error) {
            co_return std::unexpected(*parallel_state->fatal_error);
        }
    }

    std::vector<FinalizedToolCall> finalized;
    finalized.reserve(prepared_calls->size());
    for (auto& f : completion->finalized) {
        finalized.push_back(std::move(f));
    }

    std::vector<ai::ToolResultMessage> messages;
    messages.reserve(finalized.size());
    for (auto& f : finalized) {
        erase_first(state.active_tool_names, f.tool_call.name);
        erase_first(state.pending_tool_call_ids, f.tool_call.id);
        messages.push_back(std::move(f.result));
    }

    const bool terminate = !finalized.empty() &&
                           std::all_of(finalized.begin(), finalized.end(), [](const FinalizedToolCall& f) {
                               return f.call_terminate;
                           });
    co_return ToolCallBatchResult{std::move(messages), terminate};
}

} // namespace cch::agent
