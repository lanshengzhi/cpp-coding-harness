#pragma once

#include "agent/harness/session/SqliteDatabase.hpp"
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::agent::session {

using cch::harness::session::SessionMetadata;
using cch::harness::session::SessionTree;

/// Session store implemented on SQLite, mirroring pi-durable's relational backing.
class SqliteSessionStore final {
public:
    struct EntryPayload {
        std::string entry_id;
        std::optional<std::string> parent_id;
        std::string type;
        std::string payload_json;
    };

    enum class InboxKind { Steer, FollowUp, Write };

    struct InboxPayload {
        std::string id;
        InboxKind kind{InboxKind::Steer};
        std::string payload;
        std::string boundary_entry_id;
    };

    struct InboxItem {
        std::string id;
        InboxKind kind{InboxKind::Steer};
        std::string payload;
        std::string boundary_entry_id;
        std::int64_t created_at{0};
    };

    explicit SqliteSessionStore(SqliteDatabase db) : db_(std::move(db)) {}

    /// Open or create the database and apply schema migrations.
    [[nodiscard]] static support::Expected<SqliteSessionStore> open(const std::filesystem::path& db_path);

    /// In-memory database instance (test utility).
    [[nodiscard]] static support::Expected<SqliteSessionStore> open_memory();

    /// Initialize tables (conversations, entries).
    [[nodiscard]] support::ExpectedVoid init_schema();

    /// Append one session entry record in JSON wire format.
    [[nodiscard]] support::ExpectedVoid append_entry(std::string_view session_id,
            std::string_view entry_id,
            std::optional<std::string_view> parent_id,
            std::string_view type,
            std::string_view payload_json);

    /// Ensure one conversation exists, including before its first entry.
    [[nodiscard]] support::ExpectedVoid create_conversation(std::string_view session_id);

    /// Append all entries in one transaction; a failure rolls back the complete batch.
    [[nodiscard]] support::ExpectedVoid append_batch(
            std::string_view session_id, const std::vector<EntryPayload>& entries);

    /// Commit session entries and their admission inbox records atomically.
    [[nodiscard]] support::ExpectedVoid commit_batch_with_inbox(std::string_view session_id,
            const std::vector<EntryPayload>& entries,
            const std::vector<InboxPayload>& inbox,
            bool fail_after_entries = false);
    /// Persist an inbox item without an associated admission batch.
    [[nodiscard]] support::ExpectedVoid enqueue_inbox(std::string_view session_id, InboxPayload inbox);
    /// Claim all pending inbox items in creation order, transitioning them once.
    [[nodiscard]] support::Expected<std::vector<InboxItem>> claim_pending_inbox(std::string_view session_id);

    /// Load the full SessionTree for a conversation.
    [[nodiscard]] support::Expected<std::shared_ptr<SessionTree>> load_session_tree(std::string_view session_id);
    [[nodiscard]] support::Expected<SessionMetadata> load_metadata(std::string_view session_id);

    /// Import an entire existing JSONL transcript file atomically into SQLite.
    [[nodiscard]] support::ExpectedVoid import_jsonl(const std::filesystem::path& jsonl_path);

private:
    SqliteDatabase db_;
};

} // namespace cch::agent::session
