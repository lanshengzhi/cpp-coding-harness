#include <cch/durable/SqliteStorage.hpp>

#include <cch/chord/Delta.hpp>
#include <cch/support/JsonAccess.hpp>
#include <cch/support/JsonValue.hpp>

#include "StorageEncoding.hpp"
#include "StorageState.hpp"
#include "support/Json.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {

constexpr std::string_view kClosedMessage = "SQLite storage is closed";
constexpr std::string_view kPoisonMessage = "SQLite storage is poisoned and must be reopened";

[[nodiscard]] support::Error database_error(sqlite3* database, std::string_view operation) {
    const int code = database == nullptr ? SQLITE_ERROR : sqlite3_errcode(database);
    const std::string detail = database == nullptr ? "SQLite database is unavailable" : sqlite3_errmsg(database);
    const bool busy = code == SQLITE_BUSY || code == SQLITE_LOCKED;
    return support::make_error(busy ? support::ErrorCode::Busy : support::ErrorCode::Validation,
            std::format("{} failed", operation),
            detail);
}

[[nodiscard]] support::Error corrupt(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

[[nodiscard]] bool valid_prefix(std::string_view prefix) {
    if (prefix.empty()) return true;
    if (!((prefix.front() >= 'a' && prefix.front() <= 'z') || (prefix.front() >= 'A' && prefix.front() <= 'Z') ||
                prefix.front() == '_')) {
        return false;
    }
    const bool identifier = std::all_of(prefix.begin() + 1, prefix.end(), [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
               value == '_';
    });
    std::string lowercase{prefix.substr(0, std::min<std::size_t>(prefix.size(), 7))};
    std::ranges::transform(lowercase, lowercase.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    });
    return identifier && !lowercase.starts_with("sqlite_");
}

[[nodiscard]] std::string quote_identifier(std::string_view identifier) {
    std::string result{"\""};
    for (const char value : identifier) {
        if (value == '"') result += '"';
        result += value;
    }
    result += '"';
    return result;
}

[[nodiscard]] std::string table(std::string_view prefix, std::string_view name) {
    return quote_identifier(std::format("{}{}", prefix, name));
}

[[nodiscard]] std::string as_string(const chord::Json& value) {
    auto serialized = support::write_json(value.to_support());
    return serialized ? std::move(*serialized) : std::string{};
}

[[nodiscard]] std::string ops_json(const chord::OpBatch& ops) {
    auto text = support::write_json(ops_value(ops));
    return text ? std::move(*text) : std::string{"[]"};
}

[[nodiscard]] support::Expected<chord::Json> parse_json(std::string_view text) {
    auto value = support::read_json(text);
    if (!value) return std::unexpected(std::move(value.error()));
    return chord::Json::from_support(*value);
}

[[nodiscard]] std::optional<std::int64_t> safe_sqlite_integer(double value) {
    if (!std::isfinite(value) || std::trunc(value) != value || value < 0 ||
            value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return static_cast<std::int64_t>(value);
}

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql) {
        if (database == nullptr) return;
        const int prepared =
                sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
        if (prepared != SQLITE_OK) {
            error_ = database_error(database, "SQLite prepare");
            statement_ = nullptr;
        }
    }

    Statement(Statement&& other) noexcept
        : statement_(std::exchange(other.statement_, nullptr)), error_(std::move(other.error_)) {}
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (statement_ != nullptr) sqlite3_finalize(statement_);
            statement_ = std::exchange(other.statement_, nullptr);
            error_ = std::move(other.error_);
        }
        return *this;
    }

    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] bool valid() const { return statement_ != nullptr; }
    [[nodiscard]] sqlite3_stmt* get() const { return statement_; }
    [[nodiscard]] support::Error error() const { return error_; }

    [[nodiscard]] support::ExpectedVoid bind_int(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            return std::unexpected(database_error(sqlite3_db_handle(statement_), "SQLite bind"));
        }
        return {};
    }

    [[nodiscard]] support::ExpectedVoid bind_text(int index, std::string_view text) {
        if (sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) !=
                SQLITE_OK) {
            return std::unexpected(database_error(sqlite3_db_handle(statement_), "SQLite bind"));
        }
        return {};
    }

    [[nodiscard]] support::ExpectedVoid bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            return std::unexpected(database_error(sqlite3_db_handle(statement_), "SQLite bind"));
        }
        return {};
    }

    [[nodiscard]] support::Expected<bool> step() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        return std::unexpected(database_error(sqlite3_db_handle(statement_), "SQLite step"));
    }

    [[nodiscard]] support::ExpectedVoid reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK) {
            return std::unexpected(database_error(sqlite3_db_handle(statement_), "SQLite reset"));
        }
        return {};
    }

private:
    sqlite3_stmt* statement_{nullptr};
    support::Error error_{support::make_error(support::ErrorCode::Validation, "Statement uninitialized")};
};

[[nodiscard]] support::ExpectedVoid execute(sqlite3* database, std::string_view sql) {
    char* errmsg = nullptr;
    const int result = sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &errmsg);
    if (result == SQLITE_OK) return {};
    std::string detail = errmsg != nullptr ? errmsg : "Unknown SQLite error";
    sqlite3_free(errmsg);
    const bool busy = result == SQLITE_BUSY || result == SQLITE_LOCKED;
    return std::unexpected(support::make_error(
            busy ? support::ErrorCode::Busy : support::ErrorCode::Validation, "SQLite execution failed", detail));
}

} // namespace

struct SqliteStorage::Impl {
    Impl(sqlite3* database, SqliteStorageOptions options)
        : database(database), options(std::move(options)), state(std::string{kClosedMessage}) {}

    ~Impl() {
        if (database != nullptr) sqlite3_close_v2(database);
    }

    sqlite3* database{};
    SqliteStorageOptions options;
    StorageState state;
    bool poisoned{false};

    [[nodiscard]] std::optional<support::Error> unusable() const {
        if (poisoned) return support::make_error(support::ErrorCode::Validation, std::string{kPoisonMessage});
        if (state.closed()) return support::make_error(support::ErrorCode::Session, std::string{kClosedMessage});
        return std::nullopt;
    }

