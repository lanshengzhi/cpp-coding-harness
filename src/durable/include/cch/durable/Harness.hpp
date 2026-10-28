#pragma once

#include <cch/durable/Observation.hpp>
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

    /// Read every live task and owner edge as a structured graph from
    /// committed records (frozen pi `Harness.taskGraph`).
    [[nodiscard]] support::AsyncResult<TaskGraph> task_graph();

    /// Watch the task graph, delivering one exact frame per committed
    /// revision (frozen pi `WatchHandle` over the `TaskGraphView`).
    [[nodiscard]] support::AsyncResult<ViewWatch> watch_task_graph(ViewFrameListener listener);

    /// Hydrate one conversation's committed view (frozen pi
    /// `ConversationViews` snapshot): the conversation record, the visible
    /// entries with the retained head marker, and the five built-in
    /// documents keyed by full kind. Rejects a missing conversation.
    [[nodiscard]] support::AsyncResult<chord::Json> conversation_view(ConversationId conversation_id);

    /// Watch one conversation's committed view, delivering one exact frame
    /// per committed revision touching it (frozen pi `WatchHandle` over the
    /// `ConversationViews`). Rejects a missing conversation.
    [[nodiscard]] support::AsyncResult<ViewWatch> watch_conversation(
            ConversationId conversation_id, ViewFrameListener listener);

    /// Run one atomic transaction on the Session.
    [[nodiscard]] support::AsyncResult<void> commit(
            std::move_only_function<support::Expected<void>(Transaction&)> change);

    /// Start one manual compaction of a conversation (frozen pi
    /// `Harness.compact`): a conversation-owned, non-background task that a
    /// blocking state never waits for. `instructions` steers the
    /// summarizer's focus. Returns the created task id.
    [[nodiscard]] support::Expected<TaskId> compact(
            ConversationId conversation_id, std::optional<std::string> instructions = std::nullopt);

    /// Resume scheduling tasks.
    [[nodiscard]] support::ExpectedVoid resume();

    /// Reconcile surviving running tasks back to pending (call upon reopen).
    [[nodiscard]] support::AsyncResult<void> open_tasks();

    /// Wait for a task to reach terminal status and return its terminal record
    /// receipt (frozen pi `Harness.waitForTask`). Cancelling `stop` rejects only
    /// this wait with a "stop waiting" cancellation error; the task keeps
    /// running and settles on its own.
    [[nodiscard]] support::AsyncResult<TaskRecord> wait_for_task(TaskId id, std::stop_token stop = std::stop_token{});

    /// Resolve once ordinary traversal from `conversation` reaches no live
    /// non-background task, or from every ownerless conversation when
    /// `conversation` is absent (frozen pi `Harness.waitForIdle`,
    /// `Conversation.waitForIdle`). Live background work never holds it.
    /// Cancelling `stop` rejects only this wait with a "stop waiting"
    /// cancellation error.
    [[nodiscard]] support::AsyncResult<void> wait_for_idle(
            std::optional<ConversationId> conversation = std::nullopt, std::stop_token stop = std::stop_token{});

    /// Abort a task. Returns "marked" or "terminal". The Harness must outlive the asynchronous operation.
    [[nodiscard]] support::AsyncResult<std::string> abort_task(TaskId id);

    /// Read the latest complete record for one task.
    [[nodiscard]] support::AsyncResult<std::optional<TaskRecord>> get_task(TaskId id);

    /// Lazily create the reserved root and its five built-in documents in one
    /// transaction. Later calls preserve the existing root's configuration.
    [[nodiscard]] support::AsyncResult<ConversationId> root(RootOptions options = {});

    /// Create a durable branch at one visible entry and copy its selected
    /// conversation documents in the same commit.
    [[nodiscard]] support::AsyncResult<ConversationRecord> fork_conversation(ConversationId parent, EntryId at);

    /// Start a new context, optionally carrying handoff text as its first user
    /// message. The reset is admitted through the ordinary submission boundary.
    [[nodiscard]] support::AsyncResult<void> reset(ConversationId conversation_id, std::optional<std::string> handoff);

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

    /// Abort one conversation's ordinary scope (frozen pi
    /// `Conversation.abort`): withdraw every queued input, keep queued writes,
    /// mark every live non-background task ordinary traversal reaches, and
    /// settle the running run's inputs `unanswered/aborted`. With `background`,
    /// traversal crosses background boundaries, the reached tasks are waited
    /// for, and the wait also covers them. Resolves once the scope is idle.
    [[nodiscard]] support::AsyncResult<void> abort_conversation(
            ConversationId conversation_id, bool background = false);

    /// Close the harness.
    [[nodiscard]] support::AsyncResult<void> close();

private:
    struct Impl;
    explicit Harness(std::unique_ptr<Impl> impl);
    /// One abort pass: withdraw the in-scope queued inputs and mark every live
    /// reached task in one commit, then wait for the reached tasks (with
    /// `background`) and the idle scope, re-running until the scope is idle
    /// (frozen pi `Conversation.abort`).
    void abort_conversation_pass(
            ConversationId conversation_id, bool background, support::AsyncCompletion<void, support::Error> done);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::durable
