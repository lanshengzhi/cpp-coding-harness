#pragma once

#include <cch/ai/Message.hpp>

#include "agent/harness/session/EntrySerializer.hpp"

#include <optional>
#include <string>

#include <cch/support/JsonValue.hpp>

namespace cch::harness::session {

/// The session-record redaction family (ADR 0026:23, ADR 0028). One rule per
/// free-text field; JSON details redact recursively through `redact_json_value`
/// with key-based secret replacement. Entry writers and the message mirror
/// share the same operations so the line and the live tree agree field for
/// field.
[[nodiscard]] support::JsonValue redact_json_value(const support::JsonValue& value);

void redact_content(ai::Content& content);

void redact_custom_message_entry_content(CustomMessageEntryContent& content);

void redact_assistant_content(ai::AssistantContent& content);

void redact_diagnostic_entry(ai::DiagnosticEntry& entry);

[[nodiscard]] ai::MessageVariant redacted_message(const ai::MessageVariant& message);

void redact_summary_entry_text(std::string& summary, std::optional<support::JsonValue>& details);

} // namespace cch::harness::session
