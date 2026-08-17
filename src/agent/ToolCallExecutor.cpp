#include "ToolCallExecutor.hpp"

#include "ExecutionShared.hpp"
#include "ToolArgumentPreparation.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/BoundedText.hpp"
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
#include <memory>
#include <mutex>
#include <optional>
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
    auto result = co_await ai::detail::await_async_result(
        tool.execute(invocation, stop_token, std::move(update_sink)));
    close_tool_updates(gate);
    co_return result;
}

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

struct FinalizedToolCall {
    ai::ToolResultMessage result;
    bool call_terminate{false};
};

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

constexpr std::size_t kMaxToolFailureDiagnosticBytes = 4096;

/// Failure text for a per-call error result: pi isolates a failed tool or
/// hook as the call's error result (ADR 0008). Redact before bounding the
/// diagnostic that is shown to the model.
[[nodiscard]] std::string bounded_failure_text(const support::Error& error) {
    std::string diagnostic = error.detail.empty() ? error.message : error.detail;
    return ai::bounded_redacted_text(
        std::move(diagnostic),
        kMaxToolFailureDiagnosticBytes,
        "...");
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
    std::vector<FinalizedToolCall> finalized) {
    // pi shouldTerminateToolBatch: every finalized result carries an explicit
    // terminate hint. Error results carry no implicit ban (ADR 0008): a result
    // finalizes as terminate only through the tool result or the after hook,
    // never through failure handling.
    const bool terminate = !finalized.empty() &&
                           std::all_of(finalized.begin(), finalized.end(), [](const FinalizedToolCall& call) {
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

} // namespace

ToolCallExecutor::ToolCallExecutor(
    ToolRegistry& registry,
    ToolCallExecutorOptions options)
    : registry_(registry), options_(std::move(options)) {}

boost::asio::awaitable<support::Expected<ToolCallBatchResult>> ToolCallExecutor::execute(
    ToolCallBatchRequest request,
    AgentEventSink& sink) {
    const auto calls = tool_calls_from(request.assistant_message);
    if (calls.empty()) {
        co_return ToolCallBatchResult{};
    }

    const auto* parallel = std::get_if<BoundedParallelToolExecution>(&options_.execution);

    // pi executeToolCalls: the whole batch runs through the sequential path
    // when the policy is sequential or when any tool call references a tool
    // whose execution mode is sequential (C++: an adapter declaring
    // `ToolConcurrency::Exclusive`). A bounded-parallel cap of one is
    // effectively sequential and shares that path.
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
    std::vector<FinalizedToolCall> finalized;
    finalized.reserve(calls.size());

    for (const auto& call : calls) {
        if (options_.stop_token.stop_requested()) {
            break;
        }
        CCH_TRY_VOID(emit_agent_event(
            sink,
            ToolExecutionStartEvent{
                .tool_call_id = call.id,
                .tool_name = call.name,
                .args = call.arguments.value_or(support::JsonValue{}),
            }));

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
                        // pi prepareToolCall: a failing before hook finalizes
                        // only this call's outcome; it never aborts the run
                        // (ADR 0008).
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
                    tool_result = error_tool_result(call, "Operation aborted");
                }

                if (!blocked) {
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
                        if (!update_gate->active) return support::ExpectedVoid{};
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
                    if (!executed) {
                        // A failed tool becomes an error outcome, never a run
                        // failure. Preserve the returned diagnostic.
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
                        // pi finalizeExecutedToolCall: the after hook runs for
                        // every executed outcome, success or error (ADR 0008).
                        AfterToolCallContext hook_context{
                            .assistant_message = request.assistant_message,
                            .tool_call = call,
                            .args = invocation.arguments,
                            .result = outcome,
                            .is_error = outcome.is_error,
                            .context = request.context,
                        };
                        auto after_result = co_await invoke_agent_hook(
                            "afterToolCall",
                            *options_.after_tool_call,
                            std::move(hook_context),
                            options_.stop_token);
                        if (!after_result) {
                            // A failing after hook finalizes this call's
                            // outcome as an error result; the run keeps going
                            // (ADR 0008).
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

        CCH_TRY_VOID(emit_agent_event(
            sink,
            ToolExecutionEndEvent{
                .tool_call_id = call.id,
                .tool_name = call.name,
                .result = execution_result_from(tool_result, call_terminate),
                .is_error = tool_result.is_error,
            }));
        CCH_TRY_VOID(emit_tool_result_message(sink, tool_result));
        finalized.push_back(FinalizedToolCall{
            .result = std::move(tool_result),
            .call_terminate = call_terminate,
        });
        if (options_.stop_token.stop_requested()) {
            break;
        }
    }

    co_return make_batch_result(std::move(finalized));
}

boost::asio::awaitable<support::Expected<ToolCallBatchResult>> ToolCallExecutor::execute_parallel(
    ToolCallBatchRequest request,
    const std::vector<ai::ToolCallContent>& calls,
    std::size_t max_in_flight,
    AgentEventSink& sink) {
    struct PreparedToolCall {
        std::size_t source_index{};
        ai::ToolCallContent tool_call;
        Tool* tool{};
        support::JsonValue arguments;
    };

    std::vector<PreparedToolCall> prepared;
    prepared.reserve(calls.size());
    std::vector<std::optional<FinalizedToolCall>> finalized_calls(calls.size());

    auto complete_immediate = [&](std::size_t source_index,
                                  const ai::ToolCallContent& call,
                                  ai::ToolResultMessage result) -> support::ExpectedVoid {
        auto emitted = emit_agent_event(
            sink,
            ToolExecutionEndEvent{
                .tool_call_id = call.id,
                .tool_name = call.name,
                .result = execution_result_from(result),
                .is_error = result.is_error,
            });
        if (!emitted) {
            return emitted;
        }
        finalized_calls[source_index] = FinalizedToolCall{
            .result = std::move(result),
            .call_terminate = false,
        };
        return {};
    };

    for (std::size_t source_index = 0; source_index < calls.size(); ++source_index) {
        if (options_.stop_token.stop_requested()) {
            break;
        }
        const auto& call = calls[source_index];
        CCH_TRY_VOID(emit_agent_event(
            sink,
            ToolExecutionStartEvent{
                .tool_call_id = call.id,
                .tool_name = call.name,
                .args = call.arguments.value_or(support::JsonValue{}),
            }));

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

        auto arguments = prepare_tool_arguments(tool->definition, call);
        if (!arguments) {
            CCH_TRY_VOID(complete_immediate(
                source_index,
                call,
                error_tool_result(call, arguments.error().detail)));
            continue;
        }

        if (options_.before_tool_call) {
            BeforeToolCallContext hook_context{
                .assistant_message = request.assistant_message,
                .tool_call = call,
                .args = *arguments,
                .context = request.context,
            };
            auto before_result = co_await invoke_agent_hook(
                "beforeToolCall",
                *options_.before_tool_call,
                std::move(hook_context),
                options_.stop_token);
            if (!before_result) {
                // pi prepareToolCall: a failing before hook finalizes only
                // this call's outcome (ADR 0008).
                CCH_TRY_VOID(complete_immediate(
                    source_index,
                    call,
                    error_tool_result(call, bounded_failure_text(before_result.error()))));
                continue;
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

        if (options_.stop_token.stop_requested()) {
            CCH_TRY_VOID(complete_immediate(
                source_index,
                call,
                error_tool_result(call, "Operation aborted")));
            continue;
        }

        prepared.push_back(PreparedToolCall{
            .source_index = source_index,
            .tool_call = call,
            .tool = tool,
            .arguments = std::move(*arguments),
        });
    }

    struct ParallelState {
        std::mutex callback_mutex;
        std::mutex error_mutex;
        std::optional<support::Error> emit_error;
        std::optional<support::Error> fatal_error;
    };

    auto executor = co_await boost::asio::this_coro::executor;
    using HookPermitChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code)>;
    std::shared_ptr<HookPermitChannel> after_hook_permits;
    if (options_.after_tool_call) {
        after_hook_permits = std::make_shared<HookPermitChannel>(executor, 1);
        if (!after_hook_permits->try_send(boost::system::error_code{})) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Tool,
                "afterToolCall hook serialization failed"));
        }
    }

    auto state = std::make_shared<ParallelState>();
    auto prepared_calls = std::make_shared<std::vector<PreparedToolCall>>(std::move(prepared));
    auto assistant_snapshot = std::make_shared<ai::AssistantMessage>(request.assistant_message);
    auto context_snapshot = std::make_shared<ai::AiContext>(request.context);
    auto next_index = std::make_shared<std::atomic_size_t>(0);
    auto finalized = std::make_shared<std::vector<std::optional<FinalizedToolCall>>>(
        std::move(finalized_calls));

    // Every prepared tool here is ParallelSafe: execute() routes any batch
    // containing an Exclusive (pi "sequential") tool through the sequential
    // path, so this worker pool never observes one. A cap of zero (the policy
    // default) dispatches every prepared call concurrently, matching pi's
    // unbounded parallel default (ADR 0034).
    const std::size_t execution_limit =
        max_in_flight == 0 ? prepared_calls->size() : max_in_flight;
    const std::size_t worker_count = std::min(execution_limit, prepared_calls->size());

    // Tool-update delivery is part of the executing call. Its failure must
    // return through ToolUpdateSink so that the worker can finalize that call
    // as an isolated error outcome; other lifecycle publication failures still
    // fail the batch.
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
        while (true) {
            if (stop_token.stop_requested()) {
                break;
            }
            const auto index = next_index->fetch_add(1);
            if (index >= prepared_calls->size()) {
                break;
            }
            if (stop_token.stop_requested()) {
                break;
            }

            auto& prepared_call = (*prepared_calls)[index];
            FinalizedToolCall outcome;

            auto update_gate = std::make_shared<ToolUpdateGate>();
            ToolInvocation invocation{
                .call_id = prepared_call.tool_call.id,
                .name = prepared_call.tool_call.name,
                .arguments = std::move(prepared_call.arguments),
                .raw_arguments = prepared_call.tool_call.raw_arguments,
            };
            ToolUpdateSink update_sink = [
                parallel_emit,
                update_gate,
                call_id = prepared_call.tool_call.id,
                tool_name = prepared_call.tool_call.name,
                args = prepared_call.tool_call.arguments.value_or(support::JsonValue{})](
                    const AsyncToolExecutionResult& partial_result) {
                std::lock_guard lock(update_gate->mutex);
                if (!update_gate->active) return support::ExpectedVoid{};
                return parallel_emit(ToolExecutionUpdateEvent{
                    .tool_call_id = call_id,
                    .tool_name = tool_name,
                    .args = args,
                    .partial_result = partial_result,
                });
            };
            AsyncToolExecutionResult executed_outcome;
            auto executed = co_await execute_with_update_lifetime(
                *prepared_call.tool,
                invocation,
                stop_token,
                std::move(update_sink),
                update_gate);
            if (!executed) {
                // A failed tool becomes an error outcome, never a run
                // failure. Preserve the returned diagnostic.
                executed_outcome = AsyncToolExecutionResult{
                    .content = std::vector<ai::Content>{ai::text_content(
                        bounded_failure_text(executed.error()))},
                    .details = std::nullopt,
                    .is_error = true,
                    .terminate = false,
                };
            } else {
                executed_outcome = std::move(*executed);
            }

            outcome.result.tool_call_id = prepared_call.tool_call.id;
            outcome.result.tool_name = prepared_call.tool_call.name;
            outcome.result.content = executed_outcome.content;
            outcome.result.details = executed_outcome.details;
            outcome.result.is_error = executed_outcome.is_error;
            outcome.call_terminate = executed_outcome.terminate;

            if (after_hook) {
                // pi finalizeExecutedToolCall: the after hook runs for
                // every executed outcome, success or error (ADR 0008).
                // Hook invocations stay serialized so the move-only
                // contract never overlaps.
                const auto [permit_error] = co_await after_hook_permits->async_receive(
                    boost::asio::as_tuple(boost::asio::use_awaitable));
                if (permit_error) {
                    std::lock_guard error_lock(state->error_mutex);
                    if (!state->fatal_error) {
                        state->fatal_error = support::make_error(
                            support::ErrorCode::Tool,
                            "afterToolCall hook serialization failed",
                            permit_error.message());
                    }
                    outcome.result = error_tool_result(
                        prepared_call.tool_call,
                        "afterToolCall hook failed");
                    outcome.call_terminate = false;
                } else {
                    AfterToolCallContext hook_context{
                        .assistant_message = *assistant_snapshot,
                        .tool_call = prepared_call.tool_call,
                        .args = invocation.arguments,
                        .result = executed_outcome,
                        .is_error = executed_outcome.is_error,
                        .context = *context_snapshot,
                    };
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
                            state->fatal_error = support::make_error(
                                support::ErrorCode::Tool,
                                "afterToolCall hook serialization failed");
                        }
                        outcome.result = error_tool_result(
                            prepared_call.tool_call,
                            "afterToolCall hook failed");
                        outcome.call_terminate = false;
                    } else if (!after_result) {
                        // A failing after hook finalizes this call's
                        // outcome as an error result; the run keeps going.
                        outcome.result = error_tool_result(
                            prepared_call.tool_call,
                            bounded_failure_text(after_result.error()));
                        outcome.call_terminate = false;
                    } else {
                        apply_after_result(
                            outcome.result,
                            outcome.call_terminate,
                            std::move(*after_result),
                            executed_outcome.terminate);
                    }
                }
            }

            (void)parallel_emit(ToolExecutionEndEvent{
                .tool_call_id = prepared_call.tool_call.id,
                .tool_name = prepared_call.tool_call.name,
                .result = execution_result_from(outcome.result, outcome.call_terminate),
                .is_error = outcome.result.is_error,
            });
            (*finalized)[prepared_call.source_index] = std::move(outcome);
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

    for (const auto& prepared_call : *prepared_calls) {
        auto& finalized_call = (*finalized)[prepared_call.source_index];
        if (finalized_call) {
            continue;
        }
        auto result = error_tool_result(prepared_call.tool_call, "Operation aborted");
        (void)parallel_emit(ToolExecutionEndEvent{
            .tool_call_id = prepared_call.tool_call.id,
            .tool_name = prepared_call.tool_call.name,
            .result = execution_result_from(result),
            .is_error = true,
        });
        finalized_call = FinalizedToolCall{
            .result = std::move(result),
            .call_terminate = false,
        };
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

    std::vector<FinalizedToolCall> completed;
    completed.reserve(finalized->size());
    for (auto& finalized_call : *finalized) {
        if (!finalized_call) {
            continue;
        }
        CCH_TRY_VOID(emit_tool_result_message(sink, finalized_call->result));
        completed.push_back(std::move(*finalized_call));
    }

    co_return make_batch_result(std::move(completed));
}

} // namespace cch::agent
