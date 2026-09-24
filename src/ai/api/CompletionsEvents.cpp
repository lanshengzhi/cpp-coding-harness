#include "CompletionsEvents.hpp"

#include "PartialJson.hpp"
#include "ai/JsonAccess.hpp"
#include "ai/Timestamps.hpp"
#include "ai/api/UsageNormalization.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;

struct ToolSlot {
    std::size_t key{};
    std::size_t content_index{};
    std::optional<std::size_t> stream_index{std::nullopt};
    bool start_emitted{false};
};

[[nodiscard]] CompletionsProviderError provider_error(const JsonObject& event) {
    const auto* nested = json_object_member(event, "error");
    auto code = json_string_member(event, "code");
    if (!code && nested) {
        code = json_string_member(*nested, "code");
    }
    auto message = json_string_member(event, "message");
    if (!message && nested) {
        message = json_string_member(*nested, "message");
    }
    return CompletionsProviderError{
            .code = code ? std::optional<std::string>{std::string{*code}} : std::nullopt,
            .message = message ? std::optional<std::string>{std::string{*message}} : std::nullopt,
            .suggested_backoff_ms = providers::provider_backoff_hint_ms(event, current_timestamp_ms()),
    };
}

void apply_usage(const Model& model, const JsonObject& usage, AssistantMessage& assistant) {
    const auto* input_details = json_object_member(usage, "prompt_tokens_details");
    CompletionsUsageFields fields{
            .prompt_tokens = json_integer_member(usage, "prompt_tokens").value_or(0),
            .completion_tokens = json_integer_member(usage, "completion_tokens").value_or(0),
            .cached_tokens = input_details ? json_integer_member(*input_details, "cached_tokens")
                                           : json_integer_member(usage, "cached_tokens"),
            .prompt_cache_hit_tokens = json_integer_member(usage, "prompt_cache_hit_tokens"),
            .cache_write_tokens =
                    input_details ? json_integer_member(*input_details, "cache_write_tokens") : std::nullopt,
            .reasoning_tokens = [&usage]() -> std::optional<std::int64_t> {
                const auto* details = json_object_member(usage, "completion_tokens_details");
                return details ? json_integer_member(*details, "reasoning_tokens") : std::nullopt;
            }(),
    };
    assistant.usage = normalize_completions_usage(model, fields);
}