    [[nodiscard]] support::ExpectedVoid configure() {
        if (options.busy_timeout_ms < 0 || options.wal_auto_checkpoint_pages < 0) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "SQLite options must be nonnegative"));
        }
        if (sqlite3_busy_timeout(database, options.busy_timeout_ms) != SQLITE_OK) {
            return std::unexpected(database_error(database, "SQLite busy timeout"));
        }
        if (auto mode = execute(database, "PRAGMA journal_mode=WAL"); !mode) return mode;
        return execute(database, std::format("PRAGMA wal_autocheckpoint={}", options.wal_auto_checkpoint_pages));
    }

    [[nodiscard]] support::ExpectedVoid create_schema() {
        const std::string schema = table(options.prefix, "durable_schema");
        const std::string schema_name = std::format("{}durable_schema", options.prefix);
        Statement lookup{database, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1"};
        if (!lookup.valid()) return std::unexpected(lookup.error());
        if (auto bound = lookup.bind_text(1, schema_name); !bound) return std::unexpected(std::move(bound.error()));
        auto exists = lookup.step();
        if (!exists) return std::unexpected(std::move(exists.error()));
        if (*exists) {
            Statement current{database, std::format("SELECT version FROM {} WHERE singleton=1", schema)};
            if (!current.valid()) return std::unexpected(current.error());
            auto row = current.step();
            if (!row) return std::unexpected(std::move(row.error()));
            if (!*row) return std::unexpected(corrupt("Durable SQLite schema metadata is missing"));
            const std::int64_t version = sqlite3_column_int64(current.get(), 0);
            if (version > 1) {
                return std::unexpected(corrupt(
                        std::format("Durable SQLite schema version {} is newer than supported version 1", version)));
            }
            if (version == 1) return {};
        }

        if (auto begin = execute(database, "BEGIN IMMEDIATE"); !begin) return begin;
        const auto fail = [this](support::Error error) -> support::ExpectedVoid {
            (void)execute(database, "ROLLBACK");
            return std::unexpected(std::move(error));
        };
        const std::vector<std::string> definitions{
                std::format("CREATE TABLE IF NOT EXISTS {} (singleton INTEGER PRIMARY KEY CHECK(singleton=1), version "
                            "INTEGER NOT NULL CHECK(version>=0)) STRICT",
                        schema),
                std::format("INSERT OR IGNORE INTO {} (singleton, version) VALUES(1,0)", schema),
                std::format("CREATE TABLE IF NOT EXISTS {} (singleton INTEGER PRIMARY KEY CHECK(singleton=1), next_id "
                            "TEXT NOT NULL, next_seq INTEGER NOT NULL) STRICT",
                        table(options.prefix, "durable_metadata")),
                std::format("INSERT OR IGNORE INTO {} (singleton, next_id, next_seq) VALUES(1,'2',1)",
                        table(options.prefix, "durable_metadata")),
                std::format("CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, record_type TEXT NOT NULL "
                            "CHECK(record_type IN ('conversation','entry','task','submission','document'))) STRICT",
                        table(options.prefix, "record_ids")),
                std::format("CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, owner_conversation_id INTEGER, "
                            "owner_task_id INTEGER, record TEXT NOT NULL CHECK(json_valid(record))) STRICT",
                        table(options.prefix, "conversations")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(owner_conversation_id, id)",
                        quote_identifier(std::format("{}conversations_by_owner_conversation", options.prefix)),
                        table(options.prefix, "conversations")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(owner_task_id, id)",
                        quote_identifier(std::format("{}conversations_by_owner_task", options.prefix)),
                        table(options.prefix, "conversations")),
                std::format(
                        "CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, head "
                        "INTEGER, commit_seq INTEGER NOT NULL, record TEXT NOT NULL CHECK(json_valid(record))) STRICT",
                        table(options.prefix, "entries")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(conversation_id, id DESC)",
                        quote_identifier(std::format("{}entries_by_conversation", options.prefix)),
                        table(options.prefix, "entries")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(conversation_id, id DESC) WHERE head IS NOT NULL",
                        quote_identifier(std::format("{}entry_heads_by_conversation", options.prefix)),
                        table(options.prefix, "entries")),
                std::format("CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, "
                            "kind TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN "
                            "('pending','running','waiting','completing','terminal')), abort_requested INTEGER NOT "
                            "NULL CHECK(abort_requested IN (0,1)), background INTEGER NOT NULL CHECK(background IN "
                            "(0,1)), record TEXT NOT NULL CHECK(json_valid(record))) STRICT",
                        table(options.prefix, "tasks")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(status, id)",
                        quote_identifier(std::format("{}tasks_by_status", options.prefix)),
                        table(options.prefix, "tasks")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(conversation_id, id)",
                        quote_identifier(std::format("{}tasks_by_conversation", options.prefix)),
                        table(options.prefix, "tasks")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(kind, id)",
                        quote_identifier(std::format("{}tasks_by_kind", options.prefix)),
                        table(options.prefix, "tasks")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(abort_requested, id)",
                        quote_identifier(std::format("{}tasks_by_abort_requested", options.prefix)),
                        table(options.prefix, "tasks")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(background, id)",
                        quote_identifier(std::format("{}tasks_by_background", options.prefix)),
                        table(options.prefix, "tasks")),
                std::format("CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, conversation_id INTEGER NOT NULL, "
                            "request_id TEXT, status TEXT NOT NULL CHECK(status IN "
                            "('queued','placed','done','unanswered')), record TEXT NOT NULL CHECK(json_valid(record))) "
                            "STRICT",
                        table(options.prefix, "submissions")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(conversation_id, request_id)",
                        quote_identifier(std::format("{}submissions_by_request", options.prefix)),
                        table(options.prefix, "submissions")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(conversation_id, id)",
                        quote_identifier(std::format("{}submissions_by_conversation", options.prefix)),
                        table(options.prefix, "submissions")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(status, id)",
                        quote_identifier(std::format("{}submissions_by_status", options.prefix)),
                        table(options.prefix, "submissions")),
                std::format(
                        "CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, family INTEGER NOT "
                        "NULL CHECK(family IN (0,1)), key_value TEXT NOT NULL, scope_kind TEXT NOT NULL "
                        "CHECK(scope_kind IN ('session','conversation','task')), owner_id INTEGER NOT NULL, created_at "
                        "INTEGER NOT NULL, retired_at INTEGER, record TEXT NOT NULL CHECK(json_valid(record))) STRICT",
                        table(options.prefix, "documents")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(kind, scope_kind, owner_id, family, key_value, "
                            "created_at DESC, retired_at)",
                        quote_identifier(std::format("{}documents_by_address", options.prefix)),
                        table(options.prefix, "documents")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(scope_kind, owner_id, id)",
                        quote_identifier(std::format("{}documents_by_scope", options.prefix)),
                        table(options.prefix, "documents")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(scope_kind, owner_id, kind, id)",
                        quote_identifier(std::format("{}documents_by_scope_kind", options.prefix)),
                        table(options.prefix, "documents")),
                std::format("CREATE TABLE IF NOT EXISTS {} (document_id INTEGER NOT NULL, seq INTEGER NOT NULL, kind "
                            "TEXT NOT NULL CHECK(kind IN ('base','delta')), version INTEGER NOT NULL, content TEXT NOT "
                            "NULL CHECK(json_valid(content)), PRIMARY KEY(document_id,seq)) STRICT",
                        table(options.prefix, "document_revisions")),
                std::format("CREATE INDEX IF NOT EXISTS {} ON {}(document_id, kind, seq DESC)",
                        quote_identifier(std::format("{}document_revisions_by_kind", options.prefix)),
                        table(options.prefix, "document_revisions")),
                std::format("UPDATE {} SET version=1 WHERE singleton=1", schema),
        };
        for (const std::string& sql : definitions) {
            if (auto result = execute(database, sql); !result) return fail(std::move(result.error()));
        }
        if (auto committed = execute(database, "COMMIT"); !committed) return fail(std::move(committed.error()));
        return {};
    }

    [[nodiscard]] support::ExpectedVoid recover() {
        const auto load_rows = [this](std::string_view sql, auto callback) -> support::ExpectedVoid {
            Statement statement{database, sql};
            if (!statement.valid()) return std::unexpected(statement.error());
            while (true) {
                auto row = statement.step();
                if (!row) return std::unexpected(std::move(row.error()));
                if (!*row) return {};
                if (auto loaded = callback(statement.get()); !loaded) return loaded;
            }
        };
        const std::string conversations = table(options.prefix, "conversations");
        const std::string entries = table(options.prefix, "entries");
        const std::string documents = table(options.prefix, "documents");
        const std::string revisions = table(options.prefix, "document_revisions");
        const std::string tasks = table(options.prefix, "tasks");
        const std::string record_ids = table(options.prefix, "record_ids");
        const std::string metadata_table = table(options.prefix, "durable_metadata");
        const auto restore_record_id = [this, &record_ids](
                                               std::int64_t id, std::string_view kind) -> support::ExpectedVoid {
            Statement verify{database, std::format("SELECT record_type FROM {} WHERE id=?1", record_ids)};
            if (!verify.valid()) return std::unexpected(verify.error());
            if (auto bound = verify.bind_int(1, id); !bound) return bound;
            auto row = verify.step();
            if (!row) return std::unexpected(std::move(row.error()));
            const auto* stored_kind =
                    *row ? reinterpret_cast<const char*>(sqlite3_column_text(verify.get(), 0)) : nullptr;
            if (stored_kind == nullptr || std::string_view{stored_kind} != kind) {
                return std::unexpected(corrupt("Persisted record ID is missing or claimed by another record type"));
            }
            return {};
        };
        const auto sqlite_integer = [](const chord::Json& value) -> std::optional<std::int64_t> {
            const double* number = value.as_number();
            if (number == nullptr) return std::nullopt;
            return safe_sqlite_integer(*number);
        };
        const auto json_int = [&sqlite_integer](const chord::Json::Object* object,
                                      std::string_view key) -> std::optional<std::int64_t> {
            if (object == nullptr) return std::nullopt;
            const auto found = object->find(std::string{key});
            if (found == object->end()) return std::nullopt;
            return sqlite_integer(found->second);
        };

        auto loaded = load_rows(std::format("SELECT id,record FROM {} ORDER BY id", conversations),
                [this, &restore_record_id, &sqlite_integer](sqlite3_stmt* row) -> support::ExpectedVoid {
                    const auto* record_text = reinterpret_cast<const char*>(sqlite3_column_text(row, 1));
                    if (record_text == nullptr) return std::unexpected(corrupt("Invalid persisted conversation JSON"));
                    const auto parsed = parse_json(record_text);
                    if (!parsed) return std::unexpected(corrupt("Invalid persisted conversation JSON"));
                    const auto* object = parsed->as_object();
                    const auto id_value = object == nullptr || !object->contains("id")
                                                  ? std::nullopt
                                                  : sqlite_integer(object->at("id"));
                    if (!id_value || *id_value != sqlite3_column_int64(row, 0))
                        return std::unexpected(corrupt("Invalid persisted conversation"));
                    const std::int64_t id = *id_value;
                    const std::array write{StorageWrite{
                            .type = "conversation", .conversation = ConversationRecord{ConversationId{id}}}};
                    auto restored = state.restore(write, 1);
                    if (!restored) return std::unexpected(std::move(restored.error()));
                    return restore_record_id(id, "conversation");
                });
        if (!loaded) return loaded;

        loaded = load_rows(
                std::format("SELECT id,conversation_id,record,commit_seq FROM {} ORDER BY commit_seq,id", entries),
                [this, &restore_record_id, &sqlite_integer, &json_int](sqlite3_stmt* row) -> support::ExpectedVoid {
                    const auto* record_text = reinterpret_cast<const char*>(sqlite3_column_text(row, 2));
                    if (record_text == nullptr) return std::unexpected(corrupt("Invalid persisted entry JSON"));
                    const auto parsed = parse_json(record_text);
                    if (!parsed) return std::unexpected(corrupt("Invalid persisted entry JSON"));
                    const auto* object = parsed->as_object();
                    if (object == nullptr) return std::unexpected(corrupt("Invalid persisted entry"));
                    const auto id = object->contains("id") ? sqlite_integer(object->at("id")) : std::nullopt;
                    const auto conversation_id = object->contains("conversationId")
                                                         ? sqlite_integer(object->at("conversationId"))
                                                         : std::nullopt;
                    const auto* kind = object->contains("kind") ? object->at("kind").as_string() : nullptr;
                    const auto* data = object->contains("data") ? &object->at("data") : nullptr;
                    if (!id || !conversation_id || kind == nullptr || data == nullptr ||
                            *id != sqlite3_column_int64(row, 0) || *conversation_id != sqlite3_column_int64(row, 1) ||
                            sqlite3_column_int64(row, 3) < 1) {
                        return std::unexpected(corrupt("Invalid persisted entry"));
                    }
                    std::optional<EntryId> head;
                    if (const auto head_id = json_int(object, "head"); head_id) head = EntryId{*head_id};
                    std::vector<chord::Json> model;
                    if (const auto* raw_model = object->contains("model") ? &object->at("model") : nullptr;
                            raw_model != nullptr) {
                        const auto* messages = raw_model->as_array();
                        if (messages == nullptr) return std::unexpected(corrupt("Invalid persisted entry"));
                        model.reserve(messages->size());
                        for (const auto& message : *messages) {
                            auto decoded = decode_json(message.to_support());
                            if (!decoded) return std::unexpected(corrupt("Invalid persisted entry JSON"));
                            model.push_back(std::move(*decoded));
                        }
                    }
                    std::optional<TaskId> by_task_id;
                    if (const auto task_id = json_int(object, "byTaskId")) by_task_id = TaskId{*task_id};
                    const std::array write{StorageWrite{.type = "entry",
                            .entry = EntryRecord{EntryId{*id}, ConversationId{*conversation_id}, *kind, *data, head,
                                    std::move(model), by_task_id}}};
                    const Seq seq = static_cast<Seq>(sqlite3_column_int64(row, 3));
                    auto restored = state.restore(write, seq);
                    if (!restored) return std::unexpected(std::move(restored.error()));
                    return restore_record_id(*id, "entry");
                });
        if (!loaded) return loaded;

        loaded = load_rows(
                std::format("SELECT id,kind,created_at,retired_at,owner_id,record FROM {} ORDER BY created_at,id",
                        documents),
                [this, &revisions, &restore_record_id, &sqlite_integer, &json_int](
                        sqlite3_stmt* row) -> support::ExpectedVoid {
                    const auto* record_text = reinterpret_cast<const char*>(sqlite3_column_text(row, 5));
                    if (record_text == nullptr) return std::unexpected(corrupt("Invalid persisted document record"));
                    const auto parsed = parse_json(record_text);
                    if (!parsed) return std::unexpected(corrupt("Invalid persisted document record"));
                    const auto* object = parsed->as_object();
                    if (object == nullptr) return std::unexpected(corrupt("Invalid persisted document record"));
                    const auto id = object->contains("id") ? sqlite_integer(object->at("id")) : std::nullopt;
                    const auto* kind = object->contains("kind") ? object->at("kind").as_string() : nullptr;
                    if (!id || kind == nullptr) return std::unexpected(corrupt("Invalid persisted document record"));
                    std::optional<ConversationId> conversation_id;
                    if (const auto owner = json_int(object, "conversationId"); owner)
                        conversation_id = ConversationId{*owner};
                    const auto created_at = json_int(object, "createdAt");
                    const auto* stored_kind = reinterpret_cast<const char*>(sqlite3_column_text(row, 1));
                    if (!created_at || *created_at != sqlite3_column_int64(row, 2) ||
                            *id != sqlite3_column_int64(row, 0) || stored_kind == nullptr || *kind != stored_kind) {
                        return std::unexpected(corrupt("Persisted document columns do not match its record"));
                    }
                    const bool retired = sqlite3_column_type(row, 3) != SQLITE_NULL;
                    const std::int64_t retired_seq = retired ? sqlite3_column_int64(row, 3) : 0;
                    if (retired && retired_seq <= *created_at) {
                        return std::unexpected(corrupt("Invalid persisted document retirement sequence"));
                    }
                    if (object->contains("conversationId") && !conversation_id)
                        return std::unexpected(corrupt("Invalid persisted document owner"));
                    const std::int64_t stored_id = sqlite3_column_int64(row, 0);
                    const std::int64_t stored_owner = sqlite3_column_int64(row, 4);
                    if (*id != stored_id || (conversation_id && conversation_id->value != stored_owner) ||
                            (!conversation_id && stored_owner != 0)) {
                        return std::unexpected(corrupt("Persisted document columns do not match its record"));
                    }
                    if (auto claimed = restore_record_id(*id, "document"); !claimed) return claimed;
                    const std::array create{StorageWrite{.type = "document.create",
                            .document_record = DocumentRecord{DocumentId{*id}, *kind, *created_at},
                            .document_conversation_id = conversation_id,
                            .document_value = chord::Json{chord::Json::Object{}}}};
                    auto restored = state.restore(create, *created_at);
                    if (!restored) return std::unexpected(std::move(restored.error()));
                    Statement history{database,
                            std::format("SELECT kind,version,seq,content FROM {} WHERE document_id=?1 ORDER BY seq",
                                    revisions)};
                    if (!history.valid()) return std::unexpected(history.error());
                    if (auto bound = history.bind_int(1, *id); !bound) return bound;
                    bool has_revision = false;
                    std::int64_t current_version = 0;
                    std::int64_t previous_seq = 0;
                    chord::Json current_value{chord::Json::Object{}};
                    while (true) {
                        auto revision_row = history.step();
                        if (!revision_row) return std::unexpected(std::move(revision_row.error()));
                        if (!*revision_row) break;
                        const auto* kind_text = reinterpret_cast<const char*>(sqlite3_column_text(history.get(), 0));
                        const std::int64_t version = sqlite3_column_int64(history.get(), 1);
                        const std::int64_t revision_seq = sqlite3_column_int64(history.get(), 2);
                        const auto* content_text = reinterpret_cast<const char*>(sqlite3_column_text(history.get(), 3));
                        if (kind_text == nullptr || version < 1 || revision_seq < *created_at ||
                                (has_revision && revision_seq <= previous_seq) || content_text == nullptr ||
                                (!has_revision && std::string_view{kind_text} != "base")) {
                            return std::unexpected(corrupt("Invalid persisted document revision"));
                        }
                        const std::string_view content{content_text};
                        support::Expected<chord::Json> value =
                                std::unexpected(corrupt("Invalid persisted document value"));
                        if (!has_revision || std::string_view{kind_text} == "base") {
                            value = parse_json(content);
                            current_version = version;
                        } else {
                            if (std::string_view{kind_text} != "delta" || version != current_version) {
                                return std::unexpected(corrupt("Invalid persisted document version transition"));
                            }
                            auto ops_json_val = support::read_json(content);
                            if (!ops_json_val) return std::unexpected(corrupt("Invalid persisted document delta JSON"));
                            auto ops = decode_ops(*ops_json_val);
                            if (!ops) return std::unexpected(corrupt("Invalid persisted document delta"));
                            value = chord::apply_immutable(current_value, *ops);
                        }
                        if (!value) return std::unexpected(corrupt("Invalid persisted document value"));
                        current_value = std::move(*value);
                        const std::array change{StorageWrite{.type = "document.change",
                                .document_id = DocumentId{*id},
                                .document_value = current_value}};
                        auto changed = state.restore(change, revision_seq);
                        if (!changed) return std::unexpected(std::move(changed.error()));
                        previous_seq = revision_seq;
                        has_revision = true;
                    }
                    if (!has_revision || previous_seq < *created_at || (retired && retired_seq <= previous_seq)) {
                        return std::unexpected(corrupt("Document revision history is incomplete"));
                    }
                    if (retired) {
                        const std::array retire{
                                StorageWrite{.type = "document.retire", .document_id = DocumentId{*id}}};
                        auto retired_state = state.restore(retire, retired_seq);
                        if (!retired_state) return std::unexpected(std::move(retired_state.error()));
                    }
                    return {};
                });
        if (!loaded) return loaded;

        loaded = load_rows(
                std::format(
                        "SELECT id,conversation_id,kind,status,abort_requested,background,record FROM {} ORDER BY id",
                        tasks),
                [this, &restore_record_id](sqlite3_stmt* row) -> support::ExpectedVoid {
                    const auto* record_text = reinterpret_cast<const char*>(sqlite3_column_text(row, 6));
                    if (record_text == nullptr) return std::unexpected(corrupt("Invalid persisted task JSON"));
                    const auto parsed = parse_json(record_text);
                    if (!parsed) return std::unexpected(corrupt("Invalid persisted task JSON"));
                    auto decoded = decode_task_record(*parsed);
                    if (!decoded) return std::unexpected(std::move(decoded.error()));
                    const std::int64_t id = sqlite3_column_int64(row, 0);
                    if (decoded->id.value != id) {
                        return std::unexpected(corrupt("Persisted task ID mismatch"));
                    }
                    const std::array write{StorageWrite{.type = "task", .task = *decoded}};
                    auto restored = state.restore(write, 1);
                    if (!restored) return std::unexpected(std::move(restored.error()));
                    return restore_record_id(id, "task");
                });
        if (!loaded) return loaded;

        const std::string submissions = table(options.prefix, "submissions");
        loaded = load_rows(
                std::format("SELECT id,conversation_id,request_id,status,record FROM {} ORDER BY id", submissions),
                [this, &restore_record_id](sqlite3_stmt* row) -> support::ExpectedVoid {
                    const auto* record_text = reinterpret_cast<const char*>(sqlite3_column_text(row, 4));
                    if (record_text == nullptr) return std::unexpected(corrupt("Invalid persisted submission JSON"));
                    const auto parsed = parse_json(record_text);
                    if (!parsed) return std::unexpected(corrupt("Invalid persisted submission JSON"));
                    auto decoded = decode_submission_record(*parsed);
                    if (!decoded) return std::unexpected(std::move(decoded.error()));
                    const std::int64_t id = sqlite3_column_int64(row, 0);
                    if (decoded->id.value != id || decoded->conversation_id.value != sqlite3_column_int64(row, 1)) {
                        return std::unexpected(corrupt("Persisted submission columns do not match its record"));
                    }
                    const auto* stored_status = reinterpret_cast<const char*>(sqlite3_column_text(row, 3));
                    if (stored_status == nullptr || decoded->status != stored_status) {
                        return std::unexpected(corrupt("Persisted submission columns do not match its record"));
                    }
                    const auto* stored_request = reinterpret_cast<const char*>(sqlite3_column_text(row, 2));
                    const std::optional<std::string> request_column =
                            stored_request == nullptr ? std::optional<std::string>{}
                                                      : std::optional<std::string>{std::string{stored_request}};
                    if (decoded->request_id != request_column) {
                        return std::unexpected(corrupt("Persisted submission columns do not match its record"));
                    }
                    const std::array write{StorageWrite{.type = "submission", .submission = *decoded}};
                    auto restored = state.restore(write, 1);
                    if (!restored) return std::unexpected(std::move(restored.error()));
                    return restore_record_id(id, "submission");
                });
        if (!loaded) return loaded;

        Statement metadata{database, std::format("SELECT next_id,next_seq FROM {} WHERE singleton=1", metadata_table)};
        if (!metadata.valid()) return std::unexpected(metadata.error());
        auto row = metadata.step();
        if (!row) return std::unexpected(std::move(row.error()));
        if (!*row) return std::unexpected(corrupt("SQLite metadata row is missing"));
        const auto* next_id_text = reinterpret_cast<const char*>(sqlite3_column_text(metadata.get(), 0));
        const std::int64_t metadata_next_id =
                next_id_text != nullptr ? std::stoll(next_id_text) : sqlite3_column_int64(metadata.get(), 0);
        const Seq metadata_next_seq = sqlite3_column_int64(metadata.get(), 1);
        Statement high_water{database,
                std::format("SELECT coalesce((SELECT max(id)+1 FROM {}),2), "
                            "max((SELECT coalesce(max(commit_seq)+1,1) FROM {}),"
                            "(SELECT coalesce(max(seq)+1,1) FROM {}),"
                            "(SELECT coalesce(max(created_at)+1,1) FROM {}),"
                            "(SELECT coalesce(max(retired_at)+1,1) FROM {}))",
                        record_ids,
                        entries,
                        revisions,
                        documents,
                        documents)};
        if (!high_water.valid()) return std::unexpected(high_water.error());
        auto high_row = high_water.step();
        if (!high_row) return std::unexpected(std::move(high_row.error()));
        if (!*high_row) return std::unexpected(corrupt("SQLite high-water marks are missing"));
        Statement unmatched{database,
                std::format(
                        "SELECT (SELECT count(*) FROM {} c LEFT JOIN {} r ON r.id=c.id AND "
                        "r.record_type='conversation' WHERE r.id IS NULL)+"
                        "(SELECT count(*) FROM {} e LEFT JOIN {} r ON r.id=e.id AND r.record_type='entry' WHERE r.id "
                        "IS NULL)+"
                        "(SELECT count(*) FROM {} d LEFT JOIN {} r ON r.id=d.id AND r.record_type='document' WHERE "
                        "r.id IS NULL)+"
                        "(SELECT count(*) FROM {} s LEFT JOIN {} r ON r.id=s.id AND r.record_type='submission' WHERE "
                        "r.id IS NULL)+"
                        "(SELECT count(*) FROM {} v LEFT JOIN {} d ON d.id=v.document_id WHERE d.id IS NULL)+"
                        "(SELECT count(*) FROM {} r LEFT JOIN {} c ON c.id=r.id LEFT JOIN {} e ON e.id=r.id LEFT JOIN "
                        "{} d ON d.id=r.id LEFT JOIN {} s ON s.id=r.id WHERE "
                        "(r.record_type='conversation' AND (c.id IS NULL OR e.id IS NOT NULL OR d.id IS NOT NULL OR "
                        "s.id IS NOT NULL)) OR "
                        "(r.record_type='entry' AND (e.id IS NULL OR c.id IS NOT NULL OR d.id IS NOT NULL OR s.id IS "
                        "NOT NULL)) OR "
                        "(r.record_type='document' AND (d.id IS NULL OR c.id IS NOT NULL OR e.id IS NOT NULL OR s.id "
                        "IS NOT NULL)) OR "
                        "(r.record_type='submission' AND (s.id IS NULL OR c.id IS NOT NULL OR e.id IS NOT NULL OR "
                        "d.id IS NOT NULL)))+"
                        "(SELECT count(*) FROM {} r LEFT JOIN {} d ON d.id=r.document_id WHERE d.id IS NULL)+"
                        "(SELECT count(*) FROM {} d WHERE NOT EXISTS (SELECT 1 FROM {} r WHERE r.document_id=d.id AND "
                        "r.kind='base'))",
                        conversations,
                        record_ids,
                        entries,
                        record_ids,
                        documents,
                        record_ids,
                        submissions,
                        record_ids,
                        revisions,
                        documents,
                        record_ids,
                        conversations,
                        entries,
                        documents,
                        submissions,
                        revisions,
                        documents,
                        documents,
                        revisions)};
        if (!unmatched.valid()) return std::unexpected(unmatched.error());
        auto unmatched_row = unmatched.step();
        if (!unmatched_row) return std::unexpected(std::move(unmatched_row.error()));
        if (!*unmatched_row || sqlite3_column_int64(unmatched.get(), 0) != 0) {
            return std::unexpected(corrupt("SQLite records, ID claims, and document revisions do not match"));
        }
        const std::int64_t observed_next_id = sqlite3_column_int64(high_water.get(), 0);
        const Seq observed_next_seq = static_cast<Seq>(sqlite3_column_int64(high_water.get(), 1));
        const std::int64_t next_id = std::max(metadata_next_id, observed_next_id);
        const Seq next_seq = std::max(metadata_next_seq, observed_next_seq);
        if (next_id < 2 || next_seq < 1 || metadata_next_id < 2 || metadata_next_seq < 1 || observed_next_id < 2 ||
                observed_next_seq < 1) {
            return std::unexpected(corrupt("Invalid SQLite allocator metadata"));
        }
        if (auto begin = execute(database, "BEGIN IMMEDIATE"); !begin) return begin;
        const auto fail = [this](support::Error error) -> support::ExpectedVoid {
            (void)execute(database, "ROLLBACK");
            return std::unexpected(std::move(error));
        };
        Statement repair{database,
                std::format("UPDATE {} SET next_id=cast(max(cast(next_id as integer),?1) as "
                            "text),next_seq=max(next_seq,?2) WHERE singleton=1",
                        metadata_table)};
        if (!repair.valid()) return fail(repair.error());
        if (auto bound = repair.bind_int(1, next_id); !bound) return fail(std::move(bound.error()));
        if (auto bound = repair.bind_int(2, next_seq); !bound) return fail(std::move(bound.error()));
        auto repaired = repair.step();
        if (!repaired) return fail(std::move(repaired.error()));
        if (sqlite3_changes(database) != 1) return fail(corrupt("SQLite allocator metadata repair affected no row"));
        if (auto committed = execute(database, "COMMIT"); !committed) return fail(std::move(committed.error()));
        return state.restore_high_water_marks(next_id, next_seq);
    }

    [[nodiscard]] support::Expected<Seq> commit(std::span<const StorageWrite> writes) {
        if (auto error = unusable(); error) return std::unexpected(std::move(*error));
        if (auto begin = execute(database, "BEGIN IMMEDIATE"); !begin) return std::unexpected(std::move(begin.error()));
        const auto fail = [this](support::Error error) -> support::Expected<Seq> {
            (void)execute(database, "ROLLBACK");
            return std::unexpected(std::move(error));
        };
        Statement metadata_state{database,
                std::format("SELECT next_id,next_seq FROM {} WHERE singleton=1",
                        table(options.prefix, "durable_metadata"))};
        if (!metadata_state.valid()) return fail(metadata_state.error());
        auto metadata_row = metadata_state.step();
        if (!metadata_row) return fail(std::move(metadata_row.error()));
        if (!*metadata_row) return fail(corrupt("SQLite metadata row is missing"));
        const auto* next_id_text = reinterpret_cast<const char*>(sqlite3_column_text(metadata_state.get(), 0));
        std::int64_t next_id =
                next_id_text != nullptr ? std::stoll(next_id_text) : sqlite3_column_int64(metadata_state.get(), 0);
        const Seq seq = sqlite3_column_int64(metadata_state.get(), 1);
        if (seq < 1 || next_id < 2) return fail(corrupt("Invalid SQLite allocator metadata"));
        if (auto valid = state.validate(writes, seq); !valid) return fail(std::move(valid.error()));
        const auto insert = [this](std::string_view sql, const auto& bind) -> support::ExpectedVoid {
            Statement statement{database, sql};
            if (!statement.valid()) return std::unexpected(statement.error());
            if (auto result = bind(statement); !result) return result;
            auto stepped = statement.step();
            return stepped ? support::ExpectedVoid{} : std::unexpected(std::move(stepped.error()));
        };
        for (const StorageWrite& write : writes) {
            if (write.type == "conversation") {
                auto claim = insert(std::format("INSERT INTO {}(id,record_type) VALUES(?1,'conversation')",
                                            table(options.prefix, "record_ids")),
                        [&](Statement& statement) { return statement.bind_int(1, write.conversation->id.value); });
                if (!claim) return fail(std::move(claim.error()));
                next_id = std::max(next_id, write.conversation->id.value + 1);
                const chord::Json value{
                        chord::Json::Object{{"id", chord::Json{static_cast<double>(write.conversation->id.value)}}}};
                const std::string json = as_string(value);
                auto result = insert(
                        std::format(
                                "INSERT INTO {}(id,owner_conversation_id,owner_task_id,record) VALUES(?1,NULL,NULL,?2)",
                                table(options.prefix, "conversations")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, write.conversation->id.value); !bound) return bound;
                            return statement.bind_text(2, json);
                        });
                if (!result) return fail(std::move(result.error()));
            } else if (write.type == "entry") {
                auto claim = insert(std::format("INSERT INTO {}(id,record_type) VALUES(?1,'entry')",
                                            table(options.prefix, "record_ids")),
                        [&](Statement& statement) { return statement.bind_int(1, write.entry->id.value); });
                if (!claim) return fail(std::move(claim.error()));
                next_id = std::max(next_id, write.entry->id.value + 1);
                chord::Json::Object value{{"id", chord::Json{static_cast<double>(write.entry->id.value)}},
                        {"conversationId", chord::Json{static_cast<double>(write.entry->conversation_id.value)}},
                        {"kind", chord::Json{write.entry->kind}},
                        {"data", write.entry->data}};
                if (write.entry->head)
                    value.emplace("head", chord::Json{static_cast<double>(write.entry->head->value)});
                if (!write.entry->model.empty()) {
                    chord::Json::Array model;
                    for (const auto& message : write.entry->model) model.push_back(message);
                    value.emplace("model", chord::Json{std::move(model)});
                }
                if (write.entry->by_task_id)
                    value.emplace("byTaskId", chord::Json{static_cast<double>(write.entry->by_task_id->value)});
                const std::string json = as_string(chord::Json{std::move(value)});
                if (json.empty()) return fail(corrupt("SQLite entry record serialization failed"));
                auto result = insert(
                        std::format("INSERT INTO {}(id,conversation_id,head,commit_seq,record) VALUES(?1,?2,?3,?4,?5)",
                                table(options.prefix, "entries")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, write.entry->id.value); !bound) return bound;
                            if (auto bound = statement.bind_int(2, write.entry->conversation_id.value); !bound)
                                return bound;
                            if (write.entry->head) {
                                if (auto bound = statement.bind_int(3, write.entry->head->value); !bound) return bound;
                            } else {
                                if (auto bound = statement.bind_null(3); !bound) return bound;
                            }
                            if (auto bound = statement.bind_int(4, seq); !bound) return bound;
                            return statement.bind_text(5, json);
                        });
                if (!result) return fail(std::move(result.error()));
            } else if (write.type == "document.create") {
                auto claim = insert(std::format("INSERT INTO {}(id,record_type) VALUES(?1,'document')",
                                            table(options.prefix, "record_ids")),
                        [&](Statement& statement) { return statement.bind_int(1, write.document_record->id.value); });
                if (!claim) return fail(std::move(claim.error()));
                next_id = std::max(next_id, write.document_record->id.value + 1);
                const std::int64_t owner = write.document_conversation_id ? write.document_conversation_id->value : 0;
                chord::Json::Object record{{"id", chord::Json{static_cast<double>(write.document_record->id.value)}},
                        {"kind", chord::Json{write.document_record->kind}},
                        {"createdAt", chord::Json{static_cast<double>(seq)}}};
                if (write.document_conversation_id)
                    record.emplace("conversationId", chord::Json{static_cast<double>(owner)});
                const std::string record_json = as_string(chord::Json{record});
                const std::string content = as_string(*write.document_value);
                auto result = insert(std::format("INSERT INTO "
                                                 "{}(id,kind,family,key_value,scope_kind,owner_id,created_at,retired_"
                                                 "at,record) VALUES(?1,?2,0,'',?3,?4,?5,NULL,?6)",
                                             table(options.prefix, "documents")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, write.document_record->id.value); !bound)
                                return bound;
                            if (auto bound = statement.bind_text(2, write.document_record->kind); !bound) return bound;
                            if (auto bound = statement.bind_text(
                                        3, write.document_conversation_id ? "conversation" : "session");
                                    !bound)
                                return bound;
                            if (auto bound = statement.bind_int(4, owner); !bound) return bound;
                            if (auto bound = statement.bind_int(5, seq); !bound) return bound;
                            return statement.bind_text(6, record_json);
                        });
                if (!result) return fail(std::move(result.error()));
                result = insert(
                        std::format("INSERT INTO {}(document_id,seq,kind,version,content) VALUES(?1,?2,'base',1,?3)",
                                table(options.prefix, "document_revisions")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, write.document_record->id.value); !bound)
                                return bound;
                            if (auto bound = statement.bind_int(2, seq); !bound) return bound;
                            return statement.bind_text(3, content);
                        });
                if (!result) return fail(std::move(result.error()));
            } else if (write.type == "document.change") {
                const bool base = write.document_ops.empty();
                const std::string content = base ? as_string(*write.document_value) : ops_json(write.document_ops);
                auto result =
                        insert(std::format("INSERT INTO {}(document_id,seq,kind,version,content) VALUES(?1,?2,?3,1,?4)",
                                       table(options.prefix, "document_revisions")),
                                [&](Statement& statement) -> support::ExpectedVoid {
                                    if (auto bound = statement.bind_int(1, write.document_id->value); !bound)
                                        return bound;
                                    if (auto bound = statement.bind_int(2, seq); !bound) return bound;
                                    if (auto bound = statement.bind_text(3, base ? "base" : "delta"); !bound)
                                        return bound;
                                    return statement.bind_text(4, content);
                                });
                if (!result) return fail(std::move(result.error()));
            } else if (write.type == "document.retire") {
                Statement update{database,
                        std::format("UPDATE {} SET retired_at=?2,record=json_set(record,'$.retiredAt',?2) WHERE id=?1 "
                                    "AND retired_at IS NULL",
                                table(options.prefix, "documents"))};
                if (!update.valid()) return fail(update.error());
                if (auto bound = update.bind_int(1, write.document_id->value); !bound)
                    return fail(std::move(bound.error()));
                if (auto bound = update.bind_int(2, seq); !bound) return fail(std::move(bound.error()));
                auto stepped = update.step();
                if (!stepped) return fail(std::move(stepped.error()));
                if (sqlite3_changes(database) != 1)
                    return fail(corrupt("Document retirement did not update one live record"));
            } else if (write.type == "task") {
                if (!write.task.has_value()) {
                    return fail(corrupt("Task write has no record"));
                }
                const TaskRecord& task = *write.task;
                const std::string record_str = as_string(encode_task_record(task));
                auto result = insert(
                        std::format("INSERT INTO {}(id,conversation_id,kind,status,abort_requested,background,record) "
                                    "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(id) DO UPDATE SET "
                                    "conversation_id=excluded.conversation_id,kind=excluded.kind,"
                                    "status=excluded.status,abort_requested=excluded.abort_requested,"
                                    "background=excluded.background,record=excluded.record",
                                table(options.prefix, "tasks")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, task.id.value); !bound) return bound;
                            if (auto bound = statement.bind_int(2, task.conversation_id.value); !bound) return bound;
                            if (auto bound = statement.bind_text(3, task.kind); !bound) return bound;
                            if (auto bound = statement.bind_text(4, task.state.status); !bound) return bound;
                            if (auto bound = statement.bind_int(5, task.abort_requested ? 1 : 0); !bound) return bound;
                            if (auto bound = statement.bind_int(6, task.background ? 1 : 0); !bound) return bound;
                            return statement.bind_text(7, record_str);
                        });
                if (!result) return fail(std::move(result.error()));
                auto id_result = insert(std::format("INSERT OR IGNORE INTO {}(id,record_type) VALUES(?1,'task')",
                                                table(options.prefix, "record_ids")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            return statement.bind_int(1, task.id.value);
                        });
                if (!id_result) return fail(std::move(id_result.error()));
            } else if (write.type == "submission") {
                if (!write.submission.has_value()) {
                    return fail(corrupt("Submission write has no record"));
                }
                const SubmissionRecord& submission = *write.submission;
                auto claim = insert(std::format("INSERT OR IGNORE INTO {}(id,record_type) VALUES(?1,'submission')",
                                            table(options.prefix, "record_ids")),
                        [&](Statement& statement) { return statement.bind_int(1, submission.id.value); });
                if (!claim) return fail(std::move(claim.error()));
                next_id = std::max(next_id, submission.id.value + 1);
                // Every submission write replaces the record, mirroring the
                // frozen `SqliteStorage` submissions upsert.
                const std::string record_str = as_string(encode_submission_record(submission));
                auto result = insert(
                        std::format(
                                "INSERT INTO {}(id,conversation_id,request_id,status,record) VALUES(?1,?2,?3,?4,?5) "
                                "ON CONFLICT(id) DO UPDATE SET conversation_id=excluded.conversation_id,"
                                "request_id=excluded.request_id,status=excluded.status,record=excluded.record",
                                table(options.prefix, "submissions")),
                        [&](Statement& statement) -> support::ExpectedVoid {
                            if (auto bound = statement.bind_int(1, submission.id.value); !bound) return bound;
                            if (auto bound = statement.bind_int(2, submission.conversation_id.value); !bound)
                                return bound;
                            if (submission.request_id.has_value()) {
                                if (auto bound = statement.bind_text(3, *submission.request_id); !bound) return bound;
                            } else {
                                if (auto bound = statement.bind_null(3); !bound) return bound;
                            }
                            if (auto bound = statement.bind_text(4, submission.status); !bound) return bound;
                            return statement.bind_text(5, record_str);
                        });
                if (!result) return fail(std::move(result.error()));
            }
        }
        auto metadata = insert(std::format("UPDATE {} SET next_id=?1,next_seq=?2 WHERE singleton=1",
                                       table(options.prefix, "durable_metadata")),
                [&](Statement& statement) -> support::ExpectedVoid {
                    if (auto bound = statement.bind_text(1, std::to_string(next_id)); !bound) return bound;
                    return statement.bind_int(2, seq + 1);
                });
        if (!metadata) return fail(std::move(metadata.error()));
        if (auto committed = execute(database, "COMMIT"); !committed) {
            poisoned = true;
            return std::unexpected(std::move(committed.error()));
        }
        auto applied = state.apply(writes, seq);
        if (!applied) {
            poisoned = true;
            return std::unexpected(std::move(applied.error()));
        }
        return seq;
    }
};

