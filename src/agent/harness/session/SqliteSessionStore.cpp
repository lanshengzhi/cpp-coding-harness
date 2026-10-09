#include "agent/harness/session/SqliteSessionStore.hpp"
#include "agent/harness/session/EntrySerializer.hpp"

#include "support/Json.hpp"

#include <chrono>
#include <fstream>

namespace cch::agent::session {

namespace {

constexpr std::string_view kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS conversations (
    session_id TEXT PRIMARY KEY,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS entries (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    parent_id TEXT,
    entry_type TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES conversations(session_id)
);

CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id);
CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);

CREATE TABLE IF NOT EXISTS inbox (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('steer', 'follow_up', 'write')),
    payload TEXT NOT NULL,
    boundary_entry_id TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending', 'placed', 'done')),
    created_at INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES conversations(session_id)
);
CREATE INDEX IF NOT EXISTS idx_inbox_pending ON inbox(session_id, state, created_at, id);
)SQL";

} // namespace

support::Expected<SqliteSessionStore> SqliteSessionStore::open(const std::filesystem::path& db_path) {
    auto db = SqliteDatabase::open(db_path);
    if (!db) return std::unexpected(db.error());
    SqliteSessionStore store(std::move(*db));
    if (auto res = store.init_schema(); !res) return std::unexpected(res.error());
    return store;
}

support::Expected<SqliteSessionStore> SqliteSessionStore::open_memory() {
    auto db = SqliteDatabase::open_memory();
    if (!db) return std::unexpected(db.error());
    SqliteSessionStore store(std::move(*db));
    if (auto res = store.init_schema(); !res) return std::unexpected(res.error());
    return store;
}

support::ExpectedVoid SqliteSessionStore::init_schema() { return db_.execute(kSchemaSql); }

support::ExpectedVoid SqliteSessionStore::create_conversation(std::string_view session_id) {
    const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    auto stmt = db_.prepare("INSERT INTO conversations (session_id, created_at, updated_at) "
                            "VALUES (?1, ?2, ?2) ON CONFLICT(session_id) DO NOTHING;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, session_id); !result) return result;
    if (auto result = stmt->bind_int64(2, now); !result) return result;
    if (auto step = stmt->step(); !step) return std::unexpected(step.error());
    return {};
}

support::ExpectedVoid SqliteSessionStore::append_entry(std::string_view session_id,
        std::string_view entry_id,
        std::optional<std::string_view> parent_id,
        std::string_view type,
        std::string_view payload_json) {
    const EntryPayload entry{
            .entry_id = std::string(entry_id),
            .parent_id = parent_id ? std::optional<std::string>(*parent_id) : std::nullopt,
            .type = std::string(type),
            .payload_json = std::string(payload_json),
    };
    return append_batch(session_id, std::vector<EntryPayload>{entry});
}

support::ExpectedVoid SqliteSessionStore::append_batch(
        std::string_view session_id, const std::vector<EntryPayload>& entries) {
    return commit_batch_with_inbox(session_id, entries, {});
}

support::ExpectedVoid SqliteSessionStore::commit_batch_with_inbox(std::string_view session_id,
        const std::vector<EntryPayload>& entries,
        const std::vector<InboxPayload>& inbox,
        bool fail_after_entries) {
    const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();

    SqliteTransactionGuard txn(db_);
    if (auto res = db_.begin_transaction(); !res) return res;

    auto conv = db_.prepare("INSERT INTO conversations (session_id, created_at, updated_at) "
                            "VALUES (?1, ?2, ?2) "
                            "ON CONFLICT(session_id) DO UPDATE SET updated_at = ?2;");
    if (!conv) return std::unexpected(conv.error());
    if (auto result = conv->bind_text(1, session_id); !result) return result;
    if (auto result = conv->bind_int64(2, now); !result) return result;
    if (auto step = conv->step(); !step) return std::unexpected(step.error());

    for (const auto& entry : entries) {
        auto entry_stmt =
                db_.prepare("INSERT INTO entries (id, session_id, parent_id, entry_type, payload, created_at) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
        if (!entry_stmt) return std::unexpected(entry_stmt.error());
        if (auto result = entry_stmt->bind_text(1, entry.entry_id); !result) return result;
        if (auto result = entry_stmt->bind_text(2, session_id); !result) return result;
        if (entry.parent_id) {
            if (auto result = entry_stmt->bind_text(3, *entry.parent_id); !result) return result;
        } else if (auto result = entry_stmt->bind_null(3); !result) {
            return result;
        }
        if (auto result = entry_stmt->bind_text(4, entry.type); !result) return result;
        if (auto result = entry_stmt->bind_text(5, entry.payload_json); !result) return result;
        if (auto result = entry_stmt->bind_int64(6, now); !result) return result;
        if (auto step = entry_stmt->step(); !step) return std::unexpected(step.error());
    }
    if (fail_after_entries) {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "injected failure after entry write"));
    }

    for (const auto& item : inbox) {
        auto inbox_stmt =
                db_.prepare("INSERT INTO inbox (id, session_id, kind, payload, boundary_entry_id, state, created_at) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, 'pending', ?6);");
        if (!inbox_stmt) return std::unexpected(inbox_stmt.error());
        const auto kind = item.kind == InboxKind::Steer      ? "steer"
                          : item.kind == InboxKind::FollowUp ? "follow_up"
                                                             : "write";
        if (auto result = inbox_stmt->bind_text(1, item.id); !result) return result;
        if (auto result = inbox_stmt->bind_text(2, session_id); !result) return result;
        if (auto result = inbox_stmt->bind_text(3, kind); !result) return result;
        if (auto result = inbox_stmt->bind_text(4, item.payload); !result) return result;
        if (auto result = inbox_stmt->bind_text(5, item.boundary_entry_id); !result) return result;
        if (auto result = inbox_stmt->bind_int64(6, now); !result) return result;
        if (auto step = inbox_stmt->step(); !step) return std::unexpected(step.error());
    }
    return txn.commit();
}

