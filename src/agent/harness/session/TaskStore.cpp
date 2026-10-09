#include "agent/harness/session/TaskStore.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace cch::agent::session {

namespace {

constexpr std::string_view kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('pending', 'running', 'completing', 'completed', 'failed', 'aborted')),
    checkpoint TEXT NOT NULL,
    owner_session TEXT NOT NULL,
    definition_version INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    abort_requested INTEGER NOT NULL DEFAULT 0 CHECK (abort_requested IN (0, 1)),
    parent_task_id TEXT REFERENCES tasks(id)
);
CREATE TABLE IF NOT EXISTS submissions (
    id TEXT PRIMARY KEY,
    task_id TEXT NOT NULL REFERENCES tasks(id),
    payload TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT '',
    state TEXT NOT NULL DEFAULT 'queued' CHECK (state IN ('queued', 'placed', 'done', 'unanswered')),
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_tasks_state ON tasks(state);
)SQL";

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

[[nodiscard]] support::ExpectedVoid bind_task(SqliteStatement& stmt, const DurableTask& task) {
    if (auto result = stmt.bind_text(1, task.id); !result) return result;
    if (auto result = stmt.bind_text(2, task.kind); !result) return result;
    if (auto result = stmt.bind_text(3, task.state); !result) return result;
    if (auto result = stmt.bind_text(4, task.checkpoint); !result) return result;
    if (auto result = stmt.bind_text(5, task.owner_session); !result) return result;
    if (auto result = stmt.bind_int64(6, task.definition_version); !result) return result;
    if (auto result = stmt.bind_int64(7, task.created_at); !result) return result;
    if (auto result = stmt.bind_int64(8, task.updated_at); !result) return result;
    return stmt.bind_int64(9, task.abort_requested ? 1 : 0);
}

[[nodiscard]] DurableTask read_task(SqliteStatement& stmt) {
    return {.id = stmt.column_text(0),
            .kind = stmt.column_text(1),
            .state = stmt.column_text(2),
            .checkpoint = stmt.column_text(3),
            .owner_session = stmt.column_text(4),
            .parent_task_id = stmt.column_text(9),
            .definition_version = stmt.column_int64(5),
            .created_at = stmt.column_int64(6),
            .updated_at = stmt.column_int64(7),
            .abort_requested = stmt.column_int64(8) != 0};
}

constexpr std::string_view kTaskColumns =
        "id, kind, state, checkpoint, owner_session, definition_version, created_at, updated_at, abort_requested, "
        "parent_task_id";

} // namespace

support::Expected<TaskStore> TaskStore::open(const std::filesystem::path& path) {
    auto db = SqliteDatabase::open(path);
    if (!db) return std::unexpected(db.error());
    TaskStore store(std::move(*db));
    if (auto result = store.init_schema(); !result) return std::unexpected(result.error());
    return store;
}

support::Expected<TaskStore> TaskStore::open_memory() {
    auto db = SqliteDatabase::open_memory();
    if (!db) return std::unexpected(db.error());
    TaskStore store(std::move(*db));
    if (auto result = store.init_schema(); !result) return std::unexpected(result.error());
    return store;
}

support::ExpectedVoid TaskStore::init_schema() {
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return result;
    if (auto result = db_.execute(kSchemaSql); !result) return result;
    auto columns = db_.prepare("PRAGMA table_info(tasks);");
    if (!columns) return std::unexpected(columns.error());
    bool has_parent_task_id = false;
    while (true) {
        auto row = columns->step();
        if (!row) return std::unexpected(row.error());
        if (!*row) break;
        has_parent_task_id = has_parent_task_id || columns->column_text(1) == "parent_task_id";
    }
    if (!has_parent_task_id) {
        if (auto result = db_.execute("ALTER TABLE tasks ADD COLUMN parent_task_id TEXT REFERENCES tasks(id);");
                !result)
            return result;
    }
    return transaction.commit();
}

