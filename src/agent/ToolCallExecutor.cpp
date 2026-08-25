#include "ToolCallExecutor.hpp"

#include "ExecutionShared.hpp"
#include "ToolArgumentPreparation.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/BoundedText.hpp"
#include "support/ExpectedMacros.hpp"
#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>

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
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

struct ToolUpdateGate {
    std::mutex mutex;
    bool active{true};
};

void close_tool_updates(const std::shared_ptr<ToolUpdateGate>& gate) {
    std::lock_guard lock(gate->mutex);
    gate->active = false;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<AsyncToolExecutionResult>>
execute_with_update_lifetime(
    Tool& tool,
    const ToolInvocation& invocation,
    std::stop_token stop_token,
    ToolUpdateSink update_sink,
    const std::shared_ptr<ToolUpdateGate>& gate) {
    auto result =
            co_await support::detail::await_async_result(tool.execute(invocation, stop_token, std::move(update_sink)));
    close_tool_updates(gate);
    co_return result;
}

constexpr std::string_view kOperationAbortedMessage{"Operation aborted"};

[[nodiscard]] ai::ToolResultMessage error_tool_result(
    const ai::ToolCallContent& call,
    std::string message) {
    ai::ToolResultMessage result;
    result.tool_call_id = call.id;
    result.tool_name = call.name;
    result.content.emplace_back(ai::TextContent{
        .text = std::move(message),
        .text_signature = std::nullopt,
    });
    result.is_error = true;
    return result;
}

[[nodiscard]] AsyncToolExecutionResult execution_result_from(
    const ai::ToolResultMessage& tool_result,
    bool terminate = false) {
    return AsyncToolExecutionResult{
        .content = tool_result.content,
        .details = tool_result.details,
        .is_error = tool_result.is_error,
        .terminate = terminate,
    };
}

/// Build the aborted outcome for a call that never emitted a
/// ToolExecutionStartEvent. No ToolExecutionEndEvent is emitted here:
/// lifecycle events stay strictly paired, so an unstarted call produces only
/// the aborted tool result.
[[nodiscard]] FinalizedToolCallResult abort_tool_call(const ai::ToolCallContent& call) {
    return FinalizedToolCallResult{
        .result = error_tool_result(call, std::string{kOperationAbortedMessage}),
        .call_terminate = false,
    };
}

constexpr std::size_t kMaxToolFailureDiagnosticBytes = 4096;

[[nodiscard]] std::string bounded_failure_text(const support::Error& error) {
    std::string diagnostic = error.detail.empty() ? error.message : error.detail;
    return support::bounded_redacted_text(std::move(diagnostic), kMaxToolFailureDiagnosticBytes, "...");
}

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
    std::vector<FinalizedToolCallResult> finalized) {
    const bool terminate = !finalized.empty() &&
                           std::all_of(finalized.begin(), finalized.end(), [](const FinalizedToolCallResult& call) {
                               return call.call_terminate;
                           });

    std::vector<ai::ToolResultMessage> results;
    results.reserve(finalized.size());
    for (auto& call : finalized) {
        results.push_back(std::move(call.result));
    }
    return ToolCallBatchResult{
        .results = std::move(results),
        .terminate_batch = terminate,
    };
}

template <typename Hook, typename Context, typename Result>
[[nodiscard]] boost::asio::awaitable<support::Expected<Result>> invoke_serialized_hook(
    std::string_view name,
    Hook& hook,                        // non-owning; must outlive coroutine
    Context context,
    std::stop_token stop_token,
    ExecutionPermitChannel* permits) { // non-owning; must outlive coroutine
    if (permits != nullptr) {
        const auto [permit_error] = co_await permits->async_receive(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (permit_error) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Tool,
                std::format("{} hook serialization failed", name),
                permit_error.message()));
        }
        auto result = co_await invoke_agent_hook(
            name,
            hook,
            std::move(context),
            stop_token);
        if (!permits->try_send(boost::system::error_code{})) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Tool,
                std::format("{} hook serialization release failed", name)));
        }
        co_return result;
    }
    co_return co_await invoke_agent_hook(
        name,
        hook,
        std::move(context),
        stop_token);
}

template <typename Executor>
[[nodiscard]] support::Expected<std::unique_ptr<ExecutionPermitChannel>> make_permit_channel(
    const Executor& executor,
    std::size_t capacity,
    std::string_view failure_message) {
    auto channel = std::make_unique<ExecutionPermitChannel>(executor, capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        if (!channel->try_send(boost::system::error_code{})) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Tool,
                std::string(failure_message)));
        }
    }
    return channel;
}

} // namespace

