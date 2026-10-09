#include "agent/harness/session/SqliteDatabase.hpp"

#include <format>
#include <utility>

namespace cch::agent::session {

namespace {

[[nodiscard]] support::Error db_error(sqlite3* db, std::string_view action) {
    const char* msg = db ? sqlite3_errmsg(db) : "Unknown SQLite error";
    return support::make_error(support::ErrorCode::Process, std::format("SQLite {}: {}", action, msg));
}

[[nodiscard]] support::Error stmt_error(sqlite3_stmt* stmt, std::string_view action) {
    sqlite3* db = stmt ? sqlite3_db_handle(stmt) : nullptr;
    return db_error(db, action);
}

} // namespace

SqliteDatabase::~SqliteDatabase() {
    if (db_) {
        if (in_transaction_) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        sqlite3_close(db_);
    }
}

SqliteDatabase::SqliteDatabase(SqliteDatabase&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), in_transaction_(std::exchange(other.in_transaction_, false)) {}

SqliteDatabase& SqliteDatabase::operator=(SqliteDatabase&& other) noexcept {
    if (this != &other) {
        if (db_) {
            if (in_transaction_) (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db_);
        }
        db_ = std::exchange(other.db_, nullptr);
        in_transaction_ = std::exchange(other.in_transaction_, false);
    }
    return *this;
}

support::Expected<SqliteDatabase> SqliteDatabase::open(const std::filesystem::path& path, int busy_timeout_ms) {
    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
        auto err = db_error(db, "open");
        if (db) sqlite3_close(db);
        return std::unexpected(err);
    }

    SqliteDatabase database(db);
    sqlite3_busy_timeout(db, busy_timeout_ms);

    if (auto res = database.execute("PRAGMA journal_mode = WAL;"); !res) return std::unexpected(res.error());
    if (auto res = database.execute("PRAGMA synchronous = NORMAL;"); !res) return std::unexpected(res.error());
    if (auto res = database.execute("PRAGMA foreign_keys = ON;"); !res) return std::unexpected(res.error());

    return database;
}

support::Expected<SqliteDatabase> SqliteDatabase::open_memory() {
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        auto err = db_error(db, "open :memory:");
        if (db) sqlite3_close(db);
        return std::unexpected(err);
    }
    SqliteDatabase database(db);
    if (auto res = database.execute("PRAGMA foreign_keys = ON;"); !res) return std::unexpected(res.error());
    return database;
}

support::ExpectedVoid SqliteDatabase::execute(std::string_view sql) {
    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql.data(), nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::string err = err_msg ? err_msg : "exec failed";
        sqlite3_free(err_msg);
        return std::unexpected(support::make_error(support::ErrorCode::Process, err));
    }
    return {};
}

support::Expected<SqliteStatement> SqliteDatabase::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK) {
        return std::unexpected(db_error(db_, "prepare"));
    }
    return SqliteStatement(stmt);
}

support::ExpectedVoid SqliteDatabase::begin_transaction() {
    if (in_transaction_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Transaction already in progress"));
    }
    if (auto res = execute("BEGIN IMMEDIATE;"); !res) return res;
    in_transaction_ = true;
    return {};
}

support::ExpectedVoid SqliteDatabase::commit() {
    if (!in_transaction_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "No transaction in progress"));
    }
    if (auto res = execute("COMMIT;"); !res) return res;
    in_transaction_ = false;
    return {};
}

support::ExpectedVoid SqliteDatabase::rollback() {
    if (!in_transaction_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "No transaction in progress"));
    }
    if (auto res = execute("ROLLBACK;"); !res) return res;
    in_transaction_ = false;
    return {};
}

std::int64_t SqliteDatabase::last_insert_rowid() const noexcept { return sqlite3_last_insert_rowid(db_); }

// ---------------- SqliteStatement ----------------

SqliteStatement::~SqliteStatement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

support::ExpectedVoid SqliteStatement::bind_text(int index, std::string_view value) {
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return std::unexpected(stmt_error(stmt_, "bind_text"));
    }
    return {};
}

support::ExpectedVoid SqliteStatement::bind_int64(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
        return std::unexpected(stmt_error(stmt_, "bind_int64"));
    }
    return {};
}

support::ExpectedVoid SqliteStatement::bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
        return std::unexpected(stmt_error(stmt_, "bind_null"));
    }
    return {};
}

support::Expected<bool> SqliteStatement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return std::unexpected(stmt_error(stmt_, "step"));
}

support::ExpectedVoid SqliteStatement::reset() {
    if (sqlite3_reset(stmt_) != SQLITE_OK) {
        return std::unexpected(stmt_error(stmt_, "reset"));
    }
    sqlite3_clear_bindings(stmt_);
    return {};
}

std::string SqliteStatement::column_text(int index) const {
    const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    const int bytes = sqlite3_column_bytes(stmt_, index);
    return txt ? std::string(txt, bytes) : std::string{};
}

std::int64_t SqliteStatement::column_int64(int index) const { return sqlite3_column_int64(stmt_, index); }

bool SqliteStatement::column_is_null(int index) const { return sqlite3_column_type(stmt_, index) == SQLITE_NULL; }

} // namespace cch::agent::session
