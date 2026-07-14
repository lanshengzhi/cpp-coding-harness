#include "../../include/cch/agent/AgentLoop.hpp"

#include "ToolCallExecutor.hpp"

#include "../../src/util/ExpectedMacros.hpp"

#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Usage.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "util/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {
namespace {

void sync_state(AgentState& state, const ai::AiContext& context) {
    state.messages = context.messages;
}

[[nodiscard]] std::size_t approximate_content_size(const ai::Content& block) {
    return std::visit(
        [](const auto& c) -> std::size_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::TextContent>) {
                return c.text.size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::ImageContent>) {
                return c.data.size() + c.mime_type.size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(c)>, ai::ThinkingContent>) {
                return c.thinking.size();
            }
            return 0;
        },
        block);
}

[[nodiscard]] std::size_t approximate_message_size(const ai::MessageVariant& message) {
    return std::visit(
        [](const auto& m) -> std::size_t {
            if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::UserMessage>) {
                std::size_t size = 0;
                for (const auto& block : m.content) {
                    size += approximate_content_size(block);
                }
                return size;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::AssistantMessage>) {
                std::size_t size = 0;
                for (const auto& block : m.content) {
                    if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                        size += text->text.size();
                    } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                        size += thinking->thinking.size();
                    } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                        size += call->raw_arguments.size();
                    }
                }
                return size;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::ToolResultMessage>) {
                return ai::text_from_content(m.content).size();
            } else if constexpr (std::is_same_v<std::decay_t<decltype(m)>, ai::SystemMessage>) {
                return m.content.size();
            }
            return 0;
        },
        message);
}

[[nodiscard]] util::ExpectedVoid validate_queued_messages(
    const std::vector<ai::MessageVariant>& messages) {
    constexpr std::size_t max_queued_messages = 256;
    constexpr std::size_t max_queued_bytes = 16 * 1024 * 1024;

    if (messages.size() > max_queued_messages) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "too many queued messages",
            "steering/follow-up message count exceeds " + std::to_string(max_queued_messages)));
    }

    std::size_t total = 0;
    for (const auto& message : messages) {
        total += approximate_message_size(message);
    }
    if (total > max_queued_bytes) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "queued messages too large",
            "steering/follow-up message byte size exceeds " + std::to_string(max_queued_bytes)));
    }
    return {};
}

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) {
    static const std::vector<std::string> allowed{
        "off", "minimal", "low", "medium", "high", "xhigh"};
    return std::find(allowed.begin(), allowed.end(), level) != allowed.end();
}

[[nodiscard]] util::ExpectedVoid apply_turn_update(
    AsyncAgentOptions& options,
    ai::AiContext& context,
    AgentState& state,
    const AgentLoopTurnUpdate& update) {
    if (update.append_messages) {
        if (auto validated = validate_queued_messages(*update.append_messages); !validated) {
            return validated;
        }
    }
    if (update.model && update.model->empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid model",
            "model update cannot be empty"));
    }
    if (update.thinking_level && !is_valid_thinking_level(*update.thinking_level)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid thinking level",
            *update.thinking_level));
    }

    if (update.append_messages) {
        for (auto& message : *update.append_messages) {
            context.messages.push_back(std::move(message));
        }
        sync_state(state, context);
    }

    if (update.model) {
        options.model = *update.model;
        context.model = *update.model;
        state.model = *update.model;
    }

    if (update.thinking_level) {
        state.thinking_level = *update.thinking_level;
    }

    return {};
}