SqliteStorage::SqliteStorage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SqliteStorage::SqliteStorage(SqliteStorage&& other) noexcept : Storage(), impl_(std::move(other.impl_)) {}
SqliteStorage& SqliteStorage::operator=(SqliteStorage&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}
SqliteStorage::~SqliteStorage() = default;

support::Expected<std::unique_ptr<SqliteStorage>> SqliteStorage::open(
        const std::filesystem::path& path, SqliteStorageOptions options) {
    if (!valid_prefix(options.prefix))
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "SQLite prefix must be an identifier"));
    sqlite3* database = nullptr;
    const std::string filename = path.string();
    const int opened = sqlite3_open_v2(
            filename.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (opened != SQLITE_OK) {
        auto error = database_error(database, "SQLite open");
        if (database != nullptr) sqlite3_close_v2(database);
        return std::unexpected(std::move(error));
    }
    auto impl = std::make_unique<Impl>(database, std::move(options));
    if (auto configured = impl->configure(); !configured) return std::unexpected(std::move(configured.error()));
    if (auto schema = impl->create_schema(); !schema) return std::unexpected(std::move(schema.error()));
    if (auto recovered = impl->recover(); !recovered) return std::unexpected(std::move(recovered.error()));
    return std::unique_ptr<SqliteStorage>{new SqliteStorage{std::move(impl)}};
}

