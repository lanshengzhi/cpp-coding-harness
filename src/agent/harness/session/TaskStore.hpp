#pragma once

#include "agent/harness/session/SqliteDatabase.hpp"

#include <cch/support/Error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::agent::session {

struct DurableTask {
    std::string id;
    std::string kind;
    std::string state;
    std::string checkpoint;
    std::string owner_session;
    std::int64_t definition_version{0};
    std::int64_t created_at{0};
    std::int64_t updated_at{0};
    bool abort_requested{false};
};

struct TaskSubmission {
    std::string id;
    std::string task_id;
    std::string payload;
    std::int64_t created_at{0};
};

class TaskStore final {
public:
    explicit TaskStore(SqliteDatabase db) : db_(std::move(db)) {}

    [[nodiscard]] static support::Expected<TaskStore> open(const std::filesystem::path& path);
    [[nodiscard]] static support::Expected<TaskStore> open_memory();
    [[nodiscard]] support::ExpectedVoid init_schema();
    [[nodiscard]] support::ExpectedVoid create_task(const DurableTask& task);
    [[nodiscard]] support::ExpectedVoid add_submission(const TaskSubmission& submission);
    [[nodiscard]] support::ExpectedVoid transition(std::string_view id,
            std::string_view expected_state,
            std::string_view next_state,
            std::string_view checkpoint);
    [[nodiscard]] support::ExpectedVoid request_abort(std::string_view id);
    [[nodiscard]] support::Expected<std::vector<DurableTask>> recover_tasks();
    [[nodiscard]] support::Expected<DurableTask> load_task(std::string_view id);

    [[nodiscard]] SqliteDatabase& db_for_test() noexcept { return db_; }

private:
    SqliteDatabase db_;
};

} // namespace cch::agent::session