[[nodiscard]] support::ExpectedVoid append_reasoning_detail(const JsonObject& detail,
        std::vector<support::JsonValue>& details,
        AssistantMessage& assistant,
        std::optional<std::size_t> thinking_index) {
    const auto type = json_string_member(detail, "type");
    if (!type) {
        return {};
    }
    if ((*type != "reasoning.text" && *type != "reasoning.summary" && *type != "reasoning.encrypted")) {
        return {};
    }
    if (!details.empty()) {
        auto* last = details.back().get_if<JsonObject>();
        if (last) {
            const auto last_type = json_string_member(*last, "type");
            const auto field = *type == "reasoning.text" ? "text" : "summary";
            if (last_type && *last_type == *type && *type != "reasoning.encrypted") {
                const auto current = json_string_member(*last, field);
                const auto next = json_string_member(detail, field);
                if (next) {
                    auto& target = (*last)[field];
                    target = std::string{current.value_or("")} + std::string{*next};
                }
                for (const auto& [name, value] : detail) {
                    last->try_emplace(name, value);
                }
            } else {
                details.emplace_back(detail);
            }
        } else {
            details.emplace_back(detail);
        }
    } else {
        details.emplace_back(detail);
    }
    if (!thinking_index) {
        return {};
    }
    auto& thinking = std::get<ThinkingContent>(assistant.content[*thinking_index]);
    auto serialized = support::write_json(support::JsonValue{details});
    if (serialized) {
        thinking.thinking_signature = std::move(*serialized);
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid emit_text_delta(const std::string& delta,
        AssistantMessage& assistant,
        std::optional<std::size_t>& text_index,
        AssistantEventSink& sink) {
    if (delta.empty()) {
        return {};
    }
    const bool first_delta = !text_index;
    if (first_delta) {
        text_index = assistant.content.size();
        assistant.content.emplace_back(TextContent{
                .text = {},
                .text_signature = std::nullopt,
        });
    }
    auto& text = std::get<TextContent>(assistant.content[*text_index]);
    text.text += delta;
    if (first_delta) {
        auto emitted = providers::emit(sink,
                TextStartEvent{
                        .content_index = *text_index,
                        .partial = assistant,
                });
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
    }
    return providers::emit(sink,
            TextDeltaEvent{
                    .content_index = *text_index,
                    .delta = delta,
                    .partial = assistant,
            });
}

[[nodiscard]] support::ExpectedVoid emit_thinking_delta(const std::string& delta,
        std::string signature,
        AssistantMessage& assistant,
        std::optional<std::size_t>& thinking_index,
        AssistantEventSink& sink) {
    if (delta.empty()) {
        return {};
    }
    if (!thinking_index) {
        thinking_index = assistant.content.size();
        assistant.content.emplace_back(ThinkingContent{
                .thinking = {},
                .thinking_signature = std::move(signature),
                .redacted = false,
        });
    }
    auto& thinking = std::get<ThinkingContent>(assistant.content[*thinking_index]);
    const bool first_delta = thinking.thinking.empty();
    thinking.thinking += delta;
    if (first_delta) {
        auto emitted = providers::emit(sink,
                ThinkingStartEvent{
                        .content_index = *thinking_index,
                        .partial = assistant,
                });
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
    }
    return providers::emit(sink,
            ThinkingDeltaEvent{
                    .content_index = *thinking_index,
                    .delta = delta,
                    .partial = assistant,
            });
}

[[nodiscard]] support::Expected<ToolSlot*> find_or_create_tool(const JsonObject& tool_call,
        std::map<std::size_t, ToolSlot>& slots,
        std::map<std::string, std::size_t, std::less<>>& ids,
        std::size_t& next_key,
        AssistantMessage& assistant) {
    const auto index = json_integer_member(tool_call, "index");
    const auto id = json_string_member(tool_call, "id");
    ToolSlot* slot = nullptr;
    if (index) {
        const auto found = slots.find(static_cast<std::size_t>(*index));
        if (found != slots.end()) {
            slot = &found->second;
        }
    }
    if (!slot && id) {
        if (const auto found = ids.find(std::string{*id}); found != ids.end()) {
            slot = &slots.at(found->second);
        }
    }
    if (!slot) {
        const auto key = index ? static_cast<std::size_t>(*index) : next_key++;
        while (slots.contains(key)) {
            ++next_key;
        }
        const auto content_index = assistant.content.size();
        assistant.content.emplace_back(ToolCallContent{
                .id = id ? std::string{*id} : std::string{},
                .name = {},
                .arguments = support::JsonValue::object_t{},
                .raw_arguments = {},
                .thought_signature = std::nullopt,
                .arguments_valid = true,
                .argument_error = std::nullopt,
        });
        auto [inserted, _] = slots.emplace(key,
                ToolSlot{
                        .key = key,
                        .content_index = content_index,
                        .stream_index =
                                index ? std::optional<std::size_t>{static_cast<std::size_t>(*index)} : std::nullopt,
                        .start_emitted = false,
                });
        slot = &inserted->second;
        if (id && !id->empty()) {
            ids.emplace(std::string{*id}, key);
        }
    }
    if (index && !slot->stream_index) {
        slot->stream_index = static_cast<std::size_t>(*index);
    }
    if (id && !id->empty() && !ids.contains(std::string{*id})) {
        ids.emplace(std::string{*id}, slot->key);
    }
    return slot;
}

[[nodiscard]] support::ExpectedVoid process_tool_calls(const JsonObject& delta,
        std::map<std::size_t, ToolSlot>& slots,
        std::map<std::string, std::size_t, std::less<>>& ids,
        std::size_t& next_key,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    const auto* calls = json_array_member(delta, "tool_calls");
    if (!calls) {
        return {};
    }
    for (const auto& value : *calls) {
        const auto* tool_call = value.get_if<JsonObject>();
        if (!tool_call) {
            return std::unexpected(providers::make_stream_error(
                    "Malformed OpenAI Chat Completions tool call", "tool_calls entry must be an object"));
        }
        auto slot = find_or_create_tool(*tool_call, slots, ids, next_key, assistant);
        if (!slot) {
            return std::unexpected(slot.error());
        }
        auto& call = std::get<ToolCallContent>(assistant.content[(*slot)->content_index]);
        const auto* function = json_object_member(*tool_call, "function");
        if (!function) {
            continue;
        }
        if (const auto name = json_string_member(*function, "name"); name && call.name.empty()) {
            call.name = std::string{*name};
        }
        const auto arguments = json_string_member(*function, "arguments");
        const std::string delta_arguments = arguments ? std::string{*arguments} : std::string{};
        call.raw_arguments += delta_arguments;
        call.arguments = parse_streaming_json(call.raw_arguments);
        call.arguments_valid = true;
        call.argument_error = std::nullopt;
        if (!(*slot)->start_emitted) {
            auto emitted = providers::emit(sink,
                    ToolCallStartEvent{
                            .content_index = (*slot)->content_index,
                            .partial = assistant,
                    });
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
            (*slot)->start_emitted = true;
        }
        auto emitted = providers::emit(sink,
                ToolCallDeltaEvent{
                        .content_index = (*slot)->content_index,
                        .delta = delta_arguments,
                        .partial = assistant,
                });
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
    }
    return {};
}

[[nodiscard]] support::ExpectedVoid finish_blocks(
        std::map<std::size_t, ToolSlot>& slots, AssistantMessage& assistant, AssistantEventSink& sink) {
    for (std::size_t content_index = 0; content_index < assistant.content.size(); ++content_index) {
        auto& content = assistant.content[content_index];
        if (auto* text = std::get_if<TextContent>(&content)) {
            auto emitted = providers::emit(sink,
                    TextEndEvent{
                            .content_index = content_index,
                            .content = text->text,
                            .partial = assistant,
                    });
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
            continue;
        }
        if (auto* thinking = std::get_if<ThinkingContent>(&content)) {
            auto emitted = providers::emit(sink,
                    ThinkingEndEvent{
                            .content_index = content_index,
                            .content = thinking->thinking,
                            .partial = assistant,
                    });
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
            continue;
        }
        auto* call = std::get_if<ToolCallContent>(&content);
        if (!call) {
            continue;
        }
        const auto found = std::ranges::find_if(
                slots, [content_index](const auto& entry) { return entry.second.content_index == content_index; });
        if (found == slots.end()) {
            continue;
        }
        finalize_tool_arguments(*call);
        auto emitted = providers::emit(sink,
                ToolCallEndEvent{
                        .content_index = content_index,
                        .tool_call = *call,
                        .partial = assistant,
                });
        if (!emitted) {
            return std::unexpected(emitted.error());
        }
    }
    slots.clear();
    return {};
}

[[nodiscard]] support::ExpectedVoid set_finish_reason(
        std::string_view finish_reason, AssistantMessage& assistant, bool& saw_finish_reason) {
    if (finish_reason.empty()) {
        return {};
    }
    saw_finish_reason = true;
    assistant.raw_stop_reason = std::string{finish_reason};
    if (finish_reason == "stop" || finish_reason == "end") {
        assistant.stop_reason = AssistantStopReason::Stop;
        return {};
    }
    if (finish_reason == "length") {
        assistant.stop_reason = AssistantStopReason::Length;
        return {};
    }
    if (finish_reason == "function_call" || finish_reason == "tool_calls") {
        assistant.stop_reason = AssistantStopReason::ToolUse;
        return {};
    }
    assistant.stop_reason = AssistantStopReason::Error;
    assistant.error_message = "Provider finish_reason: " + std::string{finish_reason};
    return {};
}

} // namespace

struct CompletionsEventProcessor::Impl {
    explicit Impl(Model configured_model) : model(std::move(configured_model)) {}

    Model model;
    std::map<std::size_t, ToolSlot> tool_slots;
    std::map<std::string, std::size_t, std::less<>> tool_ids;
    std::size_t next_tool_key{0};
    std::optional<std::size_t> text_index{std::nullopt};
    std::optional<std::size_t> thinking_index{std::nullopt};
    std::vector<support::JsonValue> reasoning_details;
    bool saw_finish_reason{false};
    bool finish_pending{false};
    bool blocks_finished{false};
};

CompletionsEventProcessor::CompletionsEventProcessor(Model model) : impl_(std::make_unique<Impl>(std::move(model))) {}

CompletionsEventProcessor::CompletionsEventProcessor(CompletionsEventProcessor&&) noexcept = default;
CompletionsEventProcessor& CompletionsEventProcessor::operator=(CompletionsEventProcessor&&) noexcept = default;
CompletionsEventProcessor::~CompletionsEventProcessor() = default;

support::Expected<CompletionsProcessOutcome> CompletionsEventProcessor::process(
        support::JsonValue::object_t event, AssistantMessage& assistant, AssistantEventSink& sink) {
    if (event.contains("error")) {
        return CompletionsProcessOutcome{
                .provider_error = provider_error(event),
        };
    }
    if (const auto id = json_string_member(event, "id"); id && !id->empty() && !assistant.response_id) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model = json_string_member(event, "model");
            model && !model->empty() && *model != impl_->model.id && !assistant.response_model) {
        assistant.response_model = std::string{*model};
    }
    if (const auto* usage = json_object_member(event, "usage")) {
        apply_usage(impl_->model, *usage, assistant);
    }
    const auto* choices = json_array_member(event, "choices");
    if (!choices || choices->empty()) {
        if (event.contains("usage") && impl_->finish_pending && !impl_->blocks_finished) {
            auto finished = finish_blocks(impl_->tool_slots, assistant, sink);
            if (!finished) {
                return std::unexpected(finished.error());
            }
            impl_->blocks_finished = true;
            impl_->finish_pending = false;
        }
        return CompletionsProcessOutcome{};
    }
    const auto* choice = choices->front().get_if<JsonObject>();
    if (!choice) {
        return std::unexpected(providers::make_stream_error(
                "Malformed OpenAI Chat Completions chunk", "choices[0] must be an object"));
    }
    if (!event.contains("usage")) {
        if (const auto* choice_usage = json_object_member(*choice, "usage")) {
            apply_usage(impl_->model, *choice_usage, assistant);
        }
    }
    const auto finish = json_string_member(*choice, "finish_reason");
    const auto* delta = json_object_member(*choice, "delta");
    if (delta) {
        if (const auto content = json_string_member(*delta, "content"); content) {
            auto emitted = emit_text_delta(std::string{*content}, assistant, impl_->text_index, sink);
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
        }
        for (const auto field : {"reasoning_content", "reasoning", "reasoning_text"}) {
            if (const auto reasoning = json_string_member(*delta, field); reasoning && !reasoning->empty()) {
                const std::string signature =
                        impl_->model.provider == "opencode-go" && std::string_view{field} == "reasoning"
                                ? "reasoning_content"
                                : field;
                auto emitted =
                        emit_thinking_delta(std::string{*reasoning}, signature, assistant, impl_->thinking_index, sink);
                if (!emitted) {
                    return std::unexpected(emitted.error());
                }
                break;
            }
        }
        auto processed_tools =
                process_tool_calls(*delta, impl_->tool_slots, impl_->tool_ids, impl_->next_tool_key, assistant, sink);
        if (!processed_tools) {
            return std::unexpected(processed_tools.error());
        }
        if (const auto* details = json_array_member(*delta, "reasoning_details")) {
            if (!impl_->thinking_index) {
                impl_->thinking_index = assistant.content.size();
                assistant.content.emplace_back(ThinkingContent{
                        .thinking = {},
                        .thinking_signature = std::nullopt,
                        .redacted = false,
                });
                auto emitted = providers::emit(sink,
                        ThinkingStartEvent{
                                .content_index = *impl_->thinking_index,
                                .partial = assistant,
                        });
                if (!emitted) {
                    return std::unexpected(emitted.error());
                }
            }
            for (const auto& detail : *details) {
                const auto* object = detail.get_if<JsonObject>();
                if (!object) {
                    continue;
                }
                auto appended =
                        append_reasoning_detail(*object, impl_->reasoning_details, assistant, impl_->thinking_index);
                if (!appended) {
                    return std::unexpected(appended.error());
                }
            }
        }
    }
    if (finish) {
        auto finish_set = set_finish_reason(*finish, assistant, impl_->saw_finish_reason);
        if (!finish_set) {
            return std::unexpected(finish_set.error());
        }
        impl_->finish_pending = true;
        if (event.contains("usage") && !impl_->blocks_finished) {
            auto finished = finish_blocks(impl_->tool_slots, assistant, sink);
            if (!finished) {
                return std::unexpected(finished.error());
            }
            impl_->blocks_finished = true;
            impl_->finish_pending = false;
        }
    }
    return CompletionsProcessOutcome{};
}

support::ExpectedVoid CompletionsEventProcessor::finish(AssistantMessage& assistant, AssistantEventSink& sink) {
    if (impl_->finish_pending && !impl_->blocks_finished) {
        auto finished = finish_blocks(impl_->tool_slots, assistant, sink);
        if (!finished) {
            return std::unexpected(finished.error());
        }
        impl_->blocks_finished = true;
        impl_->finish_pending = false;
    }
    if (impl_->thinking_index && !impl_->reasoning_details.empty()) {
        auto& thinking = std::get<ThinkingContent>(assistant.content[*impl_->thinking_index]);
        auto serialized = support::write_json(support::JsonValue{impl_->reasoning_details});
        if (serialized) {
            thinking.thinking_signature = std::move(*serialized);
        }
    }
    if (!impl_->saw_finish_reason) {
        return std::unexpected(providers::make_stream_error("Stream ended without finish_reason"));
    }
    if (assistant.stop_reason == AssistantStopReason::Error) {
        return std::unexpected(providers::make_stream_error(
                assistant.error_message.value_or("Provider returned an error stop reason")));
    }
    return {};
}

} // namespace cch::ai::api
