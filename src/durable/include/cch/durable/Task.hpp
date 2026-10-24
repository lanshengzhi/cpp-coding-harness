#pragma once

#include <cch/chord/Json.hpp>
#include <cch/durable/Records.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::durable {

class Transaction;

/// Next state a task phase or abort handler commits (frozen pi `NextTaskState`).
struct NextTaskState {
    std::string status{};
    std::optional<chord::Json> checkpoint{std::nullopt};
    std::optional<TaskOutcome> outcome{std::nullopt};

    [[nodiscard]] static NextTaskState running(chord::Json checkpoint) {
        return NextTaskState{.status = "running", .checkpoint = std::move(checkpoint)};
    }

    [[nodiscard]] static NextTaskState terminal(TaskOutcome outcome) {
        return NextTaskState{.status = "terminal", .outcome = std::move(outcome)};
    }
};

/// Execution runtime provided to a task phase or abort handler (frozen pi `TaskRuntime`).
class TaskRuntime {
public:
    virtual ~TaskRuntime() = default;

    [[nodiscard]] virtual TaskId task_id() const noexcept = 0;
    [[nodiscard]] virtual ConversationId conversation_id() const noexcept = 0;
    [[nodiscard]] virtual const std::stop_token& stop_token() const noexcept = 0;

    /// Read or stage a memo (first-writer-wins).
    [[nodiscard]] virtual support::AsyncResult<chord::Json> memo(std::string key, chord::Json candidate) = 0;

    /// Commit progress in the task's execution.
    using CommitCallback =
            std::move_only_function<support::Expected<std::optional<NextTaskState>>(Transaction&, const TaskRecord&)>;
    [[nodiscard]] virtual support::AsyncResult<void> commit(CommitCallback change) = 0;
};

using PhaseHandler = std::move_only_function<support::AsyncResult<void>(const TaskRecord& task, TaskRuntime& runtime)>;

using AbortHandler = std::move_only_function<support::AsyncResult<void>(const TaskRecord& task, TaskRuntime& runtime)>;

class TaskDefinitionBuilder;

/// Definition of an executable durable task kind (frozen pi `defineTask`).
class TaskDefinition {
public:
    TaskDefinition(TaskDefinition&&) noexcept;
    TaskDefinition& operator=(TaskDefinition&&) noexcept;
    ~TaskDefinition();

    TaskDefinition(const TaskDefinition&) = delete;
    TaskDefinition& operator=(const TaskDefinition&) = delete;

    [[nodiscard]] const std::string& kind() const noexcept;
    [[nodiscard]] std::int64_t version() const noexcept;
    [[nodiscard]] chord::Json initial(const chord::Json& input) const;

    [[nodiscard]] bool has_phase(std::string_view phase) const;
    [[nodiscard]] support::AsyncResult<void> run_phase(
            std::string_view phase, const TaskRecord& task, TaskRuntime& runtime) const;
    [[nodiscard]] support::AsyncResult<void> run_abort(const TaskRecord& task, TaskRuntime& runtime) const;

private:
    friend class TaskDefinitionBuilder;

    struct Impl;
    explicit TaskDefinition(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Builder for defining a `TaskDefinition`.
class TaskDefinitionBuilder {
public:
    explicit TaskDefinitionBuilder(std::string kind, std::int64_t version = 1);
    TaskDefinitionBuilder(TaskDefinitionBuilder&&) noexcept;
    TaskDefinitionBuilder& operator=(TaskDefinitionBuilder&&) noexcept;
    ~TaskDefinitionBuilder();

    TaskDefinitionBuilder(const TaskDefinitionBuilder&) = delete;
    TaskDefinitionBuilder& operator=(const TaskDefinitionBuilder&) = delete;

    TaskDefinitionBuilder& initial(std::move_only_function<chord::Json(const chord::Json&)> fn);
    TaskDefinitionBuilder& phase(std::string name, PhaseHandler handler);
    TaskDefinitionBuilder& abort(AbortHandler handler);

    [[nodiscard]] support::Expected<TaskDefinition> build();

private:
    std::unique_ptr<TaskDefinition::Impl> impl_;
};

} // namespace cch::durable
