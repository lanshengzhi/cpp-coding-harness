#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace cch::harness::session {

/// The entries one append durably accepted, in wire order, plus the append's
/// own outcome. `entries` can be non-empty when `status` is an error: the
/// message append persists the message before its leaf marker, and a marker
/// failure must not hide the durable message from the live tree mirror.
struct AppendResult {
    std::vector<SessionEntry> entries;
    support::ExpectedVoid status;
};

/// Private JSONL persistence engine behind the SessionStore facade: owns the
/// session file journal, the wire encoding (through EntrySerializer), and
/// the in-process append-parent/leaf-marker discipline. Append operations
/// report the entries they durably wrote (parsed back from the exact wire
/// line) so the owning SessionStore can mirror them into its live
/// SessionTree without re-reading or re-parsing the session file.
class JsonlSessionStore final {
public:
    struct Impl;
public:
    JsonlSessionStore() = default;
    ~JsonlSessionStore();
    JsonlSessionStore(JsonlSessionStore&&);
    JsonlSessionStore& operator=(JsonlSessionStore&&);

    [[nodiscard]] static support::Expected<JsonlSessionStore> create_new(
        const std::filesystem::path& path,
        SessionMetadata metadata);
    [[nodiscard]] static support::Expected<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    /// Open for appending over an already-parsed session; the SessionStore
    /// facade loads once and shares the parse with its live tree.
    [[nodiscard]] static support::Expected<JsonlSessionStore> open_loaded(
        const std::filesystem::path& path,
        const LoadedSession& loaded);
    [[nodiscard]] static support::Expected<LoadedSession> load(const std::filesystem::path& path);

    /// Append one completed Session Entry message (pi `appendEntry`); when
    /// leaf-marker discipline is active the durable leaf marker follows in
    /// the same AppendResult.
    [[nodiscard]] AppendResult append(const ai::MessageVariant& message);

    // --- v3 tree entry append methods ---
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_active_tools_change(
        std::optional<std::string> parent_id,
        std::vector<std::string> tools);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_custom_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        support::JsonValue data);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_custom_message_entry(
        std::optional<std::string> parent_id,
        std::string custom_type,
        CustomMessageEntryContent content,
        bool display,
        std::optional<support::JsonValue> details);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_compaction(
        std::optional<std::string> parent_id,
        std::string summary,
        std::string first_kept_entry_id,
        std::size_t tokens_before,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook,
        std::vector<ai::MessageVariant> retained_tail = {},
        std::optional<ai::Usage> usage = std::nullopt);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_session_info(
        std::optional<std::string> parent_id,
        std::string name);

    /// Write a Leaf entry to persist the current active leaf position.
    /// The target_id is the entry ID that becomes the new leaf; nullopt
    /// writes the root leaf (pi `setLeafId(null)`, used after navigating
    /// back before the first entry).
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_leaf(
        std::optional<std::string> parent_id,
        std::optional<std::string> target_id);

    [[nodiscard]] std::optional<std::filesystem::path> path() const;
    [[nodiscard]] const SessionMetadata& metadata() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::harness::session