[[nodiscard]] util::Expected<std::vector<ai::MessageVariant>> invoke_transform_context_hook(
    TransformContextHook& hook,
    const std::vector<ai::MessageVariant>& messages) {
    try {
        return hook(messages);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "transformContext hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "transformContext hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::Expected<std::vector<ai::MessageVariant>> invoke_convert_to_llm_hook(
    ConvertToLlmHook& hook,
    const std::vector<ai::MessageVariant>& messages) {
    try {
        return hook(messages);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "convertToLlm hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "convertToLlm hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::Expected<std::vector<ai::MessageVariant>> invoke_get_steering_messages_hook(
    GetSteeringMessagesHook& hook) {
    try {
        return hook();
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "getSteeringMessages hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "getSteeringMessages hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::Expected<std::vector<ai::MessageVariant>> invoke_get_follow_up_messages_hook(
    GetFollowUpMessagesHook& hook) {
    try {
        return hook();
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "getFollowUpMessages hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "getFollowUpMessages hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::Expected<std::optional<AgentLoopTurnUpdate>> invoke_prepare_next_turn_hook(
    PrepareNextTurnHook& hook,
    const PrepareNextTurnContext& context) {
    try {
        return hook(context);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "prepareNextTurn hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "prepareNextTurn hook failed", "unknown exception"));
    }
}

[[nodiscard]] util::ExpectedVoid invoke_validate_turn_update_hook(
    ValidateTurnUpdateHook& hook,
    const AgentLoopTurnUpdate& update) {
    try {
        return hook(update);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "validateTurnUpdate hook failed", e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool, "validateTurnUpdate hook failed", "unknown exception"));
    }
}

} // namespace

AsyncAgentLoop::AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options)
    : client_(client), registry_(std::move(registry)), options_(std::move(options)) {}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::run(
    std::string user_prompt,
    AgentEventSink sink) {
    co_return co_await continue_with({}, std::move(user_prompt), std::move(sink));
}

boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> AsyncAgentLoop::continue_with(
    std::vector<ai::MessageVariant> history,
    std::string user_prompt,
    AgentEventSink sink) {
    ai::AiContext context;
    context.model = options_.model;
    context.tools = registry_.definitions();
    context.messages = std::move(history);

    AgentState state;
    state.model = options_.model;

    CCH_TRY_VOID(emit(sink, AgentStartEvent{}));

    context.messages.push_back(ai::MessageVariant{ai::user_text_message(std::move(user_prompt))});
    sync_state(state, context);

    std::vector<ai::MessageVariant> pending_messages;
    if (options_.get_steering_messages) {
        auto steering = invoke_get_steering_messages_hook(*options_.get_steering_messages);
        if (!steering) {
            CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
            co_return std::unexpected(steering.error());
        }
        if (auto validated = validate_queued_messages(*steering); !validated) {
            CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
            co_return std::unexpected(validated.error());
        }
        pending_messages = std::move(*steering);
    }

    for (int turn = 1; turn <= options_.max_turns; ++turn) {
        CCH_TRY_VOID(emit(sink, TurnStartEvent{}));

        if (!pending_messages.empty()) {
            for (auto& message : pending_messages) {
                CCH_TRY_VOID(emit(sink, MessageStartEvent{message}));
                context.messages.push_back(std::move(message));
                sync_state(state, context);
                CCH_TRY_VOID(emit(sink, MessageEndEvent{context.messages.back()}));
            }
            pending_messages.clear();
        }

        ai::StreamChatRequest request;
        request.model = options_.model;

        {
            ai::AiContext request_context = context;
            if (options_.transform_context) {
                auto transformed = invoke_transform_context_hook(
                    *options_.transform_context, request_context.messages);
                if (!transformed) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(transformed.error());
                }
                request_context.messages = std::move(*transformed);
            }
            if (options_.convert_to_llm) {
                auto converted = invoke_convert_to_llm_hook(
                    *options_.convert_to_llm, request_context.messages);
                if (!converted) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(converted.error());
                }
                if (converted->empty()) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "convertToLlm returned no messages",
                        "LLM request would be empty");
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(error);
                }
                request_context.messages = std::move(*converted);
            }
            request.context = std::move(request_context);
        }

        auto assistant = co_await client_.stream(
            request,
            [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                if (const auto* start = std::get_if<ai::AssistantStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, MessageStartEvent{ai::MessageVariant{start->partial}});
                }
                if (const auto* start = std::get_if<ai::TextStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::TextEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
                }
                if (const auto* start = std::get_if<ai::ThinkingStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::ThinkingDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::ThinkingEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
                }
                if (const auto* start = std::get_if<ai::ToolCallStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{start->partial}, event});
                }
                if (const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{delta->partial}, event});
                }
                if (const auto* end = std::get_if<ai::ToolCallEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    if (!end->tool_call.id.empty()) {
                        state.pending_tool_call_ids.push_back(end->tool_call.id);
                    }
                    return emit(sink, MessageUpdateEvent{ai::MessageVariant{end->partial}, event});
                }
                if (const auto* done = std::get_if<ai::AssistantDoneEvent>(&event)) {
                    state.streaming_message = done->message;
                    return {};
                }
                if (const auto* error = std::get_if<ai::AssistantErrorEvent>(&event)) {
                    state.streaming_message = error->error;
                    return {};
                }
                return {};
            });

        if (!assistant) {
            std::string reason = assistant.error().message;
            if (assistant.error().code == util::ErrorCode::Provider && !assistant.error().detail.empty()) {
                reason += " (" + assistant.error().detail + ")";
            }
            CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
            co_return std::unexpected(assistant.error());
        }

        CCH_TRY_VOID(emit(sink, MessageEndEvent{ai::MessageVariant{*assistant}}));
        context.messages.push_back(ai::MessageVariant{*assistant});
        state.streaming_message = *assistant;

        auto calls = tool_calls(*assistant);
        state.pending_tool_call_ids.clear();
        for (const auto& call : calls) {
            if (!call.id.empty()) {
                state.pending_tool_call_ids.push_back(call.id);
            }
        }

        std::vector<ai::ToolResultMessage> tool_results;
        bool terminate_batch = false;
        if (!calls.empty() && assistant->stop_reason == ai::AssistantStopReason::Length) {
            tool_results.reserve(calls.size());
            for (const auto& call : calls) {
                CCH_TRY_VOID(emit(sink, ToolExecutionStartEvent{call.id, call.name, call.arguments.value_or(util::JsonValue{})}));

                ai::ToolResultMessage result;
                result.tool_call_id = call.id;
                result.tool_name = call.name;
                result.is_error = true;
                result.content.emplace_back(ai::text_content(
                    "Tool call \"" + call.name +
                    "\" was not executed because the response hit the output token limit and its arguments may be "
                    "truncated. Re-issue the tool call with complete arguments."));

                AsyncToolExecutionResult execution_result;
                execution_result.content = result.content;
                execution_result.is_error = true;
                CCH_TRY_VOID(emit(sink, ToolExecutionEndEvent{
                    call.id, call.name, std::move(execution_result), true}));
                tool_results.push_back(std::move(result));
            }
        } else if (!calls.empty()) {
            agent::ToolCallExecutorOptions executor_options;
            if (options_.before_tool_call) {
                executor_options.before_tool_call = [&hook = options_.before_tool_call](
                                                         const BeforeToolCallContext& context)
                                                         -> util::Expected<BeforeToolCallResult> {
                    return (*hook)(context);
                };
            }
            if (options_.after_tool_call) {
                executor_options.after_tool_call = [&hook = options_.after_tool_call](
                                                        const AfterToolCallContext& context)
                                                        -> util::Expected<AfterToolCallResult> {
                    return (*hook)(context);
                };
            }
            executor_options.execution = options_.tool_execution;
            agent::ToolCallExecutor executor{registry_, std::move(executor_options)};

            auto execution = co_await executor.execute(
                agent::ToolCallBatchRequest{turn, *assistant, context},
                sink);
            if (!execution) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                co_return std::unexpected(execution.error());
            }

            tool_results = std::move(execution->results);
            terminate_batch = execution->terminate_batch;
        }

        if (!tool_results.empty()) {
            for (const auto& result : tool_results) {
                context.messages.push_back(ai::MessageVariant{result});
            }
            state.active_tool_names.clear();
            state.pending_tool_call_ids.clear();
            sync_state(state, context);
        }

        CCH_TRY_VOID(emit(sink, TurnEndEvent{ai::MessageVariant{*assistant}, tool_results}));

        if (terminate_batch) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), ai::AssistantStopReason::ToolUse, turn, std::move(state)};
        }

        if (options_.get_steering_messages) {
            auto steering = invoke_get_steering_messages_hook(*options_.get_steering_messages);
            if (!steering) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                co_return std::unexpected(steering.error());
            }
            if (auto validated = validate_queued_messages(*steering); !validated) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                co_return std::unexpected(validated.error());
            }
            pending_messages = std::move(*steering);
        }

        if (calls.empty() && pending_messages.empty()) {
            if (options_.get_follow_up_messages) {
                auto follow_up = invoke_get_follow_up_messages_hook(*options_.get_follow_up_messages);
                if (!follow_up) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(follow_up.error());
                }
                if (auto validated = validate_queued_messages(*follow_up); !validated) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(validated.error());
                }
                pending_messages = std::move(*follow_up);
            }
        }

        if (options_.prepare_next_turn) {
            PrepareNextTurnContext prepare_context;
            prepare_context.assistant_message = *assistant;
            prepare_context.tool_results = tool_results;
            prepare_context.context = context;
            prepare_context.queued_messages = pending_messages;

            auto update = invoke_prepare_next_turn_hook(*options_.prepare_next_turn, prepare_context);
            if (!update) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                co_return std::unexpected(update.error());
            }
            if (*update) {
                if ((**update).model && !options_.validate_turn_update) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "model update requires validation",
                        (**update).model.value());
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(error);
                }
                if (options_.validate_turn_update) {
                    auto validated = invoke_validate_turn_update_hook(*options_.validate_turn_update, **update);
                    if (!validated) {
                        CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                        co_return std::unexpected(validated.error());
                    }
                }
                if (auto applied = apply_turn_update(options_, context, state, **update); !applied) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
                    co_return std::unexpected(applied.error());
                }
            }
        }

        if (calls.empty() && pending_messages.empty()) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), assistant->stop_reason, turn, std::move(state)};
        }
    }

    auto error = util::make_error(
        util::ErrorCode::Provider,
        "max turns exceeded",
        "agent reached max_turns before a final assistant response");
    CCH_TRY_VOID(emit(sink, AgentEndEvent{context.messages}));
    co_return std::unexpected(error);
}

util::ExpectedVoid AsyncAgentLoop::emit(AgentEventSink& sink, const AgentLifecycleEvent& event) const {
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

std::vector<ai::ToolCallContent> AsyncAgentLoop::tool_calls(const ai::AssistantMessage& message) const {
    std::vector<ai::ToolCallContent> calls;
    for (const auto& block : message.content) {
        if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
            calls.push_back(*call);
        }
    }
    return calls;
}

} // namespace cch::agent