ToolCallExecutor::ToolCallExecutor(
    ToolRegistry& registry,
    ToolCallExecutorOptions options)
    : registry_(registry), options_(std::move(options)) {}

boost::asio::awaitable<support::Expected<FinalizedToolCallResult>> ToolCallExecutor::execute_single_call(
    ToolCallBatchRequest request,
    const ai::ToolCallContent& call,
    AgentEventSink& sink,
    ToolExecutionPermits permits) {
    if (permits.preparation != nullptr) {
        const auto [permit_error] = co_await permits.preparation->async_receive(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (permit_error) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Tool,
                "tool call preparation serialization failed",
                permit_error.message()));
        }
    }

    auto release_preparation = [&]() {
        if (permits.preparation != nullptr) {
            (void)permits.preparation->try_send(boost::system::error_code{});
            permits.preparation = nullptr;
        }
    };

    if (options_.stop_token.stop_requested()) {
        release_preparation();
        co_return abort_tool_call(call);
    }

    if (auto start_emit = emit_agent_event(
            sink,
            ToolExecutionStartEvent{
                .tool_call_id = call.id,
                .tool_name = call.name,
                .args = call.arguments.value_or(support::JsonValue{}),
            });
        !start_emit) {
        release_preparation();
        co_return std::unexpected(start_emit.error());
    }

    ai::ToolResultMessage tool_result;
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
        auto arguments = prepare_tool_arguments(tool->definition, call);
        if (!arguments) {
            tool_result = error_tool_result(call, arguments.error().detail);
        } else {
            ToolInvocation invocation{
                .call_id = call.id,
                .name = call.name,
                .arguments = std::move(*arguments),
                .raw_arguments = call.raw_arguments,
            };
            bool blocked = false;

            if (options_.before_tool_call) {
                BeforeToolCallContext hook_context{
                    .assistant_message = request.assistant_message,
                    .tool_call = call,
                    .args = invocation.arguments,
                    .context = request.context,
                };
                auto before_result = co_await invoke_agent_hook(
                    "beforeToolCall",
                    *options_.before_tool_call,
                    std::move(hook_context),
                    options_.stop_token);
                if (!before_result) {
                    blocked = true;
                    tool_result = error_tool_result(
                        call, bounded_failure_text(before_result.error()));
                } else if (before_result->block) {
                    blocked = true;
                    tool_result = error_tool_result(
                        call, before_result->reason.value_or("Tool execution was blocked"));
                }
            }

            if (!blocked && options_.stop_token.stop_requested()) {
                blocked = true;
                tool_result = error_tool_result(call, std::string{kOperationAbortedMessage});
            }

            if (!blocked) {
                release_preparation();

                if (permits.concurrency != nullptr) {
                    const auto [permit_error] = co_await permits.concurrency->async_receive(
                        boost::asio::as_tuple(boost::asio::use_awaitable));
                    if (permit_error) {
                        auto error = support::make_error(
                            support::ErrorCode::Tool,
                            "tool concurrency permit acquisition failed",
                            permit_error.message());
                        // A start event was already dispatched for this call;
                        // close the lifecycle pair before failing the batch.
                        (void)emit_agent_event(
                            sink,
                            ToolExecutionEndEvent{
                                .tool_call_id = call.id,
                                .tool_name = call.name,
                                .result = execution_result_from(
                                    error_tool_result(call, error.message)),
                                .is_error = true,
                            });
                        co_return std::unexpected(std::move(error));
                    }
                }

                if (options_.stop_token.stop_requested()) {
                    if (permits.concurrency != nullptr) {
                        (void)permits.concurrency->try_send(boost::system::error_code{});
                    }
                    tool_result = error_tool_result(call, std::string{kOperationAbortedMessage});
                    call_terminate = false;
                } else {
                    AsyncToolExecutionResult outcome;
                    auto update_gate = std::make_shared<ToolUpdateGate>();
                    ToolUpdateSink update_sink = [
                        &sink,
                        update_gate,
                        call_id = call.id,
                        tool_name = call.name,
                        args = call.arguments.value_or(support::JsonValue{})](
                            const AsyncToolExecutionResult& partial_result) {
                        std::lock_guard lock(update_gate->mutex);
                        if (!update_gate->active) {
                            return support::ExpectedVoid{};
                        }
                        return emit_agent_event(sink, ToolExecutionUpdateEvent{
                            .tool_call_id = call_id,
                            .tool_name = tool_name,
                            .args = args,
                            .partial_result = partial_result,
                        });
                    };
                    auto executed = co_await execute_with_update_lifetime(
                        *tool,
                        invocation,
                        options_.stop_token,
                        std::move(update_sink),
                        update_gate);

                    if (permits.concurrency != nullptr) {
                        (void)permits.concurrency->try_send(boost::system::error_code{});
                    }

                    if (!executed) {
                        outcome = AsyncToolExecutionResult{
                            .content = std::vector<ai::Content>{ai::text_content(
                                bounded_failure_text(executed.error()))},
                            .details = std::nullopt,
                            .is_error = true,
                            .terminate = false,
                        };
                    } else {
                        outcome = std::move(*executed);
                    }

                    tool_result.tool_call_id = call.id;
                    tool_result.tool_name = call.name;
                    tool_result.content = outcome.content;
                    tool_result.details = outcome.details;
                    tool_result.is_error = outcome.is_error;
                    call_terminate = outcome.terminate;

                    if (options_.after_tool_call) {
                        AfterToolCallContext hook_context{
                            .assistant_message = request.assistant_message,
                            .tool_call = call,
                            .args = invocation.arguments,
                            .result = outcome,
                            .is_error = outcome.is_error,
                            .context = request.context,
                        };
                        auto after_result = co_await invoke_serialized_hook<
                            AfterToolCallHook,
                            AfterToolCallContext,
                            AfterToolCallResult>(
                            "afterToolCall",
                            *options_.after_tool_call,
                            std::move(hook_context),
                            options_.stop_token,
                            permits.after_hook);
                        if (!after_result) {
                            tool_result = error_tool_result(
                                call, bounded_failure_text(after_result.error()));
                            call_terminate = false;
                        } else {
                            apply_after_result(
                                tool_result,
                                call_terminate,
                                std::move(*after_result),
                                outcome.terminate);
                        }
                    }
                }
            }
        }
    }

    auto end_emit = emit_agent_event(
        sink,
        ToolExecutionEndEvent{
            .tool_call_id = call.id,
            .tool_name = call.name,
            .result = execution_result_from(tool_result, call_terminate),
            .is_error = tool_result.is_error,
        });
    release_preparation();
    if (!end_emit) {
        co_return std::unexpected(end_emit.error());
    }

    co_return FinalizedToolCallResult{
        .result = std::move(tool_result),
        .call_terminate = call_terminate,
    };
}

