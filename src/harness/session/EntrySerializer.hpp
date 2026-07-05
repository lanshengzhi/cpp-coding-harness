#pragma once

#include "cch/harness/session/SessionEntry.hpp"

#include "cch/ai/Message.hpp"
#include "cch/util/Error.hpp"
#include "cch/util/JsonValue.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

/// Private adapter that converts between session entry domain objects and
/// JSONL lines. EntrySerializer owns the DTOs, redaction rules, and parsing
/// logic; JsonlSessionStore only coordinates file I/O through SessionJournal.
class EntrySerializer {
public:
    struct SerializedMessageEntry {
        std::string line;
        std::string entry_id;
    };

    [[nodiscard]] util::Expected<std::string> serialize_header(const SessionMetadata& metadata) const;

    [[nodiscard]] util::Expected<LoadedSession> parse_lines(const std::vector<std::string>& lines) const;

    [[nodiscard]] util::Expected<std::string> serialize_message(const ai::MessageVariant& message) const;
    [[nodiscard]] util::Expected<SerializedMessageEntry> serialize_message_entry(
        const ai::MessageVariant& message,
        std::optional<std::string> parent_id) const;

    [[nodiscard]] util::Expected<std::string> serialize_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id) const;

    [[nodiscard]] util::Expected<std::string> serialize_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level) const;

    [[nodiscard]] util::Expected<std::string> serialize_active_tools_change(
        std::optional<std::string> parent_id,
        std::vector<std::string> tools) const;

    [[nodiscard]] util::Expected<std::string> serialize_custom_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        util::JsonValue data) const;

    [[nodiscard]] util::Expected<std::string> serialize_custom_message_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        std::string content,
        bool display,
        std::optional<util::JsonValue> details) const;

    [[nodiscard]] util::Expected<std::string> serialize_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label) const;

    [[nodiscard]] util::Expected<std::string> serialize_compaction(
        std::optional<std::string> parent_id,
        std::string summary,
        std::string first_kept_entry_id,
        std::size_t tokens_before,
        std::optional<util::JsonValue> details,
        std::optional<bool> from_hook) const;

    [[nodiscard]] util::Expected<std::string> serialize_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<util::JsonValue> details,
        std::optional<bool> from_hook) const;

    [[nodiscard]] util::Expected<std::string> serialize_session_info(
        std::optional<std::string> parent_id,
        std::string name) const;

    [[nodiscard]] util::Expected<std::string> serialize_leaf(
        std::optional<std::string> parent_id,
        std::string target_id) const;
};

} // namespace cch::harness::session
