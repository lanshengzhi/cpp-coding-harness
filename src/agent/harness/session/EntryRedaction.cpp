#include "agent/harness/session/EntryRedaction.hpp"

#include "support/Json.hpp"
#include "support/Redactor.hpp"

#include <cch/ai/Content.hpp>

#include <utility>
#include <variant>

namespace cch::harness::session {

[[nodiscard]] support::JsonValue redact_json_value(const support::JsonValue& value) {
    if (const auto* text = value.get_if<std::string>()) {
        return support::JsonValue{support::redact_text(*text)};
    }
    if (const auto* values = value.get_if<support::JsonValue::array_t>()) {
        support::JsonValue::array_t redacted;
        redacted.reserve(values->size());
        for (const auto& item : *values) {
            redacted.push_back(redact_json_value(item));
        }
        return support::JsonValue{std::move(redacted)};
    }
    if (const auto* object = value.get_if<support::JsonValue::object_t>()) {
        support::JsonValue::object_t redacted;
        for (const auto& [key, item] : *object) {
            if (support::looks_secret_key(key)) {
                redacted.emplace(key, support::JsonValue{std::string{support::kRedactionMarker}});
            } else {
                redacted.emplace(key, redact_json_value(item));
            }
        }
        return support::JsonValue{std::move(redacted)};
    }
    return value;
}

void redact_content(ai::Content& content) {
    std::visit(
        [](auto& block) {
            using T = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<T, ai::TextContent>) {
                block.text = support::redact_text(std::move(block.text));
            } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                block.thinking = support::redact_text(std::move(block.thinking));
            }
        },
        content);
}

void redact_custom_message_entry_content(CustomMessageEntryContent& content) {
    if (auto* text = std::get_if<std::string>(&content)) {
        *text = support::redact_text(std::move(*text));
        return;
    }

    for (auto& block : std::get<std::vector<CustomMessageEntryContentBlock>>(content)) {
        if (auto* text = std::get_if<ai::TextContent>(&block)) {
            text->text = support::redact_text(std::move(text->text));
        }
    }
}

void redact_assistant_content(ai::AssistantContent& content) {
    std::visit(
        [](auto& block) {
            using T = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<T, ai::TextContent>) {
                block.text = support::redact_text(std::move(block.text));
            } else if constexpr (std::is_same_v<T, ai::ThinkingContent>) {
                block.thinking = support::redact_text(std::move(block.thinking));
            } else if constexpr (std::is_same_v<T, ai::ToolCallContent>) {
                if (block.arguments) {
                    block.arguments = redact_json_value(*block.arguments);
                }
                block.raw_arguments = support::redact_text(std::move(block.raw_arguments));
                if (block.argument_error) {
                    block.argument_error = support::redact_text(std::move(*block.argument_error));
                }
            }
        },
        content);
}

void redact_diagnostic_entry(ai::DiagnosticEntry& entry) {
    entry.type = support::redact_text(std::move(entry.type));
    if (entry.error) {
        auto& error = *entry.error;
        if (error.name) {
            *error.name = support::redact_text(std::move(*error.name));
        }
        error.message = support::redact_text(std::move(error.message));
        if (error.stack) {
            *error.stack = support::redact_text(std::move(*error.stack));
        }
        if (error.code) {
            *error.code = support::redact_text(std::move(*error.code));
        }
    }
    if (entry.details) {
        entry.details = redact_json_value(*entry.details);
    }
}

[[nodiscard]] ai::MessageVariant redacted_message(const ai::MessageVariant& message) {
    auto redacted = message;
    std::visit(
        [](auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, ai::SystemMessage>) {
                concrete.content = support::redact_text(std::move(concrete.content));
            } else if constexpr (std::is_same_v<T, ai::AssistantMessage>) {
                for (auto& block : concrete.content) {
                    redact_assistant_content(block);
                }
                if (concrete.error_message) {
                    concrete.error_message = support::redact_text(std::move(*concrete.error_message));
                }
                // Provider/tool diagnostics carry free-form error text; they
                // were silently omitted before #666. ADR 0028 narrows the raw
                // rule to User Bash text only and leaves non-User-Bash
                // diagnostics under ADR 0026:23's mandatory redaction.
                if (concrete.diagnostics) {
                    for (auto& entry : *concrete.diagnostics) {
                        redact_diagnostic_entry(entry);
                    }
                }
            } else if constexpr (std::is_same_v<T, ai::BashExecutionMessage>) {
                // Both User Bash text values are stored raw, so this branch
                // carries no redaction. `command` (ADR 0028:30, re-proposal
                // rejected) and `output` (ADR 0028's Output section, "No
                // redaction", adjudicated in #677 option 甲 and applied here
                // under #679).
            } else if constexpr (std::is_same_v<T, ai::CustomMessage>) {
                for (auto& block : concrete.content) {
                    redact_content(block);
                }
                if (concrete.details) {
                    concrete.details = redact_json_value(*concrete.details);
                }
            } else if constexpr (std::is_same_v<T, ai::BranchSummaryMessage>) {
                // Summaries are model-generated from context that can contain
                // raw User Bash text (ADR 0028) and stored model/user content,
                // so "already plain" does not hold and they are redacted like
                // any other conversational text (#666). The separate
                // compaction/branch-summary *entry* writers build their DTOs
                // directly instead of through this function, so they carry the
                // same rule in `redact_summary_entry_text` (#675).
                concrete.summary = support::redact_text(std::move(concrete.summary));
            } else if constexpr (std::is_same_v<T, ai::CompactionSummaryMessage>) {
                // Same rationale as BranchSummaryMessage above.
                concrete.summary = support::redact_text(std::move(concrete.summary));
            } else if constexpr (std::is_same_v<T, ai::UserMessage>) {
                if (auto* text = std::get_if<std::string>(&concrete.content)) {
                    *text = support::redact_text(std::move(*text));
                } else {
                    for (auto& block :
                         std::get<std::vector<ai::Content>>(concrete.content)) {
                        redact_content(block);
                    }
                }
            } else {
                // ToolResultMessage
                for (auto& block : concrete.content) {
                    redact_content(block);
                }
                if constexpr (std::is_same_v<T, ai::ToolResultMessage>) {
                    if (concrete.details) {
                        concrete.details = redact_json_value(*concrete.details);
                    }
                }
            }
        },
        redacted);
    return redacted;
}

/// The free-text pair a summary-bearing entry value carries.
///
/// The compaction and branch-summary *entry* writers build their DTOs straight
/// from these values rather than through `redacted_message`, so before #675
/// both fields reached the pi v3 wire as written. The routing is the one the
/// message path already uses — `support::redact_text` for free text,
/// `redact_json_value` for JSON — so the two paths agree field for field.
///
/// Authority: #675 (the entry writers bypassed redaction altogether) under
/// ADR 0026:23, which keeps non-User-Bash text redaction mandatory. Neither
/// field is exempt and neither is User Bash text, so no exemption applies.
void redact_summary_entry_text(std::string& summary, std::optional<support::JsonValue>& details) {
    summary = support::redact_text(std::move(summary));
    if (details) {
        details = redact_json_value(*details);
    }
}

} // namespace cch::harness::session
