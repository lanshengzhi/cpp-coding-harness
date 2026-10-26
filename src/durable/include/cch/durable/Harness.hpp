#pragma once

#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include <cch/durable/Workflow.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::durable {

/// Sink receiving non-fatal failures such as isolated hook errors or failed task migrations (frozen pi
/// `HarnessOptions.onReport`).
using ReportSink = std::move_only_function<void(const support::Error&)>;

/// Options for opening a Harness (frozen pi `HarnessOptions`).
struct HarnessOptions {
    std::vector<TaskDefinition> tasks{};
    std::move_only_function<std::int64_t()> now{};
    /// When supplied, the registry must outlive this Harness and its operations.
    Registry* registry{nullptr};
    ai::ModelStreamFactory stream{};
    HarnessSettings settings{};
    ReportSink on_report{};
};

/// Top-level durable runtime harness (frozen pi `Harness`).
class Harness {
public:
    Harness(Harness&&) noexcept;
    Harness& operator=(Harness&&) noexcept;
    ~Harness();

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// Open a Harness over storage. The caller keeps `storage` alive until `close()` completes.
    [[nodiscard]] static support::AsyncResult<std::unique_ptr<Harness>> open(
            Storage& storage, HarnessOptions options = {});

    /// Access the underlying durable Session.
    [[nodiscard]] Session& session() noexcept;

    /// Look up a registered task definition by kind.
    [[nodiscard]] const TaskDefinition* task_definition(std::string_view kind) const noexcept;

    /// Inspect scheduling state and live tasks without running task code.
    [[nodiscard]] support::AsyncResult<HarnessInspection> inspect();

    /// Run one atomic transaction on the Session.
    [[nodiscard]] support::AsyncResult<void> commit(
            std::move_only_function<support::Expected<void>(Transaction&)> change);

    /// Resume scheduling tasks.
    [[nodiscard]] support::ExpectedVoid resume();

    /// Reconcile surviving running tasks back to pending (call upon reopen).
    [[nodiscard]] support::AsyncResult<void> open_tasks();

    /// Wait for a task to reach terminal status and return its terminal record receipt.
    [[nodiscard]] support::AsyncResult<TaskRecord> wait_for_task(TaskId id);

    /// Abort a task. Returns "marked" or "terminal". The Harness must outlive the asynchronous operation.
    [[nodiscard]] support::AsyncResult<std::string> abort_task(TaskId id);

    /// Read the latest complete record for one task.
    [[nodiscard]] support::AsyncResult<std::optional<TaskRecord>> get_task(TaskId id);

    /// Lazily create the reserved root and its five built-in documents in one
    /// transaction. Later calls preserve the existing root's configuration.
    [[nodiscard]] support::AsyncResult<ConversationId> root(RootOptions options = {});

    /// Admit one submission in one commit (frozen pi `Harness.submit` /
    /// `admitSubmission`): a known request ID returns its existing submission
    /// without writing, a display-only write settles at once, a busy
    /// conversation queues (or rejects `WhenBusy::Reject` with
    /// `ConversationBusy`), and an idle conversation places the input and
    /// starts a run. The Harness must outlive all returned work.
    [[nodiscard]] support::AsyncResult<Submission> submit(ConversationId conversation_id, SubmissionDraft draft);

    /// Withdraw a queued submission and remove its inbox item (frozen pi
    /// `Harness.abortSubmission`): `"aborted"`, `"already_placed"`,
    /// `"settled"` or `"not_found"`. The optional conversation narrows the
    /// lookup, mirroring the frozen conversation-scoped abort.
    [[nodiscard]] support::AsyncResult<std::string> abort_submission(
            SubmissionId id, std::optional<ConversationId> conversation_id = std::nullopt);

    /// Withdraw every queued input of the conversation, keep queued writes,
    /// and settle the running run's inputs `unanswered/aborted` (frozen pi
    /// `Conversation.abort`, admitted-behavior slice; background crossing
    /// stays out of scope). Resolves once the conversation is idle.
    [[nodiscard]] support::AsyncResult<void> abort_conversation(ConversationId conversation_id);

    /// Close the harness.
    [[nodiscard]] support::AsyncResult<void> close();

private:
    struct Impl;
    explicit Harness(std::unique_ptr<Impl> impl);
    /// One abort pass: withdraw queued inputs and read the staged live run in
    /// one commit, abort the marked run, then re-run until the conversation
    /// is idle (frozen pi `Conversation.abort`).
    void abort_conversation_pass(ConversationId conversation_id, support::AsyncCompletion<void, support::Error> done);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