support::ExpectedVoid TaskStore::create_task(const DurableTask& task) {
    const auto now = now_ms();
    auto stmt = db_.prepare("INSERT INTO tasks (id, kind, state, checkpoint, owner_session, definition_version, "
                            "created_at, updated_at, abort_requested, parent_task_id) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
    if (!stmt) return std::unexpected(stmt.error());
    auto normalized = task;
    normalized.created_at = task.created_at == 0 ? now : task.created_at;
    normalized.updated_at = task.updated_at == 0 ? now : task.updated_at;
    if (auto result = bind_task(*stmt, normalized); !result) return result;
    if (normalized.parent_task_id.empty()) {
        if (auto result = stmt->bind_null(10); !result) return result;
    } else if (auto result = stmt->bind_text(10, normalized.parent_task_id); !result) {
        return result;
    }
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    return {};
}

support::ExpectedVoid TaskStore::add_submission(const TaskSubmission& submission) {
    auto stmt = db_.prepare("INSERT INTO submissions (id, task_id, payload, kind, state, created_at) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, submission.id); !result) return result;
    if (auto result = stmt->bind_text(2, submission.task_id); !result) return result;
    if (auto result = stmt->bind_text(3, submission.payload); !result) return result;
    if (auto result = stmt->bind_text(4, submission.kind); !result) return result;
    if (auto result = stmt->bind_text(5, submission.state.empty() ? "queued" : submission.state); !result)
        return result;
    if (auto result = stmt->bind_int64(6, submission.created_at == 0 ? now_ms() : submission.created_at); !result) {
        return result;
    }
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    return {};
}

support::Expected<TaskSubmission> TaskStore::submit(
        const DurableTask& task, std::string request_id, std::string kind, std::string payload) {
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return std::unexpected(result.error());
    auto existing = load_submission(request_id);
    if (existing) {
        if (auto result = transaction.commit(); !result) return std::unexpected(result.error());
        return existing;
    }
    if (auto result = create_task(task); !result) return std::unexpected(result.error());
    TaskSubmission submission{.id = std::move(request_id),
            .task_id = task.id,
            .kind = std::move(kind),
            .payload = std::move(payload),
            .state = "queued"};
    if (auto result = add_submission(submission); !result) return std::unexpected(result.error());
    if (auto result = transaction.commit(); !result) return std::unexpected(result.error());
    return submission;
}

support::Expected<TaskSubmission> TaskStore::load_submission(std::string_view request_id) {
    auto stmt = db_.prepare("SELECT id, task_id, kind, payload, state, created_at FROM submissions WHERE id = ?1;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, request_id); !result) return std::unexpected(result.error());
    auto row = stmt->step();
    if (!row) return std::unexpected(row.error());
    if (!*row) return std::unexpected(support::make_error(support::ErrorCode::Validation, "Submission does not exist"));
    return TaskSubmission{.id = stmt->column_text(0),
            .task_id = stmt->column_text(1),
            .kind = stmt->column_text(2),
            .payload = stmt->column_text(3),
            .state = stmt->column_text(4),
            .created_at = stmt->column_int64(5)};
}

support::ExpectedVoid TaskStore::update_submission_state(std::string_view task_id, std::string_view state) {
    auto stmt = db_.prepare("UPDATE submissions SET state = ?1 WHERE task_id = ?2;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, state); !result) return result;
    if (auto result = stmt->bind_text(2, task_id); !result) return result;
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    return {};
}

support::ExpectedVoid TaskStore::transition(std::string_view id,
        std::string_view expected_state,
        std::string_view next_state,
        std::string_view checkpoint) {
    const bool allowed = (expected_state == "pending" && next_state == "running") ||
                         (expected_state == "running" && next_state == "completing") ||
                         (expected_state == "completing" &&
                                 (next_state == "completed" || next_state == "failed" || next_state == "aborted"));
    if (!allowed) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Invalid durable task transition"));
    }
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return result;
    auto stmt = db_.prepare("UPDATE tasks SET state = ?1, checkpoint = ?2, updated_at = ?3 "
                            "WHERE id = ?4 AND state = ?5 AND abort_requested = 0 "
                            "AND (?1 != 'completing' OR NOT EXISTS (SELECT 1 FROM tasks AS ancestor "
                            "WHERE ancestor.id = tasks.parent_task_id AND ancestor.state = 'failed'));");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, next_state); !result) return result;
    if (auto result = stmt->bind_text(2, checkpoint); !result) return result;
    if (auto result = stmt->bind_int64(3, now_ms()); !result) return result;
    if (auto result = stmt->bind_text(4, id); !result) return result;
    if (auto result = stmt->bind_text(5, expected_state); !result) return result;
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    if (sqlite3_changes(db_.handle()) != 1) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Task state changed or task is missing"));
    }
    if (next_state == "completed" || next_state == "failed" || next_state == "aborted") {
        if (auto result = update_submission_state(id, "done"); !result) return result;
        if (next_state == "failed") {
            auto children =
                    db_.prepare("WITH RECURSIVE descendants(id) AS (SELECT id FROM tasks WHERE parent_task_id = ?1 "
                                "UNION ALL SELECT tasks.id FROM tasks JOIN descendants ON tasks.parent_task_id = "
                                "descendants.id) UPDATE tasks SET state = 'failed', updated_at = ?2 "
                                "WHERE id IN (SELECT id FROM descendants) AND state NOT IN "
                                "('completed', 'failed', 'aborted');");
            if (!children) return std::unexpected(children.error());
            if (auto result = children->bind_text(1, id); !result) return result;
            if (auto result = children->bind_int64(2, now_ms()); !result) return result;
            if (auto result = children->step(); !result) return std::unexpected(result.error());
            auto submissions =
                    db_.prepare("UPDATE submissions SET state = 'done' WHERE task_id IN "
                                "(WITH RECURSIVE descendants(id) AS (SELECT id FROM tasks WHERE parent_task_id = "
                                "?1 UNION ALL SELECT tasks.id FROM tasks JOIN descendants ON "
                                "tasks.parent_task_id = descendants.id) SELECT id FROM descendants);");
            if (!submissions) return std::unexpected(submissions.error());
            if (auto result = submissions->bind_text(1, id); !result) return result;
            if (auto result = submissions->step(); !result) return std::unexpected(result.error());
        }
    }
    return transaction.commit();
}

