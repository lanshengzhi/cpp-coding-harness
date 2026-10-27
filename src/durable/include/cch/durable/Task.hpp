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
///
/// A returned `terminal` state is stored as `completing` while ordinary owned
/// work below the task is live; the scheduler writes the terminal record once
/// that work drains (spec §5.5).
struct NextTaskState {
    std::string status{};
    std::optional<chord::Json> checkpoint{std::nullopt};
    /// The tasks to wait for; only meaningful for `waiting`.
    std::vector<TaskId> on{};
    /// The join policy; only meaningful for `waiting`.
    std::optional<JoinPolicy> policy{std::nullopt};
    std::optional<TaskOutcome> outcome{std::nullopt};

    [[nodiscard]] static NextTaskState running(chord::Json checkpoint) {
        return NextTaskState{.status = "running", .checkpoint = std::move(checkpoint)};
    }

    /// Park the task until every task in `on` is terminal (frozen
    /// `TaskState` `waiting`).
    [[nodiscard]] static NextTaskState waiting(chord::Json checkpoint, std::vector<TaskId> on, JoinPolicy policy) {
        return NextTaskState{
                .status = "waiting", .checkpoint = std::move(checkpoint), .on = std::move(on), .policy = policy};
    }

    [[nodiscard]] static NextTaskState terminal(TaskOutcome outcome) {
        return NextTaskState{.status = "terminal", .outcome = std::move(outcome)};
    }
};

/// Migrated input and checkpoint produced by a task definition migration (frozen pi `TaskDefinition.migrate` result).
struct MigratedTaskState {
    chord::Json input{};
    chord::Json checkpoint{};

    [[nodiscard]] bool operator==(const MigratedTaskState&) const = default;
};

using MigrateHook = std::move_only_function<support::Expected<MigratedTaskState>(
        const chord::Json& input, const chord::Json& checkpoint, std::int64_t from_version)>;

class TaskRuntime;

/// Handler for a named task phase hook (frozen pi `HookRegistration`).
using TaskHook =
        std::move_only_function<support::AsyncResult<chord::Json>(const chord::Json& input, TaskRuntime& runtime)>;

/// Invoker passed to `TaskRuntime::each_hook` (frozen pi `HookRunner.each`).
using HookInvokeHook = std::move_only_function<support::AsyncResult<void>(TaskHook& handler)>;

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

    /// Forward a non-fatal failure to `HarnessOptions::on_report`.
    virtual void report(const support::Error& error) = 0;

    /// Run matching handlers for `name` in extension order, isolating ordinary errors through `report`
    /// and propagating cancellation once `stop_token().stop_requested()` is set (frozen pi `HookRunner.each`).
    [[nodiscard]] virtual support::AsyncResult<void> each_hook(std::string_view name, HookInvokeHook invoke) = 0;

    /// Invoke each matching handler for `name` with `input`.
    [[nodiscard]] virtual support::AsyncResult<void> each_hook(
            std::string_view name, chord::Json input = chord::Json{nullptr}) = 0;
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

    [[nodiscard]] bool has_migrate() const noexcept;
    [[nodiscard]] support::Expected<MigratedTaskState> migrate(
            const chord::Json& input, const chord::Json& checkpoint, std::int64_t from_version) const;
    [[nodiscard]] bool has_hook(std::string_view name) const noexcept;

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
///
/// `build()` consumes the builder: it moves the definition out, so a builder is
/// valid for exactly one successful `build()`. A second `build()` on the same
/// builder, or any mutator called after a successful `build()`, returns a
/// `Validation` error (`build()`) or is a no-op that leaves the builder
/// buildable-but-empty, which the next `build()` also reports as `Validation`.
class TaskDefinitionBuilder {
public:
    explicit TaskDefinitionBuilder(std::string kind, std::int64_t version = 1);
    TaskDefinitionBuilder(TaskDefinitionBuilder&&) noexcept;
    TaskDefinitionBuilder& operator=(TaskDefinitionBuilder&&) noexcept;
    ~TaskDefinitionBuilder();

    TaskDefinitionBuilder(const TaskDefinitionBuilder&) = delete;
    TaskDefinitionBuilder& operator=(const TaskDefinitionBuilder&) = delete;

    /// Mutators are only meaningful before the first successful `build()`.
    /// After it, the builder holds no definition and the mutator is a no-op.
    TaskDefinitionBuilder& initial(std::move_only_function<chord::Json(const chord::Json&)> fn);
    TaskDefinitionBuilder& phase(std::string name, PhaseHandler handler);
    TaskDefinitionBuilder& abort(AbortHandler handler);
    TaskDefinitionBuilder& migrate(MigrateHook handler);
    TaskDefinitionBuilder& hook(std::string name);

    /// Build the definition and consume this builder. Calling it again on the
    /// same builder returns a `Validation` error instead of reading a moved-out
    /// definition.
    [[nodiscard]] support::Expected<TaskDefinition> build();

    /// Whether this builder still owns a definition, i.e. `build()` has not
    /// succeeded yet.
    [[nodiscard]] bool valid() const noexcept;

private:
    std::unique_ptr<TaskDefinition::Impl> impl_;
};

} // namespace cch::durable