support::AsyncResult<Seq> SqliteStorage::commit(std::span<const StorageWrite> writes) {
    return support::AsyncResult<Seq>{impl_->commit(writes)};
}
support::Expected<std::int64_t> SqliteStorage::mint_id() {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    if (auto begin = execute(impl_->database, "BEGIN IMMEDIATE"); !begin)
        return std::unexpected(std::move(begin.error()));
    Statement statement{impl_->database,
            std::format("UPDATE {} SET next_id=cast(cast(next_id as integer)+1 as text) WHERE singleton=1 RETURNING "
                        "cast(next_id as integer)-1",
                    table(impl_->options.prefix, "durable_metadata"))};
    if (!statement.valid()) {
        (void)execute(impl_->database, "ROLLBACK");
        return std::unexpected(statement.error());
    }
    auto row = statement.step();
    if (!row || !*row) {
        (void)execute(impl_->database, "ROLLBACK");
        return std::unexpected(row ? corrupt("SQLite metadata row is missing") : std::move(row.error()));
    }
    const std::int64_t id = sqlite3_column_int64(statement.get(), 0);
    auto finished = statement.step();
    if (!finished || *finished) {
        (void)execute(impl_->database, "ROLLBACK");
        return std::unexpected(finished ? corrupt("SQLite minted more than one ID") : std::move(finished.error()));
    }
    if (auto committed = execute(impl_->database, "COMMIT"); !committed) {
        impl_->poisoned = true;
        return std::unexpected(std::move(committed.error()));
    }
    auto advanced = impl_->state.restore_high_water_marks(id + 1, impl_->state.next_seq());
    if (!advanced) {
        impl_->poisoned = true;
        return std::unexpected(std::move(advanced.error()));
    }
    return id;
}
support::Expected<std::optional<ConversationRecord>> SqliteStorage::conversation(ConversationId id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.conversation(id);
}
support::Expected<std::optional<StoredEntry>> SqliteStorage::entry(EntryId id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.entry(id);
}
support::Expected<std::vector<ConversationRecord>> SqliteStorage::scan_conversations(std::size_t limit) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.scan_conversations(limit);
}
support::Expected<std::vector<EntryRecord>> SqliteStorage::scan_entries(ConversationId id, std::size_t limit) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.scan_entries(id, limit);
}
support::Expected<std::optional<DocumentRecord>> SqliteStorage::find_document(
        std::string_view kind, std::optional<ConversationId> id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.find_document(kind, id);
}
support::Expected<std::optional<chord::Json>> SqliteStorage::document(DocumentId id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.document(id);
}
support::Expected<std::optional<TaskRecord>> SqliteStorage::task(TaskId id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.task(id);
}
support::Expected<std::vector<TaskRecord>> SqliteStorage::scan_tasks(std::size_t limit) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.scan_tasks(limit);
}
support::Expected<std::optional<SubmissionRecord>> SqliteStorage::submission(SubmissionId id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.submission(id);
}
support::Expected<std::optional<SubmissionRecord>> SqliteStorage::submission_by_request(
        ConversationId conversation_id, std::string_view request_id) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.submission_by_request(conversation_id, request_id);
}
support::Expected<std::vector<SubmissionRecord>> SqliteStorage::scan_submissions(std::size_t limit) {
    if (auto error = impl_->unusable(); error) return std::unexpected(std::move(*error));
    return impl_->state.scan_submissions(limit);
}
support::ExpectedVoid SqliteStorage::close() {
    if (impl_->database != nullptr) {
        const int result = sqlite3_close_v2(impl_->database);
        impl_->database = nullptr;
        if (result != SQLITE_OK)
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "SQLite close failed"));
    }
    impl_->state.close();
    return {};
}

} // namespace cch::durable