support::Expected<std::vector<std::string>> TaskStore::task_descendants(std::string_view id) {
    auto stmt = db_.prepare("WITH RECURSIVE descendants(id) AS (SELECT id FROM tasks WHERE id = ?1 UNION ALL "
                            "SELECT tasks.id FROM tasks JOIN descendants ON tasks.parent_task_id = descendants.id) "
                            "SELECT id FROM descendants;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, id); !result) return std::unexpected(result.error());
    std::vector<std::string> ids;
    while (true) {
        auto row = stmt->step();
        if (!row) return std::unexpected(row.error());
        if (!*row) break;
        ids.push_back(stmt->column_text(0));
    }
    if (ids.empty()) return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task does not exist"));
    return ids;
}

support::ExpectedVoid TaskStore::request_abort(std::string_view id) {
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return result;
    auto ids = task_descendants(id);
    if (!ids) return std::unexpected(ids.error());
    auto stmt = db_.prepare("WITH RECURSIVE descendants(id) AS (SELECT id FROM tasks WHERE id = ?1 UNION ALL "
                            "SELECT tasks.id FROM tasks JOIN descendants ON tasks.parent_task_id = descendants.id) "
                            "UPDATE tasks SET abort_requested = 1, state = CASE WHEN state IN ('pending', 'running') "
                            "THEN 'aborted' ELSE state END, updated_at = ?2 WHERE id IN (SELECT id FROM descendants);");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, id); !result) return result;
    if (auto result = stmt->bind_int64(2, now_ms()); !result) return result;
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    auto submissions = db_.prepare("WITH RECURSIVE descendants(id) AS (SELECT id FROM tasks WHERE id = ?1 UNION ALL "
                                   "SELECT tasks.id FROM tasks JOIN descendants ON tasks.parent_task_id = "
                                   "descendants.id) UPDATE submissions SET state = 'unanswered' WHERE task_id IN "
                                   "(SELECT id FROM descendants WHERE id != ?1 AND (SELECT state FROM tasks WHERE "
                                   "tasks.id = descendants.id) = 'aborted') OR task_id = ?1 AND "
                                   "(SELECT state FROM tasks WHERE id = ?1) = 'aborted';");
    if (!submissions) return std::unexpected(submissions.error());
    if (auto result = submissions->bind_text(1, id); !result) return result;
    if (auto result = submissions->step(); !result) return std::unexpected(result.error());
    return transaction.commit();
}