boost::asio::awaitable<support::Expected<ToolCallBatchResult>> ToolCallExecutor::execute(
    ToolCallBatchRequest request,
    AgentEventSink& sink) {
    const auto calls = tool_calls_from(request.assistant_message);
    if (calls.empty()) {
        co_return ToolCallBatchResult{};
    }

    const auto* parallel = std::get_if<BoundedParallelToolExecution>(&options_.execution);

    const bool any_sequential_tool = std::any_of(
        calls.begin(), calls.end(), [&](const ai::ToolCallContent& call) {
            const auto* tool = registry_.find(call.name);
            return tool != nullptr && tool->concurrency == ToolConcurrency::Exclusive;
        });
    if (parallel == nullptr || any_sequential_tool || parallel->max_in_flight == 1) {
        co_return co_await execute_sequential(request, calls, sink);
    }

    co_return co_await execute_parallel(request, calls, parallel->max_in_flight, sink);
}

boost::asio::awaitable<support::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_sequential(
    ToolCallBatchRequest request,
    const std::vector<ai::ToolCallContent>& calls,
    AgentEventSink& sink) {
    std::vector<FinalizedToolCallResult> finalized;
    finalized.reserve(calls.size());

    for (const auto& call : calls) {
        if (options_.stop_token.stop_requested()) {
            break;
        }
        auto call_result = co_await execute_single_call(
            request,
            call,
            sink,
            ToolExecutionPermits{});
        if (!call_result) {
            co_return std::unexpected(call_result.error());
        }
        CCH_TRY_VOID(emit_tool_result_message(sink, call_result->result));
        finalized.push_back(std::move(*call_result));
        if (options_.stop_token.stop_requested()) {
            break;
        }
    }

    co_return make_batch_result(std::move(finalized));
}

struct ParallelExecutionState {
    std::mutex callback_mutex;
    std::mutex error_mutex;
    std::optional<support::Error> emit_error;
    std::optional<support::Error> fatal_error;
    std::vector<std::optional<FinalizedToolCallResult>> finalized;

    explicit ParallelExecutionState(std::size_t size) : finalized(size) {}
};

