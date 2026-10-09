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

support::ExpectedVoid SqliteSessionStore::append_entry(std::string_view session_id,
        std::string_view entry_id,
        std::optional<std::string_view> parent_id,
        std::string_view type,
        std::string_view payload_json) {
    const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();

    SqliteTransactionGuard txn(db_);
    if (auto res = db_.begin_transaction(); !res) return res;

    // Ensure conversation exists
    auto conv_stmt = db_.prepare("INSERT INTO conversations (session_id, created_at, updated_at) "
                                 "VALUES (?1, ?2, ?2) "
                                 "ON CONFLICT(session_id) DO UPDATE SET updated_at = ?2;");
    if (!conv_stmt) return std::unexpected(conv_stmt.error());
    (void)conv_stmt->bind_text(1, session_id);
    (void)conv_stmt->bind_int64(2, now);
    if (auto step = conv_stmt->step(); !step) return std::unexpected(step.error());

    // Insert entry
    auto entry_stmt = db_.prepare("INSERT INTO entries (id, session_id, parent_id, entry_type, payload, created_at) "
                                  "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
    if (!entry_stmt) return std::unexpected(entry_stmt.error());
    (void)entry_stmt->bind_text(1, entry_id);
    (void)entry_stmt->bind_text(2, session_id);
    if (parent_id)
        (void)entry_stmt->bind_text(3, *parent_id);
    else
        (void)entry_stmt->bind_null(3);
    (void)entry_stmt->bind_text(4, type);
    (void)entry_stmt->bind_text(5, payload_json);
    (void)entry_stmt->bind_int64(6, now);

    if (auto step = entry_stmt->step(); !step) return std::unexpected(step.error());
    return txn.commit();
}

support::Expected<std::shared_ptr<SessionTree>> SqliteSessionStore::load_session_tree(std::string_view session_id) {
    auto stmt = db_.prepare("SELECT payload FROM entries WHERE session_id = ?1 ORDER BY created_at ASC;");
    if (!stmt) return std::unexpected(stmt.error());
    (void)stmt->bind_text(1, session_id);

    std::vector<std::string> lines;
    // Add dummy header to satisfy parser
    lines.push_back(std::format(
            R"({{"type":"session","version":3,"id":"{}","timestamp":"2026-10-09T00:00:00.000Z","cwd":"/tmp"}})",
            session_id));

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