support::ExpectedVoid TaskStore::finish_aborted(std::string_view id, std::string_view checkpoint) {
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return result;
    auto stmt = db_.prepare("UPDATE tasks SET state = 'aborted', checkpoint = ?1, updated_at = ?2 "
                            "WHERE id = ?3 AND state = 'running' AND abort_requested = 1;");
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, checkpoint); !result) return result;
    if (auto result = stmt->bind_int64(2, now_ms()); !result) return result;
    if (auto result = stmt->bind_text(3, id); !result) return result;
    if (auto result = stmt->step(); !result) return std::unexpected(result.error());
    if (sqlite3_changes(db_.handle()) != 1) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task is not abortable"));
    }
    if (auto result = update_submission_state(id, "unanswered"); !result) return result;
    return transaction.commit();
}

support::Expected<std::vector<DurableTask>> TaskStore::recover_tasks() {
    SqliteTransactionGuard transaction(db_);
    if (auto result = db_.begin_transaction(); !result) return std::unexpected(result.error());
    auto query = db_.prepare(std::format("WITH RECURSIVE ancestry(id, depth) AS (SELECT id, 0 FROM tasks WHERE "
                                         "parent_task_id IS NULL OR parent_task_id = '' UNION ALL SELECT tasks.id, "
                                         "ancestry.depth + 1 FROM tasks JOIN ancestry ON tasks.parent_task_id = "
                                         "ancestry.id) SELECT {} FROM tasks LEFT JOIN (SELECT id, MIN(depth) AS "
                                         "depth FROM ancestry GROUP BY id) AS ordering ON ordering.id = tasks.id WHERE "
                                         "tasks.state = 'running' ORDER BY COALESCE(ordering.depth, 2147483647), "
                                         "tasks.created_at, tasks.id;",
            "tasks.id, tasks.kind, tasks.state, tasks.checkpoint, "
            "tasks.owner_session, tasks.definition_version, tasks.created_at, "
            "tasks.updated_at, tasks.abort_requested, tasks.parent_task_id"));
    if (!query) return std::unexpected(query.error());
    std::vector<DurableTask> recovered;
    while (true) {
        auto row = query->step();
        if (!row) return std::unexpected(row.error());
        if (!*row) break;
        recovered.push_back(read_task(*query));
    }
    auto update =
            db_.prepare("UPDATE tasks SET state = 'pending', updated_at = ?1 WHERE id = ?2 AND state = 'running';");
    if (!update) return std::unexpected(update.error());
    for (auto& task : recovered) {
        if (!task.abort_requested) {
            if (auto result = update->bind_int64(1, now_ms()); !result) return std::unexpected(result.error());
            if (auto result = update->bind_text(2, task.id); !result) return std::unexpected(result.error());
            if (auto result = update->step(); !result) return std::unexpected(result.error());
            if (auto result = update->reset(); !result) return std::unexpected(result.error());
            task.state = "pending";
            task.updated_at = now_ms();
        }
    }
    std::erase_if(recovered, [](const DurableTask& task) { return task.abort_requested; });
    if (auto result = transaction.commit(); !result) return std::unexpected(result.error());
    return recovered;
}

support::Expected<DurableTask> TaskStore::load_task(std::string_view id) {
    auto stmt = db_.prepare(std::format("SELECT {} FROM tasks WHERE id = ?1;", kTaskColumns));
    if (!stmt) return std::unexpected(stmt.error());
    if (auto result = stmt->bind_text(1, id); !result) return std::unexpected(result.error());
    auto row = stmt->step();
    if (!row) return std::unexpected(row.error());
    if (!*row) return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task does not exist"));
    return read_task(*stmt);
}

} // namespace cch::agent::session
