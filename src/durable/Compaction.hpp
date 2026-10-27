#pragma once

#include <cch/chord/Delta.hpp>
#include <cch/durable/Harness.hpp>
#include <cch/durable/Task.hpp>

#include <functional>
#include <memory>

namespace cch::durable {

class WorkflowRuntime;

/// The status of one live compaction task (frozen pi `CompactionStatus`).
struct CompactionStatusRecord {
    TaskId task{};
    std::string reason{};
    bool blocking{false};
    std::int64_t attempt{1};
};

/// The status of compaction task `task`, if listed in the live document
/// (frozen pi `compactionStatus`).
[[nodiscard]] support::Expected<std::optional<CompactionStatusRecord>> live_compaction_status(
        chord::Draft& live, TaskId task);

/// Stage the status of a compaction task created in this commit (frozen pi
/// `addCompactionStatus`).
[[nodiscard]] support::ExpectedVoid add_live_compaction_status(chord::Draft& live, CompactionStatusRecord status);

/// Remove the status of compaction task `task`, and the list once empty
/// (frozen pi `removeCompactionStatus`).
[[nodiscard]] support::ExpectedVoid remove_live_compaction_status(chord::Draft& live, TaskId task);

/// Remove the status of compaction task `task` and record that it failed with
/// `error` (frozen pi `failCompactionStatus`).
[[nodiscard]] support::ExpectedVoid fail_live_compaction_status(
        chord::Draft& live, TaskId task, std::string_view error);

/// Whether any compaction status is listed in the live document: the
/// one-background-compaction-at-a-time check (frozen pi generation
/// `prepare`).
[[nodiscard]] support::Expected<bool> live_has_compaction(chord::Draft& live);

/// Create one `pi.compaction` task with its live status in this commit
/// (frozen pi `createCompaction`): `owner` is the generation that waits for
/// it (a blocking compaction); without one it is conversation-owned, and
/// background unless it is manual.
[[nodiscard]] support::Expected<TaskId> create_compaction_task(Transaction& tx,
        const TaskDefinition& definition,
        ConversationId conversation_id,
        const chord::Json& input,
        std::optional<TaskId> owner,
        bool background);

/// Harness-owned durable compaction runtime: the built-in `pi.compaction`
/// task definition and its phase handlers. Session, Storage, registry
/// options, the clock and the workflow runtime outlive every operation and
/// task callback below.
class CompactionRuntime {
public:
    CompactionRuntime(Session& session,
            Storage& storage,
            const HarnessOptions& options,
            std::shared_ptr<std::move_only_function<std::int64_t()>> clock,
            WorkflowRuntime& workflow);
    CompactionRuntime(const CompactionRuntime&) = delete;
    CompactionRuntime& operator=(const CompactionRuntime&) = delete;
    ~CompactionRuntime() = default;

    /// The built-in `pi.compaction` definition: `select` finds the cut and
    /// asks the `beforeCompact` hooks, `summarize` requests the summary and
    /// places it, `retry` backs off and re-enters `summarize`.
    [[nodiscard]] std::shared_ptr<const TaskDefinition> definition() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
