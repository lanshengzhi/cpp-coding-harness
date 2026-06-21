#include "../../include/cch/agent/AgentLoop.hpp"

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

struct AsyncAgentLoop::FinalizedToolCall {
    ai::ToolCallContent tool_call;
    ai::ToolResultMessage result;
    bool is_error{false};
    bool call_terminate{false};
};

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

    CCH_TRY_VOID(emit(sink, AgentStartEvent{user_prompt}));

    context.messages.push_back(ai::MessageVariant{ai::user_text_message(std::move(user_prompt))});
    sync_state(state, context);

    std::vector<ai::MessageVariant> pending_messages;
    if (options_.get_steering_messages) {
        auto steering = invoke_get_steering_messages_hook(*options_.get_steering_messages);
        if (!steering) {
            CCH_TRY_VOID(emit(sink, AgentEndEvent{false, steering.error().message}));
            co_return std::unexpected(steering.error());
        }
        if (auto validated = validate_queued_messages(*steering); !validated) {
            CCH_TRY_VOID(emit(sink, AgentEndEvent{false, validated.error().message}));
            co_return std::unexpected(validated.error());
        }
        pending_messages = std::move(*steering);
    }

    for (int turn = 1; turn <= options_.max_turns; ++turn) {
        CCH_TRY_VOID(emit(sink, TurnStartEvent{turn}));

        if (!pending_messages.empty()) {
            for (auto& message : pending_messages) {
                CCH_TRY_VOID(emit(sink, QueuedMessageStartEvent{turn, message}));
                context.messages.push_back(std::move(message));
                sync_state(state, context);
                CCH_TRY_VOID(emit(sink, QueuedMessageEndEvent{turn, context.messages.back()}));
            }
            pending_messages.clear();
        }

        CCH_TRY_VOID(emit(sink, MessageStartEvent{turn}));

        ai::StreamChatRequest request;
        request.model = options_.model;

        {
            ai::AiContext request_context = context;
            if (options_.transform_context) {
                auto transformed = invoke_transform_context_hook(
                    *options_.transform_context, request_context.messages);
                if (!transformed) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, transformed.error().message}));
                    co_return std::unexpected(transformed.error());
                }
                request_context.messages = std::move(*transformed);
            }
            if (options_.convert_to_llm) {
                auto converted = invoke_convert_to_llm_hook(
                    *options_.convert_to_llm, request_context.messages);
                if (!converted) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, converted.error().message}));
                    co_return std::unexpected(converted.error());
                }
                if (converted->empty()) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "convertToLlm returned no messages",
                        "LLM request would be empty");
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, error.message}));
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
                    return {};
                }
                if (const auto* start = std::get_if<ai::TextStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return {};
                }
                if (const auto* delta = std::get_if<ai::TextDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, MessageUpdateEvent{turn, delta->delta});
                }
                if (const auto* end = std::get_if<ai::TextEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return {};
                }
                if (const auto* start = std::get_if<ai::ThinkingStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return {};
                }
                if (const auto* delta = std::get_if<ai::ThinkingDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, ThinkingUpdateEvent{turn, delta->content_index, delta->delta});
                }
                if (const auto* end = std::get_if<ai::ThinkingEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    return {};
                }
                if (const auto* start = std::get_if<ai::ToolCallStartEvent>(&event)) {
                    state.streaming_message = start->partial;
                    return emit(sink, ToolCallStreamStartEvent{turn, start->content_index});
                }
                if (const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&event)) {
                    state.streaming_message = delta->partial;
                    return emit(sink, ToolCallStreamUpdateEvent{turn, delta->content_index, delta->delta});
                }
                if (const auto* end = std::get_if<ai::ToolCallEndEvent>(&event)) {
                    state.streaming_message = end->partial;
                    if (!end->tool_call.id.empty()) {
                        state.pending_tool_call_ids.push_back(end->tool_call.id);
                    }
                    return emit(sink, ToolCallStreamEndEvent{turn, end->content_index, end->tool_call});
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
            CCH_TRY_VOID(emit(sink, AgentEndEvent{false, reason}));
            co_return std::unexpected(assistant.error());
        }

        CCH_TRY_VOID(emit(sink, MessageEndEvent{turn, *assistant}));
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
        if (!calls.empty()) {
            const bool has_sequential_tool = std::any_of(
                calls.begin(), calls.end(), [this](const ai::ToolCallContent& call) {
                    auto* tool = registry_.find(call.name);
                    return tool == nullptr ||
                           tool->execution_mode() == ai::ToolExecutionMode::Sequential;
                });

            auto execution = options_.tool_execution_mode == ai::ToolExecutionMode::Sequential || has_sequential_tool ||
                                     calls.size() > options_.max_parallel_tools
                                 ? co_await execute_tool_calls_sequential(turn, *assistant, calls, context, state, sink)
                                 : co_await execute_tool_calls_parallel(turn, *assistant, calls, context, state, sink);

            if (!execution) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, execution.error().message}));
                co_return std::unexpected(execution.error());
            }

            tool_results = std::move(execution->first);
            terminate_batch = execution->second;

            for (auto& result : tool_results) {
                context.messages.push_back(ai::MessageVariant{std::move(result)});
            }
            sync_state(state, context);
        }

        CCH_TRY_VOID(emit(sink, TurnEndEvent{turn, assistant->stop_reason}));

        if (terminate_batch) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit(sink, AgentEndEvent{true, ai::stop_reason_to_string(ai::AssistantStopReason::ToolUse)}));
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), ai::AssistantStopReason::ToolUse, turn, std::move(state)};
        }

        if (options_.get_steering_messages) {
            auto steering = invoke_get_steering_messages_hook(*options_.get_steering_messages);
            if (!steering) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, steering.error().message}));
                co_return std::unexpected(steering.error());
            }
            if (auto validated = validate_queued_messages(*steering); !validated) {
                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, validated.error().message}));
                co_return std::unexpected(validated.error());
            }
            pending_messages = std::move(*steering);
        }

        if (calls.empty() && pending_messages.empty()) {
            if (options_.get_follow_up_messages) {
                auto follow_up = invoke_get_follow_up_messages_hook(*options_.get_follow_up_messages);
                if (!follow_up) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, follow_up.error().message}));
                    co_return std::unexpected(follow_up.error());
                }
                if (auto validated = validate_queued_messages(*follow_up); !validated) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, validated.error().message}));
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
                CCH_TRY_VOID(emit(sink, AgentEndEvent{false, update.error().message}));
                co_return std::unexpected(update.error());
            }
            if (*update) {
                if ((**update).model && !options_.validate_turn_update) {
                    auto error = util::make_error(
                        util::ErrorCode::Validation,
                        "model update requires validation",
                        (**update).model.value());
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, error.message}));
                    co_return std::unexpected(error);
                }
                if (options_.validate_turn_update) {
                    auto validated = invoke_validate_turn_update_hook(*options_.validate_turn_update, **update);
                    if (!validated) {
                        CCH_TRY_VOID(emit(sink, AgentEndEvent{false, validated.error().message}));
                        co_return std::unexpected(validated.error());
                    }
                }
                if (auto applied = apply_turn_update(options_, context, state, **update); !applied) {
                    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, applied.error().message}));
                    co_return std::unexpected(applied.error());
                }
            }
        }

        if (calls.empty() && pending_messages.empty()) {
            state.streaming_message.reset();
            CCH_TRY_VOID(emit(sink, AgentEndEvent{true, ai::stop_reason_to_string(assistant->stop_reason)}));
            sync_state(state, context);
            co_return AsyncAgentRunResult{std::move(context), assistant->stop_reason, turn, std::move(state)};
        }
    }

    auto error = util::make_error(
        util::ErrorCode::Provider,
        "max turns exceeded",
        "agent reached max_turns before a final assistant response");
    CCH_TRY_VOID(emit(sink, AgentEndEvent{false, error.message}));
    co_return std::unexpected(error);
}

boost::asio::awaitable<util::Expected<std::pair<std::vector<ai::ToolResultMessage>, bool>>>
AsyncAgentLoop::execute_tool_calls_sequential(
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
    co_return std::make_pair(std::move(messages), terminate);
}

boost::asio::awaitable<util::Expected<std::pair<std::vector<ai::ToolResultMessage>, bool>>>
AsyncAgentLoop::execute_tool_calls_parallel(
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

    auto parallel_emit = [this, parallel_state, sink_handle](const AgentLifecycleEvent& event) -> util::ExpectedVoid {
        std::lock_guard emit_lock(parallel_state->emit_mutex);
        util::ExpectedVoid result;
        {
            std::lock_guard sink_lock(sink_handle->mutex);
            if (!sink_handle->active || sink_handle->sink == nullptr) {
                return {};
            }
            result = this->emit(*sink_handle->sink, event);
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
    co_return std::make_pair(std::move(messages), terminate);
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