support::ExpectedVoid SqliteSessionStore::enqueue_inbox(std::string_view session_id, InboxPayload inbox) {
    return commit_batch_with_inbox(session_id, {}, {inbox});
}

support::Expected<std::vector<SqliteSessionStore::InboxItem>> SqliteSessionStore::claim_pending_inbox(
        std::string_view session_id) {
    SqliteTransactionGuard txn(db_);
    if (auto begun = db_.begin_transaction(); !begun) return std::unexpected(begun.error());
    auto stmt = db_.prepare("UPDATE inbox SET state = 'placed' WHERE id IN ("
                            "SELECT id FROM inbox WHERE session_id = ?1 AND state = 'pending' ORDER BY created_at, id"
                            ") AND state = 'pending' RETURNING id, kind, payload, boundary_entry_id, created_at;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto bound = stmt->bind_text(1, session_id); !bound) return std::unexpected(bound.error());
    std::vector<InboxItem> claimed;
    while (true) {
        auto row = stmt->step();
        if (!row) return std::unexpected(row.error());
        if (!*row) break;
        const auto kind = stmt->column_text(1);
        claimed.push_back({.id = stmt->column_text(0),
                .kind = kind == "steer"       ? InboxKind::Steer
                        : kind == "follow_up" ? InboxKind::FollowUp
                                              : InboxKind::Write,
                .payload = stmt->column_text(2),
                .boundary_entry_id = stmt->column_text(3),
                .created_at = stmt->column_int64(4)});
    }
    if (auto committed = txn.commit(); !committed) return std::unexpected(committed.error());
    return claimed;
}

support::Expected<std::shared_ptr<SessionTree>> SqliteSessionStore::load_session_tree(std::string_view session_id) {
    auto stmt = db_.prepare("SELECT payload FROM entries WHERE session_id = ?1 ORDER BY rowid ASC;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto bound = stmt->bind_text(1, session_id); !bound) return std::unexpected(bound.error());

    std::vector<std::string> lines;
    // Add dummy header to satisfy parser
    auto header_stmt =
            db_.prepare("SELECT entry_type, payload FROM entries WHERE session_id = ?1 ORDER BY rowid ASC LIMIT 1;");
    if (!header_stmt) return std::unexpected(header_stmt.error());
    if (auto bound = header_stmt->bind_text(1, session_id); !bound) return std::unexpected(bound.error());
    auto has_header = header_stmt->step();
    if (!has_header) return std::unexpected(has_header.error());
    if (*has_header && header_stmt->column_text(0) == "session") {
        lines.push_back(header_stmt->column_text(1));
    } else {
        lines.push_back(std::format(
                R"({{"type":"session","version":3,"id":"{}","timestamp":"2026-10-09T00:00:00.000Z","cwd":"/tmp"}})",
                session_id));
    }

    while (true) {
        auto has_row = stmt->step();
        if (!has_row) return std::unexpected(has_row.error());
        if (!*has_row) break;

        lines.push_back(stmt->column_text(0));
    }

    cch::harness::session::EntrySerializer serializer;
    auto parsed = serializer.parse_lines(lines);
    if (!parsed) return std::unexpected(parsed.error());

    return std::make_shared<SessionTree>(std::move(*parsed));
}

support::Expected<SessionMetadata> SqliteSessionStore::load_metadata(std::string_view session_id) {
    auto stmt =
            db_.prepare("SELECT entry_type, payload FROM entries WHERE session_id = ?1 ORDER BY rowid ASC LIMIT 1;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto bound = stmt->bind_text(1, session_id); !bound) return std::unexpected(bound.error());
    auto has_row = stmt->step();
    if (!has_row) return std::unexpected(has_row.error());
    if (!*has_row) {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "SQLite session does not exist"));
    }
    if (stmt->column_text(0) != "session") {
        return std::unexpected(support::make_error(support::ErrorCode::Session, "SQLite session header is missing"));
    }
    std::vector<std::string> header{stmt->column_text(1)};
    cch::harness::session::EntrySerializer serializer;
    auto parsed = serializer.parse_lines(header);
    if (!parsed) return std::unexpected(parsed.error());
    return parsed->metadata;
}

support::ExpectedVoid SqliteSessionStore::import_jsonl(const std::filesystem::path& jsonl_path) {
    std::ifstream file(jsonl_path);
    if (!file.is_open()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process, "Cannot open JSONL file for import"));
    }

    std::string line;
    std::string session_id;
    struct StagedEntry {
        std::string id;
        std::optional<std::string> parent_id;
        std::string type;
        std::string json;
    };
    std::vector<StagedEntry> staged;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto parsed = support::read_json(line);
        if (!parsed || !parsed->holds<support::JsonValue::object_t>()) continue;
        const auto& obj = parsed->get<support::JsonValue::object_t>();

        if (session_id.empty()) {
            if (auto it = obj.find("sessionId"); it != obj.end() && it->second.holds<std::string>()) {
                session_id = it->second.get_string();
            }
        }
        std::string entry_id;
        if (auto it = obj.find("id"); it != obj.end() && it->second.holds<std::string>()) {
            entry_id = it->second.get_string();
        }
        std::optional<std::string> parent_id;
        if (auto it = obj.find("parentId"); it != obj.end() && it->second.holds<std::string>()) {
            parent_id = it->second.get_string();
        }
        std::string type;
        if (auto it = obj.find("type"); it != obj.end() && it->second.holds<std::string>()) {
            type = it->second.get_string();
        }

        if (!entry_id.empty()) {
            staged.push_back({.id = entry_id, .parent_id = parent_id, .type = type, .json = line});
        }
    }

    if (session_id.empty()) {
        session_id = jsonl_path.stem().string();
    }

    SqliteTransactionGuard txn(db_);
    if (auto res = db_.begin_transaction(); !res) return res;

    for (const auto& item : staged) {
        if (auto res = append_entry(session_id, item.id, item.parent_id, item.type, item.json); !res) {
            return res;
        }
    }
    return txn.commit();
}

} // namespace cch::agent::session
