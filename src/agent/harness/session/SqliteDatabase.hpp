#pragma once

#include <cch/support/Error.hpp>

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::agent::session {

class SqliteStatement;

/// Private strict-no-exceptions SQLite database connection wrapper.
/// Manages sqlite3* handle, WAL mode, busy timeout, and transactions.
class SqliteDatabase final {
public:
    SqliteDatabase() = default;
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    SqliteDatabase(SqliteDatabase&& other) noexcept;
    SqliteDatabase& operator=(SqliteDatabase&& other) noexcept;

    /// Open or create a database file with WAL mode and busy timeout.
    [[nodiscard]] static support::Expected<SqliteDatabase> open(
            const std::filesystem::path& path, int busy_timeout_ms = 5000);

    /// Open an in-memory database (useful for hermetic tests).
    [[nodiscard]] static support::Expected<SqliteDatabase> open_memory();

    /// Execute raw DDL/DML statements (e.g. schema migrations).
    [[nodiscard]] support::ExpectedVoid execute(std::string_view sql);

    /// Prepare a parameterized SQL statement.
    [[nodiscard]] support::Expected<SqliteStatement> prepare(std::string_view sql);

    /// Begin an immediate transaction (`BEGIN IMMEDIATE`).
    [[nodiscard]] support::ExpectedVoid begin_transaction();

    /// Commit the active transaction (`COMMIT`).
    [[nodiscard]] support::ExpectedVoid commit();

    /// Rollback the active transaction (`ROLLBACK`).
    [[nodiscard]] support::ExpectedVoid rollback();

    /// Whether a transaction is currently in progress.
    [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }

    /// Last inserted rowid.
    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;

    /// Direct handle access (internal).
    [[nodiscard]] sqlite3* handle() const noexcept { return db_; }

private:
    explicit SqliteDatabase(sqlite3* db) noexcept : db_(db) {}

    sqlite3* db_{nullptr};
    bool in_transaction_{false};
};

/// RAII Transaction Guard that rolls back on destruction unless committed.
class SqliteTransactionGuard final {
public:
    explicit SqliteTransactionGuard(SqliteDatabase& db) : db_(db) {}
    ~SqliteTransactionGuard() {
        if (!committed_ && db_.in_transaction()) {
            (void)db_.rollback();
        }
    }

    SqliteTransactionGuard(const SqliteTransactionGuard&) = delete;
    SqliteTransactionGuard& operator=(const SqliteTransactionGuard&) = delete;

    [[nodiscard]] support::ExpectedVoid commit() {
        auto res = db_.commit();
        if (res) committed_ = true;
        return res;
    }

private:
    SqliteDatabase& db_;
    bool committed_{false};
};

/// Private strict-no-exceptions SQLite statement wrapper.
class SqliteStatement final {
public:
    SqliteStatement() = default;
    ~SqliteStatement();

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    [[nodiscard]] support::ExpectedVoid bind_text(int index, std::string_view value);
    [[nodiscard]] support::ExpectedVoid bind_int64(int index, std::int64_t value);
    [[nodiscard]] support::ExpectedVoid bind_null(int index);

    /// Step execution. Returns true if a row is available (SQLITE_ROW), false if done (SQLITE_DONE).
    [[nodiscard]] support::Expected<bool> step();

    /// Reset statement for re-execution.
    [[nodiscard]] support::ExpectedVoid reset();

    /// Read columns (0-indexed).
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

private:
    friend class SqliteDatabase;
    explicit SqliteStatement(sqlite3_stmt* stmt) noexcept : stmt_(stmt) {}

    sqlite3_stmt* stmt_{nullptr};
};

} // namespace cch::agent::session
