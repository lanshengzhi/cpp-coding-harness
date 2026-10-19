#include "MessageNormalization.hpp"

#include "MessageText.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

constexpr std::string_view kUserImagePlaceholder = "(image omitted: model does not support images)";
constexpr std::string_view kToolImagePlaceholder = "(tool image omitted: model does not support images)";

[[nodiscard]] std::vector<Content> downgrade_images(
        const std::vector<Content>& content, std::string_view placeholder, bool image_capable) {
    if (image_capable) {
        return content;
    }
    std::vector<Content> result;
    bool previous_placeholder = false;
    for (const auto& block : content) {
        if (std::holds_alternative<ImageContent>(block)) {
            if (!previous_placeholder) {
                result.emplace_back(text_content(std::string{placeholder}));
            }
            previous_placeholder = true;
            continue;
        }
        result.push_back(block);
        const auto* text = std::get_if<TextContent>(&block);
        previous_placeholder = text && text->text == placeholder;
    }
    return result;
}

} // namespace

[[nodiscard]] std::vector<MessageVariant> normalize_history(const Model& model, const AiContext& context) {
    std::vector<MessageVariant> result;
    std::vector<ToolCallContent> pending_calls;
    std::set<std::string, std::less<>> result_ids;

    const auto flush_orphans = [&]() {
        for (const auto& call : pending_calls) {
            if (!result_ids.contains(call.id)) {
                result.emplace_back(ToolResultMessage{
                        .tool_call_id = call.id,
                        .tool_name = call.name,
                        .content = {text_content("No result provided")},
                        .details = std::nullopt,
                        .is_error = true,
                        .timestamp = 0,
                });
            }
        }
        pending_calls.clear();
        result_ids.clear();
    };

    for (const auto& message : context.messages) {
        if (const auto* assistant = std::get_if<AssistantMessage>(&message)) {
            flush_orphans();
            if (assistant->stop_reason == AssistantStopReason::Error ||
                    assistant->stop_reason == AssistantStopReason::Aborted) {
                continue;
            }
            auto transformed = *assistant;
            transformed.content.clear();
            const bool same_model = assistant->provider == model.provider && assistant->api == model.api &&
                                    assistant->model == model.id;
            for (const auto& block : assistant->content) {
                if (const auto* thinking = std::get_if<ThinkingContent>(&block)) {
                    if (thinking->redacted) {
                        if (same_model) {
                            transformed.content.emplace_back(*thinking);
                        }
                    } else if (same_model && thinking->thinking_signature && !thinking->thinking_signature->empty()) {
                        transformed.content.emplace_back(*thinking);
                    } else if (!blank(thinking->thinking)) {
                        if (same_model) {
                            transformed.content.emplace_back(*thinking);
                        } else {
                            transformed.content.emplace_back(TextContent{
                                    .text = thinking->thinking,
                                    .text_signature = std::nullopt,
                            });
                        }
                    }
                    continue;
                }
                if (const auto* text = std::get_if<TextContent>(&block)) {
                    auto converted = *text;
                    if (!same_model) {
                        converted.text_signature = std::nullopt;
                    }
                    transformed.content.emplace_back(std::move(converted));
                    continue;
                }
                auto call = std::get<ToolCallContent>(block);
                if (!same_model) {
                    call.thought_signature = std::nullopt;
                }
                pending_calls.push_back(call);
                transformed.content.emplace_back(std::move(call));
            }
            result.emplace_back(std::move(transformed));
            continue;
        }

        if (const auto* tool_result = std::get_if<ToolResultMessage>(&message)) {
            auto transformed = *tool_result;
            transformed.content = downgrade_images(transformed.content, kToolImagePlaceholder, supports_images(model));
            result_ids.insert(transformed.tool_call_id);
            result.emplace_back(std::move(transformed));
            continue;
        }

        // An excluded message never enters the provider context, so it stays
        // invisible to the tool-call pairing state machine as well: pi's
        // `convertToLlm` drops it before the pairing transform ever sees the
        // sequence. Reaching the fallthrough below would flush a pending tool
        // call whose result arrives right after it, synthesizing a "No result
        // provided" orphan beside the real result (#668).
        if (excluded_from_provider_context(message)) {
            continue;
        }
        flush_orphans();
        if (const auto* user = std::get_if<UserMessage>(&message)) {
            auto transformed = *user;
            if (const auto* blocks = std::get_if<std::vector<Content>>(&transformed.content)) {
                // pi `downgradeUnsupportedImages` guards on `Array.isArray`:
                // the string alternative passes through untouched.
                transformed.content = downgrade_images(*blocks, kUserImagePlaceholder, supports_images(model));
            }
            result.emplace_back(std::move(transformed));
        } else if (auto converted = extended_message_to_user_message(message)) {
            result.emplace_back(std::move(*converted));
        }
    }
    flush_orphans();
    return result;
}

} // namespace cch::ai::api
