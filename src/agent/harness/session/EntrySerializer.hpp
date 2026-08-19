#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

/// Private adapter that converts between session entry domain objects and
/// JSONL lines. EntrySerializer owns the DTOs, redaction rules, and parsing
/// logic; JsonlSessionStore only coordinates file I/O through SessionJournal.
class EntrySerializer {
public:
    /// One serialized append: the wire line plus the SessionEntry the line
    /// represents. The entry is built from the same generated id/timestamp
    /// and (redacted) values as the line, so the owning store's live tree
    /// mirrors the wire without re-reading the file or round-tripping the
    /// line back through the (stricter) reader.
    struct SerializationResult {
        std::string line;
        SessionEntry entry;
    };

    [[nodiscard]] support::Expected<std::string> serialize_header(const SessionMetadata& metadata) const;

    [[nodiscard]] support::Expected<LoadedSession> parse_lines(const std::vector<std::string>& lines) const;

    /// Parse one persisted non-header line back into its SessionEntry.
    /// Header lines are rejected (they never appear mid-file).
    [[nodiscard]] support::Expected<SessionEntry> parse_entry(
        std::string_view line,
        std::size_t line_number) const;

    [[nodiscard]] support::Expected<std::string> serialize_message(const ai::MessageVariant& message) const;
    [[nodiscard]] support::Expected<SerializationResult> serialize_message_entry(
        const ai::MessageVariant& message,
        std::optional<std::string> parent_id) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_active_tools_change(
        std::optional<std::string> parent_id,
        std::vector<std::string> tools) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_custom_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        std::optional<support::JsonValue> data) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_custom_message_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        CustomMessageEntryContent content,
        bool display,
        std::optional<support::JsonValue> details) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_compaction(
        std::optional<std::string> parent_id,
        std::string summary,
        std::optional<std::string> first_kept_entry_id,
        std::size_t tokens_before,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::vector<ai::MessageVariant> retained_tail = {},
        std::optional<ai::Usage> usage = std::nullopt) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::optional<ai::Usage> usage = std::nullopt) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_session_info(
        std::optional<std::string> parent_id,
        std::optional<std::string> name) const;

    [[nodiscard]] support::Expected<SerializationResult> serialize_leaf(
        std::optional<std::string> parent_id,
        std::optional<std::string> target_id) const;

    /// Fresh 8-char hex entry id (pi `generateId`). Exposed for the
    /// in-memory store, which builds entries without serializing a line.
    [[nodiscard]] static std::string new_entry_id();

    /// Current time as epoch milliseconds (the entry timestamp domain).
    [[nodiscard]] static ai::TimestampMs now_timestamp_ms();

    /// Round-trip writer: re-emit a parsed `SessionEntry` as its pi wire line
    /// (byte-identical to the source line for pi-captured files) using the
    /// stored entry id/timestamp. Header and Unknown entries are rejected.
    [[nodiscard]] support::Expected<std::string> serialize_entry(const SessionEntry& entry) const;
};

} // namespace cch::harness::session
