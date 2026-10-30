#include "RunToolCallImpl.hpp"

#include "ExecutionShared.hpp"
#include "ToolArgumentDetail.hpp"
#include "ToolArgumentPreparation.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Content.hpp>

#include <boost/asio/co_spawn.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace cch::agent {
namespace {

[[nodiscard]] AgentToolCallOutcome make_aborted_outcome(ai::ToolCallContent tool_call) {
    return AgentToolCallOutcome{
            .tool_call = std::move(tool_call),
            .result =
                    AsyncToolExecutionResult{
                            .content = {ai::text_content(std::string{kOperationAbortedMessage})},
                            .details = std::nullopt,
                            .is_error = true,
                            .terminate = false,
                    },
            .is_error = true,
            .duration_ms = std::nullopt,
    };
}

} // namespace

boost::asio::awaitable<AgentToolCallOutcome> run_tool_call_coro(
        ai::ToolCallContent tool_call, RunToolCallOptions options) {
    if (options.stop_token.stop_requested()) {
        co_return make_aborted_outcome(std::move(tool_call));
    }

    Tool* tool = options.tools.get().find(tool_call.name);
    if (tool == nullptr) {
        AsyncToolExecutionResult not_found_res{
                .content = {ai::text_content("Tool " + tool_call.name + " not found")},
                .details = std::nullopt,
                .is_error = true,
                .terminate = false,
        };
        co_return AgentToolCallOutcome{
                .tool_call = std::move(tool_call),
                .result = std::move(not_found_res),
                .is_error = true,
                .duration_ms = std::nullopt,
        };
    }

    auto arguments = prepare_tool_arguments(*tool, tool_call);
    if (!arguments) {
        std::string err_text = arguments.error().detail.empty() ? arguments.error().message : arguments.error().detail;
        AsyncToolExecutionResult prep_err_res{
                .content = {ai::text_content(std::move(err_text))},
                .details = std::nullopt,
                .is_error = true,
                .terminate = false,
        };
        co_return AgentToolCallOutcome{
                .tool_call = std::move(tool_call),
                .result = std::move(prep_err_res),
                .is_error = true,
                .duration_ms = std::nullopt,
        };
    }

    if (options.before_tool_call != nullptr) {
        BeforeToolCallContext hook_context{
                .assistant_message = options.assistant_message,
                .tool_call = tool_call,
                .args = *arguments,
                .context = options.context,
        };
        auto before_result = co_await invoke_agent_hook(
                "beforeToolCall", *options.before_tool_call, std::move(hook_context), options.stop_token);
        if (options.stop_token.stop_requested()) {
            co_return make_aborted_outcome(std::move(tool_call));
        }
        if (!before_result) {
            AsyncToolExecutionResult hook_err_res{
                    .content = {ai::text_content(bounded_failure_text(before_result.error()))},
                    .details = std::nullopt,
                    .is_error = true,
                    .terminate = false,
            };
            co_return AgentToolCallOutcome{
                    .tool_call = std::move(tool_call),
                    .result = std::move(hook_err_res),
                    .is_error = true,
                    .duration_ms = std::nullopt,
            };
        }
        if (before_result->block) {
            AsyncToolExecutionResult blocked_res{
                    .content = {ai::text_content(before_result->reason.value_or("Tool execution was blocked"))},
                    .details = std::nullopt,
                    .is_error = true,
                    .terminate = before_result->terminate.value_or(false),
            };
            co_return AgentToolCallOutcome{
                    .tool_call = std::move(tool_call),
                    .result = std::move(blocked_res),
                    .is_error = true,
                    .duration_ms = std::nullopt,
            };
        }
        if (before_result->args.has_value()) {
            *arguments = std::move(*before_result->args);
        }
    }

    if (options.stop_token.stop_requested()) {
        co_return make_aborted_outcome(std::move(tool_call));
    }

    ToolInvocation invocation{
            .call_id = tool_call.id,
            .name = tool_call.name,
            .arguments = std::move(*arguments),
            .raw_arguments = tool_call.raw_arguments,
            .nested_calls = options.nested_calls,
    };

    auto update_gate = std::make_shared<ToolUpdateGate>();
    ToolUpdateSink update_sink;
    if (options.on_update) {
        update_sink = [on_update = std::move(options.on_update), update_gate](
                              const AsyncToolExecutionResult& partial_result) mutable -> support::ExpectedVoid {
            std::lock_guard lock(update_gate->mutex);
            if (!update_gate->active) {
                return support::ExpectedVoid{};
            }
            return on_update(partial_result);
        };
    }

    auto timed = co_await execute_with_update_lifetime(
            *tool, invocation, options.stop_token, std::move(update_sink), update_gate);
    const auto duration_ms = timed.duration_ms;
    auto& executed = timed.result;

    AsyncToolExecutionResult outcome;
    if (!executed) {
        outcome = AsyncToolExecutionResult{
                .content = {ai::text_content(bounded_failure_text(executed.error()))},
                .details = std::nullopt,
                .is_error = true,
                .terminate = false,
        };
    } else {
        outcome = std::move(*executed);
    }

    if (options.after_tool_call != nullptr) {
        AfterToolCallContext hook_context{
                .assistant_message = options.assistant_message,
                .tool_call = tool_call,
                .args = invocation.arguments,
                .result = outcome,
                .is_error = outcome.is_error,
                .context = options.context,
        };
        auto after_result = co_await invoke_agent_hook(
                "afterToolCall", *options.after_tool_call, std::move(hook_context), options.stop_token);
        if (!after_result) {
            outcome = AsyncToolExecutionResult{
                    .content = {ai::text_content(bounded_failure_text(after_result.error()))},
                    .details = std::nullopt,
                    .is_error = true,
                    .terminate = false,
            };
        } else {
            if (after_result->content) {
                outcome.content = std::move(*after_result->content);
            }
            if (after_result->details) {
                outcome.details = std::move(*after_result->details);
            }
            if (after_result->is_error) {
                outcome.is_error = *after_result->is_error;
            }
            if (after_result->terminate) {
                outcome.terminate = *after_result->terminate;
            }
        }
    }

    co_return AgentToolCallOutcome{
            .tool_call = std::move(tool_call),
            .result = std::move(outcome),
            .is_error = outcome.is_error,
            .duration_ms = duration_ms,
    };
}

support::AsyncResult<AgentToolCallOutcome> run_tool_call(ai::ToolCallContent tool_call, RunToolCallOptions options) {
    return support::detail::make_async_result(
            [tool_call = std::move(tool_call), options = std::move(options)]() mutable
                    -> boost::asio::awaitable<support::Expected<AgentToolCallOutcome>> {
                co_return co_await run_tool_call_coro(std::move(tool_call), std::move(options));
            });
}

} // namespace cch::agent