boost::asio::awaitable<void> ToolCallExecutor::execute_parallel_task(
    ToolCallBatchRequest request,
    const ai::ToolCallContent& call,
    std::size_t index,
    std::shared_ptr<ParallelExecutionState> state,
    AgentEventSink parallel_sink,
    ToolExecutionPermits permits) {
    auto call_result = co_await execute_single_call(
        request,
        call,
        parallel_sink,
        permits);

    if (!call_result) {
        std::lock_guard error_lock(state->error_mutex);
        if (!state->fatal_error) {
            state->fatal_error = call_result.error();
        }
        co_return;
    }

    state->finalized[index] = std::move(*call_result);
}

boost::asio::awaitable<support::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_parallel(
    ToolCallBatchRequest request,
    const std::vector<ai::ToolCallContent>& calls,
    std::size_t max_in_flight,
    AgentEventSink& sink) {
    auto executor = co_await boost::asio::this_coro::executor;

    auto preparation_permits = make_permit_channel(
        executor, 1, "tool preparation serialization failed");
    if (!preparation_permits) {
        co_return std::unexpected(preparation_permits.error());
    }

    const std::size_t execution_limit =
        max_in_flight == 0 ? calls.size() : max_in_flight;

    std::unique_ptr<ExecutionPermitChannel> concurrency_permits;
    if (execution_limit > 0) {
        auto channel = make_permit_channel(
            executor, execution_limit, "tool concurrency permit initialization failed");
        if (!channel) {
            co_return std::unexpected(channel.error());
        }
        concurrency_permits = std::move(*channel);
    }

    std::unique_ptr<ExecutionPermitChannel> after_hook_permits;
    if (options_.after_tool_call) {
        auto channel = make_permit_channel(
            executor, 1, "afterToolCall hook serialization failed");
        if (!channel) {
            co_return std::unexpected(channel.error());
        }
        after_hook_permits = std::move(*channel);
    }

    auto state = std::make_shared<ParallelExecutionState>(calls.size());

    auto parallel_emit = [state, sink_ptr = &sink](
        const AgentLifecycleEvent& event) -> support::ExpectedVoid {
        std::lock_guard callback_lock(state->callback_mutex);
        auto result = emit_agent_event(*sink_ptr, event);
        if (!result && !std::holds_alternative<ToolExecutionUpdateEvent>(event)) {
            std::lock_guard error_lock(state->error_mutex);
            if (!state->emit_error) {
                state->emit_error = result.error();
            }
        }
        return result;
    };

    ToolExecutionPermits permits{
        .preparation = preparation_permits->get(),
        .concurrency = concurrency_permits.get(),
        .after_hook = after_hook_permits.get(),
    };

    auto make_task = [this,
                      call_request = ToolCallBatchRequest{
                          .assistant_message = request.assistant_message,
                          .context = request.context,
                      },
                      &calls,
                      state,
                      parallel_emit,
                      permits,
                      executor](std::size_t index) {
        return [this,
                call_request,
                &call = calls[index],
                index,
                state,
                parallel_emit,
                permits,
                executor](auto token) mutable {
            AgentEventSink parallel_sink{parallel_emit};
            return boost::asio::co_spawn(
                executor,
                execute_parallel_task(
                    call_request,
                    call,
                    index,
                    state,
                    std::move(parallel_sink),
                    permits),
                std::move(token));
        };
    };

    if (!calls.empty()) {
        using TaskOp = decltype(make_task(0));
        std::vector<TaskOp> tasks;
        tasks.reserve(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i) {
            tasks.push_back(make_task(i));
        }
        co_await boost::asio::experimental::make_parallel_group(tasks).async_wait(
            boost::asio::experimental::wait_for_all(),
            boost::asio::use_awaitable);
    }

    for (std::size_t index = 0; index < calls.size(); ++index) {
        auto& finalized_call = state->finalized[index];
        if (finalized_call) {
            continue;
        }
        // The batch wait finished without this task running, so no
        // ToolExecutionStartEvent was dispatched for it; produce only the
        // aborted result to keep lifecycle events strictly paired.
        finalized_call = abort_tool_call(calls[index]);
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

    std::vector<FinalizedToolCallResult> completed;
    completed.reserve(state->finalized.size());
    for (auto& finalized_call : state->finalized) {
        if (!finalized_call) {
            continue;
        }
        CCH_TRY_VOID(emit_tool_result_message(sink, finalized_call->result));
        completed.push_back(std::move(*finalized_call));
    }

    co_return make_batch_result(std::move(completed));
}

} // namespace cch::agent

