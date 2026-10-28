#include <cch/durable/Harness.hpp>

#include <cch/durable/detail/SessionLineAccess.hpp>

#include "durable/Compaction.hpp"
#include "durable/SessionErrors.hpp"
#include "durable/WorkflowRuntime.hpp"

#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cch::durable {
namespace {

inline support::ExpectedVoid await_async_void(support::AsyncResult<void> result) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    support::ExpectedVoid outcome = std::unexpected(support::Error{});
    std::move(result).start([&m, &cv, &done, &outcome](support::ExpectedVoid terminal) mutable noexcept {
        std::lock_guard l(m);
        outcome = std::move(terminal);
        done = true;
        cv.notify_one();
    });
    std::unique_lock l(m);
    cv.wait(l, [&] { return done; });
    return outcome;
}

inline support::Expected<chord::Json> await_async_json(support::AsyncResult<chord::Json> result) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    support::Expected<chord::Json> outcome = std::unexpected(support::Error{});
    std::move(result).start([&m, &cv, &done, &outcome](support::Expected<chord::Json> terminal) mutable noexcept {
        std::lock_guard l(m);
        outcome = std::move(terminal);
        done = true;
        cv.notify_one();
    });
    std::unique_lock l(m);
    cv.wait(l, [&] { return done; });
    return outcome;
}

} // namespace

// ── Submission::Registry ────────────────────────────────────────────────────
//
// Resolves settled submissions from commit publications (frozen pi
// `Submissions.#observe`) and owns every `Submission::wait` waiter. Session
// close or failure rejects every waiter with the frozen close error;
// cancelling one wait settles only that waiter with a "stop waiting" error
// while the submission itself stays placed.

class Submission::Registry {
public:
    Registry(Session& session, Storage& storage);

    /// Deliver the settled submission changes of one commit publication to
    /// their waiters (frozen pi `Submissions.#observe`).
    void observe(const CommitPublication& publication);

    /// Reject every registered waiter (Session close or failure).
    void reject_all(const support::Error& error);

    [[nodiscard]] support::AsyncResult<SubmissionRecord> status(SubmissionId id);
    [[nodiscard]] support::AsyncResult<SettledSubmission> wait(SubmissionId id, std::stop_token stop);
    [[nodiscard]] support::AsyncResult<std::string> abort_submission(
            SubmissionId id, std::optional<ConversationId> conversation_id);

private:
    struct Waiter;

    [[nodiscard]] support::Error missing(SubmissionId id) const;

    Session& session_; // outlives every operation (Harness::open contract)
    Storage& storage_; // outlives every operation (Harness::open contract)
    std::mutex mutex_;
    std::optional<support::Error> wait_error_{};
    std::map<std::int64_t, std::vector<std::shared_ptr<Waiter>>> waiters_{};
};

/// Shared state of one registered wait: the completion and the atomic
/// claim that makes the resolve, reject-all, and cancellation paths meet
/// exactly once. Waiter nodes stay alive through their shared state, so a
/// cancellation racing a resolution never touches freed memory. The outcome
/// claim uses the default seq_cst ordering: every claimant already
/// serializes on the registry mutex or holds the waiter through the waiter
/// list's shared ownership, so no weaker ordering (nor a separate
/// justification at each site) is needed.
struct Submission::Registry::Waiter {
    std::atomic<unsigned> outcome{0}; ///< 0 pending, 1 resolved, 2 cancelled
    std::optional<support::AsyncCompletion<SettledSubmission, support::Error>> completion{};
    std::unique_ptr<std::stop_callback<std::move_only_function<void()>>> cancellation{};
};

Submission::Registry::Registry(Session& session, Storage& storage) : session_(session), storage_(storage) {}

support::AsyncResult<SubmissionRecord> Submission::status() const { return registry_->status(id); }

support::AsyncResult<SettledSubmission> Submission::wait(std::stop_token stop) const {
    return registry_->wait(id, std::move(stop));
}

support::AsyncResult<std::string> Submission::abort() const { return registry_->abort_submission(id, conversation_id); }

void Submission::Registry::observe(const CommitPublication& publication) {
    std::vector<std::pair<std::shared_ptr<Waiter>, SettledSubmission>> resolved;
    {
        std::lock_guard lock(mutex_);
        for (const CommitChange& change : publication.changes) {
            const auto* submission = std::get_if<SubmissionCommitChange>(&change);
            if (submission == nullptr || !submission_settled(submission->value.status)) {
                continue;
            }
            const SettledSubmission settled = settled_submission(submission->value);
            auto bucket = waiters_.find(submission->value.id.value);
            if (bucket == waiters_.end()) {
                continue;
            }
            for (const std::shared_ptr<Waiter>& waiter : bucket->second) {
                if (waiter->outcome.exchange(1) == 0) {
                    resolved.emplace_back(waiter, settled);
                }
            }
            waiters_.erase(bucket);
        }
    }
    for (auto& [waiter, settled] : resolved) {
        auto completion = std::move(waiter->completion);
        if (completion.has_value()) {
            (*completion)(std::move(settled));
        }
    }
}

void Submission::Registry::reject_all(const support::Error& error) {
    std::vector<std::shared_ptr<Waiter>> rejected;
    {
        std::lock_guard lock(mutex_);
        wait_error_ = error;
        for (auto& [_, bucket] : waiters_) {
            for (const std::shared_ptr<Waiter>& waiter : bucket) {
                if (waiter->outcome.exchange(1) == 0) {
                    rejected.push_back(waiter);
                }
            }
        }
        waiters_.clear();
    }
    for (const std::shared_ptr<Waiter>& waiter : rejected) {
        auto completion = std::move(waiter->completion);
        if (completion.has_value()) {
            (*completion)(std::unexpected(error));
        }
    }
}

support::Error Submission::Registry::missing(SubmissionId id) const {
    return support::make_error(support::ErrorCode::Validation, std::format("Submission {} does not exist", id.value));
}

support::AsyncResult<SubmissionRecord> Submission::Registry::status(SubmissionId id) {
    auto record = storage_.submission(id);
    if (!record) {
        return support::AsyncResult<SubmissionRecord>{std::unexpected(record.error())};
    }
    if (!record->has_value()) {
        return support::AsyncResult<SubmissionRecord>{std::unexpected(missing(id))};
    }
    return support::AsyncResult<SubmissionRecord>{std::move(*(*record))};
}

support::AsyncResult<SettledSubmission> Submission::Registry::wait(SubmissionId id, std::stop_token stop) {
    return support::AsyncResult<SettledSubmission>{support::AsyncProducer<SettledSubmission, support::Error>{
            [this, id, stop = std::move(stop)](
                    support::AsyncCompletion<SettledSubmission, support::Error> done) mutable noexcept {
                std::optional<support::Error> immediate_error;
                std::optional<SettledSubmission> immediate_value;
                std::shared_ptr<Waiter> waiter;
                {
                    std::lock_guard lock(mutex_);
                    if (wait_error_.has_value()) {
                        immediate_error = *wait_error_;
                    } else if (auto record = storage_.submission(id); !record) {
                        immediate_error = record.error();
                    } else if (!record->has_value()) {
                        immediate_error = missing(id);
                    } else if (submission_settled((**record).status)) {
                        immediate_value = settled_submission(**record);
                    } else if (stop.stop_requested()) {
                        immediate_error = support::make_error(support::ErrorCode::Cancelled, "stop waiting");
                    } else {
                        waiter = std::make_shared<Waiter>();
                        waiter->completion = std::move(done);
                        waiters_[id.value].push_back(waiter);
                    }
                }
                if (immediate_value.has_value()) {
                    done(std::move(*immediate_value));
                    return;
                }
                if (immediate_error.has_value()) {
                    done(std::unexpected(*immediate_error));
                    return;
                }
                // Arm the cancellation outside the lock. The callback only
                // touches the shared waiter state (one claim wins), so it
                // never needs the registry mutex; a stop that raced the
                // registration fires it immediately. It captures the waiter
                // weakly: a strong capture would cycle with the stop_callback
                // the waiter owns and leak the node after its bucket is
                // erased, so a later fire on an already-claimed (destroyed)
                // waiter locks to nothing and skips.
                waiter->cancellation = std::make_unique<std::stop_callback<std::move_only_function<void()>>>(
                        stop, [weak = std::weak_ptr<Waiter>{waiter}] {
                            const std::shared_ptr<Waiter> waiter = weak.lock();
                            if (waiter == nullptr) {
                                return;
                            }
                            if (waiter->outcome.exchange(2) != 0) {
                                return;
                            }
                            auto completion = std::move(waiter->completion);
                            if (completion.has_value()) {
                                (*completion)(std::unexpected(
                                        support::make_error(support::ErrorCode::Cancelled, "stop waiting")));
                            }
                        });
            }}};
}

support::AsyncResult<std::string> Submission::Registry::abort_submission(
        SubmissionId id, std::optional<ConversationId> conversation_id) {
    // Withdraw a queued submission and remove its inbox item; placed inputs
    // and settled submissions are only reported (frozen pi `Submissions.abort`).
    // The read, the decision, and the settle/remove share one Session commit:
    // an interleaved boundary placement therefore cannot slip between the
    // read and the write to regress a placed record or misreport a settled one.
    auto result = std::make_shared<std::string>();
    return support::AsyncResult<std::string>{support::AsyncProducer<std::string, support::Error>{
            [this, id, conversation_id, result](
                    support::AsyncCompletion<std::string, support::Error> done) mutable noexcept {
                session_.commit([id, conversation_id, result](Transaction& tx) -> support::ExpectedVoid {
                            auto record = tx.submission(id);
                            if (!record) {
                                return std::unexpected(record.error());
                            }
                            if (!record->has_value() ||
                                    (conversation_id.has_value() && (**record).conversation_id != *conversation_id)) {
                                *result = "not_found";
                                return {};
                            }
                            const SubmissionRecord& current = **record;
                            if (current.status == "queued") {
                                auto settled = tx.settle_submission(id,
                                        SubmissionSettlement{.status = "unanswered", .reason = std::string{"aborted"}});
                                if (!settled) {
                                    return std::unexpected(settled.error());
                                }
                                auto inbox = tx.doc(
                                        inbox_document(), DocumentAddress{.conversation_id = current.conversation_id});
                                if (!inbox) {
                                    return std::unexpected(inbox.error());
                                }
                                auto removed = remove_inbox_item(*inbox, id);
                                if (!removed) {
                                    return std::unexpected(removed.error());
                                }
                                *result = "aborted";
                                return {};
                            }
                            *result = current.status == "placed" ? "already_placed" : "settled";
                            return {};
                        })
                        .start([done = std::move(done), result](support::ExpectedVoid outcome) mutable noexcept {
                            if (!outcome) {
                                done(std::unexpected(outcome.error()));
                            } else {
                                done(std::move(*result));
                            }
                        });
            }}};
}

namespace {

[[nodiscard]] std::int64_t current_time(const std::shared_ptr<std::move_only_function<std::int64_t()>>& clock) {
    return (*clock)();
}

[[nodiscard]] bool can_reserve(const TaskDefinition& def, const TaskRecord& record) noexcept {
    return def.version() == record.version || (def.version() > record.version && def.has_migrate());
}

/// Whether a task record is still live: anything but `terminal` (frozen pi
/// `LIVE_STATUSES`).
[[nodiscard]] bool task_live(const TaskRecord& record) noexcept { return record.state.status != "terminal"; }

/// Whether the record holds or ended with an outcome other than `completed`
/// (frozen pi `failedOutcome`).
[[nodiscard]] bool failed_outcome(const TaskRecord& record) noexcept {
    if (record.state.status != "completing" && record.state.status != "terminal") return false;
    return !record.state.outcome.has_value() || record.state.outcome->status != "completed";
}

/// A live owner's durable cancellation intent: its abort mark, or a held
/// outcome other than `completed` (frozen pi `cancellationIntent`).
[[nodiscard]] bool cancellation_intent(const TaskRecord& record) noexcept {
    return task_live(record) && (record.abort_requested || failed_outcome(record));
}

/// The ownership tree of one scheduling pass (frozen pi `TaskScheduler.#below`,
/// `#edges`, `#roots`): tasks and conversations with their owner edges.
///
/// A task's parent is its owner task, or its conversation; a conversation's
/// parent is its owner task, if any. Walks down this tree decide what ordinary
/// work holds a task's outcome, what a conversation abort reaches, and whether a
/// scope is idle.
class OwnershipIndex {
public:
    OwnershipIndex(std::vector<TaskRecord> tasks, std::vector<ConversationRecord> conversations) {
        for (const TaskRecord& task : tasks)
            tasks_.emplace(task.id.value, task);
        for (const ConversationRecord& conversation : conversations) {
            conversations_.emplace(conversation.id.value, conversation);
        }
        for (const auto& [id, task] : tasks_) {
            if (task.owner.has_value()) {
                owned_tasks_[task.owner->value].push_back(id);
            } else {
                conversation_tasks_[task.conversation_id.value].push_back(id);
            }
        }
        for (const auto& [id, conversation] : conversations_) {
            if (conversation.owner.has_value()) {
                owned_conversations_[conversation.owner->task_id.value].push_back(id);
            }
        }
    }

    [[nodiscard]] const TaskRecord* task(TaskId id) const {
        const auto found = tasks_.find(id.value);
        return found == tasks_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const ConversationRecord* conversation(ConversationId id) const {
        const auto found = conversations_.find(id.value);
        return found == conversations_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const std::map<std::int64_t, TaskRecord>& tasks() const noexcept { return tasks_; }

    /// Live tasks below `owner` that ordinary traversal reaches: a background
    /// task is reached but neither counted as owned work nor entered, so its
    /// own subtree holds nothing above it (frozen `#ownedLive`). Tasks in
    /// `done` are neither counted nor entered: their outcome already settled.
    [[nodiscard]] std::vector<TaskId> owned_live(TaskId owner, const std::set<std::int64_t>* done = nullptr) const {
        std::vector<TaskId> found;
        walk_down(
                TaskNode{owner.value},
                false,
                [&](const TaskRecord& record, bool is_background) {
                    if (!is_background && task_live(record)) found.push_back(record.id);
                },
                [done](const TaskRecord& record) { return done == nullptr || !done->contains(record.id.value); });
        return found;
    }

    /// Whether ordinary traversal from `conversation` reaches no live
    /// non-background task (frozen `#idle`).
    [[nodiscard]] bool idle(ConversationId conversation) const {
        bool live = false;
        walk_down(
                ConversationNode{conversation.value},
                false,
                [&](const TaskRecord& record, bool is_background) {
                    if (!is_background && task_live(record)) live = true;
                },
                [](const TaskRecord&) { return true; });
        return !live;
    }

    /// Whether every ownerless conversation's ordinary scope is idle (frozen
    /// `Harness.waitForIdle`).
    [[nodiscard]] bool all_idle() const {
        for (const auto& [id, conversation] : conversations_) {
            if (conversation.owner.has_value()) continue;
            if (!idle(conversation.id)) return false;
        }
        return true;
    }

    /// Live tasks ordinary traversal from `conversation` reaches, in walk order.
    /// With `cross_background`, background boundaries are crossed and the
    /// background tasks themselves are reached too.
    [[nodiscard]] std::vector<TaskId> reachable(ConversationId conversation, bool cross_background) const {
        std::vector<TaskId> reached;
        walk_down(
                ConversationNode{conversation.value},
                cross_background,
                [&](const TaskRecord& record, bool is_background) {
                    if (cross_background || !is_background) {
                        if (task_live(record)) reached.push_back(record.id);
                    }
                },
                [](const TaskRecord&) { return true; });
        return reached;
    }

    /// Live non-background tasks below each live owner with cancellation intent,
    /// not entering another such owner, which has its own walk (frozen
    /// `#reconcile`'s cascade derivation). Marking them is the bottom-up abort
    /// cascade: a marked owner keeps its abort handler until its ordinary owned
    /// work is gone.
    [[nodiscard]] std::vector<TaskId> cascade_targets() const {
        std::vector<TaskId> targets;
        for (const auto& [id, owner] : tasks_) {
            if (!cancellation_intent(owner)) continue;
            walk_down(
                    TaskNode{id},
                    false,
                    [&](const TaskRecord& record, bool is_background) {
                        if (!is_background && task_live(record) && !record.abort_requested) {
                            targets.push_back(record.id);
                        }
                    },
                    [this](const TaskRecord& record) {
                        const TaskRecord* live = task(record.id);
                        return live != nullptr && !cancellation_intent(*live);
                    });
        }
        return targets;
    }

    /// Whether walking up from `start` reaches `conversation` without passing a
    /// background owner (frozen `#inScope`).
    [[nodiscard]] bool in_scope(TaskId start, ConversationId conversation) const {
        const TaskRecord* current = task(start);
        while (current != nullptr) {
            if (current->background) return false;
            if (current->conversation_id == conversation) {
                // A task hangs from its owner task, not its conversation, until
                // the chain reaches a task without an owner.
                if (!current->owner.has_value()) return true;
            }
            if (!current->owner.has_value()) {
                return current->conversation_id == conversation;
            }
            current = task(*current->owner);
        }
        return false;
    }

private:
    /// A node of the ownership tree: a task, or a conversation.
    struct TaskNode {
        std::int64_t value{0};
    };
    struct ConversationNode {
        std::int64_t value{0};
    };
    using Node = std::variant<TaskNode, ConversationNode>;

    /// Visit every task the walk from `start` reaches. Background tasks are
    /// reached but not entered unless `cross_background`; `enter` decides
    /// whether a reached task is descended into.
    template <typename Visit, typename Enter>
    void walk_down(Node start, bool cross_background, Visit visit, Enter enter) const {
        std::vector<Node> pending{start};
        while (!pending.empty()) {
            const Node node = pending.back();
            pending.pop_back();
            if (const auto* conversation = std::get_if<ConversationNode>(&node)) {
                const auto children = conversation_tasks_.find(conversation->value);
                if (children == conversation_tasks_.end()) continue;
                for (const std::int64_t id : children->second) {
                    const auto found = tasks_.find(id);
                    if (found == tasks_.end()) continue;
                    visit(found->second, found->second.background);
                    if ((cross_background || !found->second.background) && enter(found->second)) {
                        pending.push_back(TaskNode{id});
                    }
                }
                continue;
            }
            const std::int64_t id = std::get<TaskNode>(node).value;
            if (const auto conversations = owned_conversations_.find(id); conversations != owned_conversations_.end()) {
                for (const std::int64_t owned : conversations->second) {
                    pending.push_back(ConversationNode{owned});
                }
            }
            const auto children = owned_tasks_.find(id);
            if (children == owned_tasks_.end()) continue;
            for (const std::int64_t child : children->second) {
                const auto found = tasks_.find(child);
                if (found == tasks_.end()) continue;
                visit(found->second, found->second.background);
                if ((cross_background || !found->second.background) && enter(found->second)) {
                    pending.push_back(TaskNode{child});
                }
            }
        }
    }

    std::map<std::int64_t, TaskRecord> tasks_;
    std::map<std::int64_t, ConversationRecord> conversations_;
    std::map<std::int64_t, std::vector<std::int64_t>> owned_tasks_;
    std::map<std::int64_t, std::vector<std::int64_t>> owned_conversations_;
    std::map<std::int64_t, std::vector<std::int64_t>> conversation_tasks_;
};

/// Shared state of one registered task wait: the completion and the atomic
/// claim that makes the resolve, reject-all, and cancellation paths meet
/// exactly once (the `Submission::Registry::Waiter` shape, for a task receipt).
struct TaskWaiter {
    std::atomic<unsigned> outcome{0}; ///< 0 pending, 1 resolved, 2 cancelled
    std::optional<support::AsyncCompletion<TaskRecord, support::Error>> completion{};
    std::unique_ptr<std::stop_callback<std::move_only_function<void()>>> cancellation{};
};

/// Shared state of one registered idle wait; same claim protocol as
/// `TaskWaiter`.
struct IdleWaiter {
    std::atomic<unsigned> outcome{0}; ///< 0 pending, 1 resolved, 2 cancelled
    std::optional<support::AsyncCompletion<void, support::Error>> completion{};
    std::unique_ptr<std::stop_callback<std::move_only_function<void()>>> cancellation{};
};

class HarnessTaskRuntime final : public TaskRuntime {
public:
    HarnessTaskRuntime(Session& session,
            TaskId id,
            ConversationId conv_id,
            const TaskDefinition& task_def,
            std::vector<std::shared_ptr<TaskHookRegistration>> phase_hooks,
            std::stop_token stop_token,
            std::shared_ptr<std::move_only_function<std::int64_t()>> now_fn,
            std::move_only_function<void(const support::Error&)> report_fn,
            bool allow_abort_commit = false,
            bool abort_invocation = false)
        : session_(session), task_id_(id), conversation_id_(conv_id), task_def_(task_def),
          phase_hooks_(std::move(phase_hooks)), stop_token_(std::move(stop_token)), now_fn_(std::move(now_fn)),
          report_fn_(std::move(report_fn)), allow_abort_commit_(allow_abort_commit),
          abort_invocation_(abort_invocation) {}

    [[nodiscard]] TaskId task_id() const noexcept override { return task_id_; }
    [[nodiscard]] ConversationId conversation_id() const noexcept override { return conversation_id_; }
    [[nodiscard]] const std::stop_token& stop_token() const noexcept override { return stop_token_; }

    [[nodiscard]] support::AsyncResult<void> commit(CommitCallback change) override {
        const TaskId task_id = task_id_;
        const bool allow_abort_commit = allow_abort_commit_;
        const bool abort_invocation = abort_invocation_;
        const auto now_fn = now_fn_;
        return session_.commit([task_id, allow_abort_commit, abort_invocation, now_fn, change = std::move(change)](
                                       Transaction& tx) mutable -> support::ExpectedVoid {
            auto cur_opt = tx.task(task_id);
            if (!cur_opt || !cur_opt->has_value()) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found"));
            }
            TaskRecord current = *cur_opt.value();
            if (current.state.status != "running") {
                return std::unexpected(support::make_error(support::ErrorCode::Validation,
                        "Task " + std::to_string(task_id.value) + " is " + current.state.status));
            }
            if (current.abort_requested && !allow_abort_commit) {
                return std::unexpected(
                        support::make_error(support::ErrorCode::Validation, "Task has a durable abort mark"));
            }
            auto next_res = change(tx, current);
            if (!next_res) {
                return std::unexpected(next_res.error());
            }
            if (next_res->has_value()) {
                auto next = std::move(*next_res.value());
                if (next.status == "terminal") {
                    // A terminal outcome holds as `completing` while ordinary
                    // owned work below the task is live, judged on this
                    // commit's candidates so work it creates below the task
                    // counts too (spec §5.5). The scheduler writes the terminal
                    // record once that work drains.
                    current.state.status = has_owned_live_in(tx, current) ? "completing" : "terminal";
                    current.state.outcome = std::move(next.outcome);
                    current.state.on.clear();
                    current.state.policy = std::nullopt;
                    // `endedAt` marks the change to `terminal`; a held
                    // `completing` outcome has not ended yet.
                    if (current.state.status == "terminal") {
                        current.ended_at = current_time(now_fn);
                    }
                    current.memos.clear();
                } else if (next.status == "waiting") {
                    if (!next.policy.has_value()) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Validation, "A waiting task needs a join policy"));
                    }
                    if (abort_invocation) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Abort handler of task " + std::to_string(task_id.value) + " cannot wait"));
                    }
                    if (auto valid = validate_wait(tx, current, next.on, *next.policy); !valid) {
                        return std::unexpected(std::move(valid.error()));
                    }
                    current.state.status = "waiting";
                    current.state.checkpoint = std::move(next.checkpoint);
                    current.state.on = std::move(next.on);
                    current.state.policy = *next.policy;
                    current.state.outcome = std::nullopt;
                } else if (next.status == "running") {
                    current.state.status = "running";
                    current.state.checkpoint = std::move(next.checkpoint);
                }
                auto set_res = tx.set_task(current);
                if (!set_res) return set_res;
            }
            return {};
        });
    }

    /// Whether ordinary owned work below `current` is live, judged on this
    /// transaction's candidates (frozen pi `#commitState` ->
    /// `#hasOwnedLive(id, overlay)`), so work the same commit creates below the
    /// task counts.
    [[nodiscard]] static bool has_owned_live_in(Transaction& tx, const TaskRecord& current) {
        auto candidates = tx.overlay_tasks();
        if (!candidates) return false;
        const OwnershipIndex index(std::move(*candidates), {});
        return !index.owned_live(current.id).empty();
    }

    /// Validate a wait before it is stored (frozen pi `#validateWait`): every
    /// member exists, `failFast` may only name tasks the waiter owns, and the
    /// wait may not close a cycle in the wait-for graph. An abort handler
    /// cannot wait.
    [[nodiscard]] static support::ExpectedVoid validate_wait(
            Transaction& tx, const TaskRecord& current, const std::vector<TaskId>& on, JoinPolicy policy) {
        auto candidates = tx.overlay_tasks();
        if (!candidates) return std::unexpected(candidates.error());
        std::map<std::int64_t, TaskRecord> members;
        for (TaskRecord& record : *candidates)
            members.insert_or_assign(record.id.value, std::move(record));
        const auto find = [&members](TaskId id) -> const TaskRecord* {
            const auto found = members.find(id.value);
            return found == members.end() ? nullptr : &found->second;
        };
        for (const TaskId member_id : on) {
            const TaskRecord* member = find(member_id);
            if (member == nullptr) {
                return std::unexpected(support::make_error(
                        support::ErrorCode::Validation, "Task " + std::to_string(member_id.value) + " does not exist"));
            }
            if (policy == JoinPolicy::FailFast && member->owner != std::optional<TaskId>{current.id}) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation,
                        "Task " + std::to_string(current.id.value) + " can wait failFast only on tasks it owns; " +
                                std::to_string(member_id.value) + " is not one"));
            }
            // A live child already holds the waiter (task-owned tasks are never
            // background), so waiting on it adds no edge; a terminal task has
            // no edges at all.
            if (member->state.status == "terminal" || member->owner == std::optional<TaskId>{current.id}) continue;
            // Any other member's live work chain, and its own wait edges, must
            // not lead back to the waiter (frozen pi `#waitCycle`).
            std::vector<TaskId> pending{member_id};
            std::set<std::int64_t> visited{current.id.value};
            while (!pending.empty()) {
                const TaskId at_id = pending.back();
                pending.pop_back();
                const TaskRecord* at = find(at_id);
                if (at == nullptr) continue;
                if (!visited.insert(at->id.value).second) continue;
                if (at->owner.has_value()) {
                    if (*at->owner == current.id) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Task " + std::to_string(current.id.value) + " cannot wait on " +
                                        std::to_string(member_id.value) + ": " + std::to_string(current.id.value) +
                                        " -> " + std::to_string(member_id.value) + " is a wait cycle"));
                    }
                    pending.push_back(*at->owner);
                }
                if (at->state.status == "waiting" && !at->abort_requested) {
                    for (const TaskId next : at->state.on)
                        pending.push_back(next);
                }
            }
        }
        return {};
    }

    [[nodiscard]] support::AsyncResult<chord::Json> memo(std::string key, chord::Json candidate) override {
        auto memo_val = std::make_shared<chord::Json>();
        const TaskId task_id = task_id_;
        const bool allow_abort_commit = allow_abort_commit_;
        auto commit_res = session_.commit(
                [task_id, allow_abort_commit, key = std::move(key), candidate = std::move(candidate), memo_val](
                        Transaction& tx) mutable -> support::ExpectedVoid {
                    auto cur_opt = tx.task(task_id);
                    if (!cur_opt || !cur_opt->has_value()) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found"));
                    }
                    TaskRecord current = *cur_opt.value();
                    if (current.state.status != "running") {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                                "Task " + std::to_string(task_id.value) + " is " + current.state.status));
                    }
                    if (current.abort_requested && !allow_abort_commit) {
                        return std::unexpected(
                                support::make_error(support::ErrorCode::Validation, "Task has a durable abort mark"));
                    }
                    auto it = current.memos.find(key);
                    if (it != current.memos.end()) {
                        *memo_val = it->second;
                        return {};
                    }
                    current.memos.emplace(key, candidate);
                    *memo_val = std::move(candidate);
                    return tx.set_task(current);
                });

        return support::AsyncResult<chord::Json>{support::AsyncProducer<chord::Json, support::Error>{
                [commit_res = std::move(commit_res), memo_val](
                        support::AsyncCompletion<chord::Json, support::Error> completion) mutable noexcept {
                    std::move(commit_res)
                            .start([completion = std::move(completion), memo_val](
                                           support::ExpectedVoid outcome) mutable noexcept {
                                if (!outcome) {
                                    completion(std::unexpected(outcome.error()));
                                } else {
                                    completion(std::move(*memo_val));
                                }
                            });
                }}};
    }

    void report(const support::Error& error) override {
        if (report_fn_) {
            report_fn_(error);
        }
    }

    [[nodiscard]] support::AsyncResult<void> each_hook(std::string_view name, HookInvokeHook invoke) override {
        if (!task_def_.has_hook(name)) {
            return support::AsyncResult<void>{std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "Unknown hook: " + std::string{name}))};
        }
        if (!invoke) {
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        }
        const std::string hook_name{name};
        for (const auto& hook_reg : phase_hooks_) {
            if (stop_token_.stop_requested()) {
                return support::AsyncResult<void>{std::unexpected(
                        support::make_error(support::ErrorCode::Cancelled, "Task invocation was aborted"))};
            }
            if (!hook_reg || hook_reg->task != task_def_.kind()) {
                continue;
            }
            auto it = hook_reg->handlers.find(hook_name);
            if (it == hook_reg->handlers.end() || !it->second) {
                continue;
            }
            auto res = await_async_void(invoke(it->second));
            if (!res) {
                if (stop_token_.stop_requested()) {
                    return support::AsyncResult<void>{std::unexpected(res.error())};
                }
                report(res.error());
            } else if (stop_token_.stop_requested()) {
                return support::AsyncResult<void>{std::unexpected(
                        support::make_error(support::ErrorCode::Cancelled, "Task invocation was aborted"))};
            }
        }
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }

    [[nodiscard]] support::AsyncResult<void> each_hook(std::string_view name, chord::Json input) override {
        return each_hook(name, [this, input = std::move(input)](TaskHook& handler) -> support::AsyncResult<void> {
            auto res = await_async_json(handler(input, *this));
            if (!res) {
                return support::AsyncResult<void>{std::unexpected(res.error())};
            }
            return support::AsyncResult<void>{support::ExpectedVoid{}};
        });
    }

private:
    Session& session_; // must outlive every pending operation returned by this runtime
    TaskId task_id_;
    ConversationId conversation_id_;
    const TaskDefinition& task_def_; // must outlive every pending operation returned by this runtime
    std::vector<std::shared_ptr<TaskHookRegistration>> phase_hooks_;
    std::stop_token stop_token_;
    std::shared_ptr<std::move_only_function<std::int64_t()>> now_fn_;
    std::move_only_function<void(const support::Error&)> report_fn_;
    bool allow_abort_commit_{false};
    /// Whether this runtime belongs to an abort handler, which cannot wait.
    bool abort_invocation_{false};
};

} // namespace

struct Harness::Impl {
    Storage* storage{nullptr}; // must outlive this Harness and all of its operations
    std::unique_ptr<Session> session;
    HarnessOptions options;
    Registry default_registry;
    Registry& registry; // must outlive this Harness and all of its operations
    std::uint64_t registry_sub_id{0};
    std::shared_ptr<std::move_only_function<std::int64_t()>> clock;
    std::unique_ptr<WorkflowRuntime> workflow;
    std::unique_ptr<CompactionRuntime> compaction;
    std::shared_ptr<TaskDefinition> builtin_generation_task;
    std::unique_ptr<Submission::Registry> submissions;

    struct ExecutionThread {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };

    struct FailedMigration {
        std::shared_ptr<const TaskDefinition> definition{};
        support::Error error{};
    };

    struct TaskFit {
        std::shared_ptr<const TaskDefinition> definition{};
        bool migrates{false};
        std::optional<std::string> blocked_reason{std::nullopt};
        std::optional<support::Error> error{std::nullopt};
    };

    struct TaskResolution {
        TaskFit fit{};
        TaskRecord record{};
        bool migrated{false};
    };

    mutable std::mutex mutex;
    mutable std::mutex report_mutex;
    std::condition_variable cv;
    std::jthread worker;
    std::vector<ExecutionThread> execution_threads;
    bool enabled{false};
    bool dirty{false};
    std::atomic_bool closed{false};

    std::map<std::int64_t, std::stop_source> active_invocations;
    std::map<std::int64_t, FailedMigration> failed_migrations;
    std::map<std::int64_t, std::vector<std::shared_ptr<TaskWaiter>>> waiters;
    /// Idle waiters, keyed by conversation; `std::nullopt` waits for every
    /// ownerless scope (frozen pi `TaskScheduler.#idleWaiters`).
    std::map<std::optional<std::int64_t>, std::vector<std::shared_ptr<IdleWaiter>>> idle_waiters;
    std::optional<support::Error> wait_error;

    /// One read-only view of the ownership world the scheduler reasons over:
    /// every task and conversation record at one point in time.
    struct World {
        OwnershipIndex index;
        std::vector<TaskRecord> tasks;

        World(std::vector<TaskRecord> task_records, std::vector<ConversationRecord> conversation_records)
            : index(task_records, conversation_records), tasks(std::move(task_records)) {}
    };

    [[nodiscard]] support::Expected<World> load_world() const {
        auto tasks = storage->scan_tasks(1024);
        if (!tasks) return std::unexpected(tasks.error());
        auto conversations = storage->scan_conversations(1024);
        if (!conversations) return std::unexpected(conversations.error());
        return World{std::move(*tasks), std::move(*conversations)};
    }

    Impl(Storage* s, std::unique_ptr<Session> sess, HarnessOptions opt)
        : storage(s), session(std::move(sess)), options(std::move(opt)),
          registry(options.registry ? *options.registry : default_registry),
          clock(std::make_shared<std::move_only_function<std::int64_t()>>(std::move(options.now))) {
        if (!*clock) {
            *clock = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
            };
        }
    }

    ~Impl() { shutdown(); }

    void report(const support::Error& error) {
        std::lock_guard lock(report_mutex);
        if (options.on_report) {
            options.on_report(error);
        }
    }

    [[nodiscard]] std::shared_ptr<const TaskDefinition> find_definition(
            std::string_view kind, const Registry::Snapshot& snapshot) const {
        if (compaction && compaction->definition() && compaction->definition()->kind() == kind) {
            return compaction->definition();
        }
        if (builtin_generation_task && builtin_generation_task->kind() == kind) {
            return builtin_generation_task;
        }
        for (const auto& def : snapshot.tasks) {
            if (def && def->kind() == kind) {
                return def;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const TaskDefinition* find_definition(std::string_view kind) const {
        const auto snapshot = registry.snapshot();
        return find_definition(kind, snapshot).get();
    }

    [[nodiscard]] TaskFit fit_task(const TaskRecord& record, const Registry::Snapshot& snapshot) const {
        auto def = find_definition(record.kind, snapshot);
        if (!def) {
            return TaskFit{.blocked_reason = "missing_task"};
        }
        if (def->version() == record.version) {
            return TaskFit{.definition = std::move(def), .migrates = false};
        }
        if (def->version() < record.version) {
            return TaskFit{.blocked_reason = "task_too_old"};
        }
        {
            std::lock_guard lock(mutex);
            auto it = failed_migrations.find(record.id.value);
            if (it != failed_migrations.end() && it->second.definition.get() == def.get()) {
                return TaskFit{.blocked_reason = "migration_failed", .error = it->second.error};
            }
        }
        return TaskFit{.definition = std::move(def), .migrates = true};
    }

    [[nodiscard]] TaskInspectionState inspect_task(
            const TaskRecord& record, const Registry::Snapshot& snapshot, const std::set<std::int64_t>& invoked) const {
        // The identity view is one copy taken at the capture instant (issue
        // #1165): no per-row re-read can pair rows from different instants.
        // A committed `running` record reports running even after its identity
        // is erased: a phase that ended abnormally (or a crash-recovered
        // record) must not be reported ready.
        if (record.state.status == "running") {
            return TaskInspectionState{.kind = "running"};
        }
        // A committed `pending` record is never promoted by identity alone.
        // The definition-replacement path writes `running -> pending` and
        // erases the identity only afterwards; in that window the pair
        // (pending record, live identity) is coherent state, not a run.
        if (record.state.status != "pending" && invoked.contains(record.id.value)) {
            return TaskInspectionState{.kind = "running"};
        }
        if (record.state.status == "completing") {
            return TaskInspectionState{.kind = "completing"};
        }
        // A marked task runs its abort handler once its ordinary owned work is
        // gone, so it reports the work it waits for, like a waiting task
        // (frozen pi `#inspectTask` -> `#waitingOn`).
        auto world = load_world();
        if (world) {
            const std::vector<TaskId> on =
                    record.abort_requested
                            ? world->index.owned_live(record.id)
                            : (record.state.status == "waiting" ? record.state.on : std::vector<TaskId>{});
            std::vector<TaskId> live;
            for (const TaskId id : on) {
                const TaskRecord* member = world->index.task(id);
                if (member != nullptr && task_live(*member)) live.push_back(id);
            }
            if (!live.empty()) {
                return TaskInspectionState{.kind = "waiting", .on = std::move(live)};
            }
        }
        auto fit = fit_task(record, snapshot);
        if (fit.blocked_reason.has_value()) {
            return TaskInspectionState{.kind = "blocked", .reason = fit.blocked_reason, .error = fit.error};
        }
        if (fit.migrates && !fit.definition->has_migrate()) {
            const auto err = support::make_error(support::ErrorCode::Validation,
                    "Task " + record.kind + " version " + std::to_string(fit.definition->version()) +
                            " has no migration from " + std::to_string(record.version));
            return TaskInspectionState{.kind = "blocked", .reason = "migration_failed", .error = err};
        }
        return TaskInspectionState{.kind = "ready", .migrates = fit.migrates};
    }

    /// One-instant inspection capture for the line-read seam (issue #1165):
    /// the storage scan, the scheduling flags, and one identity copy happen
    /// inside a single line job, so every row derives from one committed
    /// state. Runs while the mutation line is held; the Harness mutex is
    /// taken only for the identity copy and released before the scan, the
    /// same order the commit and close listeners already use.
    ///
    /// The capture is bounded: `scan_tasks(256)` here and `load_world()`'s
    /// 1024-record scan for waiting/abort rows. A larger repository is
    /// inspected partially by contract; a complete committed scan beyond the
    /// cap belongs to issue #1088 (see `Harness::inspect`).
    [[nodiscard]] support::Expected<HarnessInspection> capture_inspection(Storage& storage) const {
        auto scan_res = storage.scan_tasks(256);
        if (!scan_res) {
            return std::unexpected(scan_res.error());
        }
        const auto snapshot = registry.snapshot();
        HarnessInspection inspection;
        std::set<std::int64_t> invoked;
        {
            std::lock_guard lock(mutex);
            inspection.scheduling = closed ? "closing" : (enabled ? "running" : "paused");
            for (const auto& [id, _] : active_invocations) {
                invoked.insert(id);
            }
        }
        for (const auto& task : *scan_res) {
            // Terminal records never derive an invocation state: a live or
            // stale identity must not report a finished task as running.
            if (task.state.status == "terminal") {
                continue;
            }
            inspection.tasks.push_back(TaskInspection{
                    .record = task,
                    .state = inspect_task(task, snapshot, invoked),
            });
        }
        return inspection;
    }

    [[nodiscard]] TaskResolution resolve_task(const TaskRecord& record, const Registry::Snapshot& snapshot) {
        auto fit = fit_task(record, snapshot);
        if (fit.blocked_reason.has_value()) {
            return TaskResolution{.fit = std::move(fit)};
        }
        if (!fit.migrates) {
            return TaskResolution{.fit = std::move(fit), .record = record, .migrated = false};
        }
        const chord::Json cp = record.state.checkpoint.value_or(chord::Json{nullptr});
        auto mig_res = fit.definition->migrate(record.input, cp, record.version);
        if (!mig_res) {
            {
                std::lock_guard lock(mutex);
                failed_migrations[record.id.value] =
                        FailedMigration{.definition = fit.definition, .error = mig_res.error()};
            }
            report(mig_res.error());
            return TaskResolution{
                    .fit = TaskFit{.blocked_reason = "migration_failed", .error = mig_res.error()},
            };
        }
        {
            std::lock_guard lock(mutex);
            failed_migrations.erase(record.id.value);
        }
        TaskRecord migrated = record;
        migrated.version = fit.definition->version();
        migrated.input = std::move(mig_res->input);
        migrated.state.checkpoint = std::move(mig_res->checkpoint);
        return TaskResolution{
                .fit = std::move(fit),
                .record = std::move(migrated),
                .migrated = true,
        };
    }

    void settle_orphaned(TaskId tid, std::string reason) {
        (void)await_async_void(session->commit(
                [this, tid, reason = std::move(reason)](Transaction& tx) mutable -> support::ExpectedVoid {
                    auto cur = tx.task(tid);
                    if (cur && cur->has_value()) {
                        auto t = *cur.value();
                        if (t.state.status == "terminal") return {};
                        t.state.status = "terminal";
                        t.state.outcome = TaskOutcome{
                                .status = "orphaned",
                                .reason = std::move(reason),
                        };
                        t.ended_at = current_time(clock);
                        t.memos.clear();
                        return tx.set_task(t);
                    }
                    return {};
                }));
    }

    void settle_faulted(TaskId tid, std::string message) {
        (void)await_async_void(session->commit(
                [this, tid, message = std::move(message)](Transaction& tx) mutable -> support::ExpectedVoid {
                    auto cur = tx.task(tid);
                    if (cur && cur->has_value()) {
                        auto t = *cur.value();
                        if (t.state.status == "terminal") return {};
                        t.state.status = "terminal";
                        t.state.outcome = TaskOutcome{
                                .status = "faulted",
                                .error = TaskOutcomeError{.message = std::move(message)},
                        };
                        t.ended_at = current_time(clock);
                        t.memos.clear();
                        return tx.set_task(t);
                    }
                    return {};
                }));
    }

    void notify_waiters(const TaskRecord& record) {
        std::vector<std::shared_ptr<TaskWaiter>> comps;
        {
            std::lock_guard lock(mutex);
            failed_migrations.erase(record.id.value);
            auto it = waiters.find(record.id.value);
            if (it != waiters.end()) {
                comps = std::move(it->second);
                waiters.erase(it);
            }
        }
        for (const std::shared_ptr<TaskWaiter>& waiter : comps) {
            if (waiter->outcome.exchange(1) != 0) continue;
            auto completion = std::move(waiter->completion);
            if (completion.has_value()) (*completion)(record);
        }
    }

    /// Resolve every idle waiter whose scope is now idle (frozen pi
    /// `#settleIdle`).
    void settle_idle() {
        auto world = load_world();
        if (!world) return;
        std::vector<std::shared_ptr<IdleWaiter>> resolved;
        {
            std::lock_guard lock(mutex);
            for (auto waiter = idle_waiters.begin(); waiter != idle_waiters.end();) {
                const bool idle = waiter->first.has_value() ? world->index.idle(ConversationId{*waiter->first})
                                                            : world->index.all_idle();
                if (!idle) {
                    ++waiter;
                    continue;
                }
                for (const auto& pending : waiter->second)
                    resolved.push_back(pending);
                waiter = idle_waiters.erase(waiter);
            }
        }
        for (const std::shared_ptr<IdleWaiter>& waiter : resolved) {
            if (waiter->outcome.exchange(1) != 0) continue;
            auto completion = std::move(waiter->completion);
            if (completion.has_value()) (*completion)(support::ExpectedVoid{});
        }
    }

    /// The scheduler's reconciliation pass (frozen pi `#reconcile`): in one
    /// commit, mark the live ordinary work below every owner with cancellation
    /// intent, apply `failFast` join marks, and write the terminal record of
    /// every `completing` task whose ordinary owned work is gone.
    void reconcile() {
        auto scan_res = storage->scan_tasks(1024);
        if (!scan_res) return;
        std::vector<TaskId> cascade;
        std::vector<TaskId> fail_fast;
        {
            auto conversations = storage->scan_conversations(1024);
            if (!conversations) return;
            const OwnershipIndex index(*scan_res, *conversations);
            cascade = index.cascade_targets();
            for (const TaskRecord& task : *scan_res) {
                if (task.state.status != "waiting" || task.state.policy != JoinPolicy::FailFast) continue;
                bool failed = false;
                for (const TaskId member_id : task.state.on) {
                    const TaskRecord* member = index.task(member_id);
                    if (member != nullptr && failed_outcome(*member)) failed = true;
                }
                if (failed) fail_fast.push_back(task.id);
            }
        }
        if (cascade.empty() && fail_fast.empty()) {
            // Only the completing finalize remains; it reads no marks.
            finalize_completing();
            settle_idle();
            return;
        }
        std::set<std::int64_t> to_mark;
        for (const TaskId id : cascade)
            to_mark.insert(id.value);
        for (const TaskId waiter_id : fail_fast) {
            const TaskRecord* waiter = nullptr;
            for (const TaskRecord& task : *scan_res) {
                if (task.id == waiter_id) waiter = &task;
            }
            if (waiter == nullptr) continue;
            for (const TaskId member_id : waiter->state.on) {
                for (const TaskRecord& task : *scan_res) {
                    if (task.id == member_id && task_live(task) && !failed_outcome(task)) {
                        to_mark.insert(task.id.value);
                    }
                }
            }
        }
        if (!to_mark.empty()) {
            (void)await_async_void(
                    session->commit([to_mark = std::move(to_mark)](Transaction& tx) -> support::ExpectedVoid {
                        auto tasks = tx.scan_tasks(1024);
                        if (!tasks) return std::unexpected(tasks.error());
                        for (const TaskRecord& task : *tasks) {
                            if (!to_mark.contains(task.id.value)) continue;
                            if (task.state.status == "terminal" || task.abort_requested) continue;
                            TaskRecord marked = task;
                            marked.abort_requested = true;
                            auto written = tx.set_task(std::move(marked));
                            if (!written) return written;
                        }
                        return {};
                    }));
        }
        finalize_completing();
        settle_idle();
    }

    /// Write the terminal record of every `completing` task without live
    /// ordinary owned work (frozen pi `#finalize`). Finalizing one frees the
    /// holder above it, so this repeats over the commit's candidates until
    /// nothing changes.
    void finalize_completing() {
        for (int round = 0; round < 64; ++round) {
            auto world = load_world();
            if (!world) return;
            std::vector<TaskId> ready;
            for (const TaskRecord& task : world->tasks) {
                if (task.state.status != "completing") continue;
                if (!world->index.owned_live(task.id).empty()) continue;
                ready.push_back(task.id);
            }
            if (ready.empty()) return;
            const auto written =
                    await_async_void(session->commit([ready, this](Transaction& tx) -> support::ExpectedVoid {
                        auto tasks = tx.scan_tasks(1024);
                        if (!tasks) return std::unexpected(tasks.error());
                        for (const TaskRecord& task : *tasks) {
                            bool wanted = false;
                            for (const TaskId id : ready) {
                                if (task.id == id) wanted = true;
                            }
                            if (!wanted || task.state.status != "completing") continue;
                            TaskRecord terminal = task;
                            terminal.state.status = "terminal";
                            if (!terminal.ended_at.has_value()) {
                                terminal.ended_at = current_time(clock);
                            }
                            auto written = tx.set_task(std::move(terminal));
                            if (!written) return written;
                        }
                        return {};
                    }));
            if (!written) return;
        }
    }

    /// Start one manual compaction of a conversation (frozen pi
    /// `Harness.compact`): conversation-owned and non-background, so a
    /// blocking state never waits for it.
    [[nodiscard]] support::Expected<TaskId> compact(
            ConversationId conversation_id, const std::optional<std::string>& instructions) {
        if (compaction == nullptr) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation, "No compaction runtime"));
        }
        chord::Json::Object input{{"reason", std::string{"manual"}}};
        if (instructions.has_value()) input.emplace("instructions", *instructions);
        auto created = std::make_shared<support::Expected<TaskId>>(
                std::unexpected(support::make_error(support::ErrorCode::Validation, "Compaction not committed")));
        const auto committed = await_async_void(
                session->commit([this, conversation_id, input = chord::Json{std::move(input)}, created](
                                        Transaction& tx) -> support::ExpectedVoid {
                    auto task = create_compaction_task(
                            tx, *compaction->definition(), conversation_id, input, std::nullopt, false);
                    if (!task) return std::unexpected(task.error());
                    *created = *task;
                    return {};
                }));
        if (!committed) return std::unexpected(committed.error());
        return *created;
    }

    support::ExpectedVoid init() {
        if (!options.tasks.empty()) {
            Extension initial_ext{.name = ".harness.tasks", .tasks = std::move(options.tasks)};
            if (auto res = registry.install(std::move(initial_ext)); !res) {
                return res;
            }
        }
        workflow = std::make_unique<WorkflowRuntime>(*session, *storage, options, clock);
        compaction = std::make_unique<CompactionRuntime>(*session, *storage, clock, *workflow);
        workflow->install_compaction_definition(compaction->definition());
        builtin_generation_task = workflow->generation();
        submissions = std::make_unique<Submission::Registry>(*session, *storage);
        registry_sub_id = registry.subscribe([this] {
            std::lock_guard lock(mutex);
            dirty = true;
            cv.notify_all();
        });
        (void)session->subscribe_close([this] {
            std::map<std::int64_t, std::vector<std::shared_ptr<TaskWaiter>>> pending;
            std::map<std::optional<std::int64_t>, std::vector<std::shared_ptr<IdleWaiter>>> pending_idle;
            const auto error = session->failed()
                                       ? session_failed_error()
                                       : support::make_error(support::ErrorCode::Validation, "Session is closed");
            // A storage failure closes the Session without running the
            // Harness shutdown path, so the active invocations are signalled
            // here — before the failing commit's caller observes the
            // rejection (frozen pi fail -> close -> beforeClose ->
            // TaskScheduler.join()). Copy the sources out of the scheduler
            // mutex first: stop callbacks run after it is released (issue
            // #1165), and a callback that waits on the Session line is the
            // documented reentry boundary.
            std::vector<std::stop_source> sources;
            {
                std::lock_guard lock(mutex);
                wait_error = error;
                for (const auto& [_, stop_src] : active_invocations)
                    sources.push_back(stop_src);
                pending = std::move(waiters);
                waiters.clear();
                pending_idle = std::move(idle_waiters);
                idle_waiters.clear();
            }
            for (auto& source : sources)
                source.request_stop();
            submissions->reject_all(error);
            for (auto& [id, completions] : pending) {
                (void)id;
                for (const std::shared_ptr<TaskWaiter>& waiter : completions) {
                    if (waiter->outcome.exchange(1) != 0) continue;
                    auto completion = std::move(waiter->completion);
                    if (completion.has_value()) (*completion)(std::unexpected(error));
                }
            }
            for (auto& [scope, completions] : pending_idle) {
                (void)scope;
                for (const std::shared_ptr<IdleWaiter>& waiter : completions) {
                    if (waiter->outcome.exchange(1) != 0) continue;
                    auto completion = std::move(waiter->completion);
                    if (completion.has_value()) (*completion)(std::unexpected(error));
                }
            }
        });
        (void)session->subscribe_commits([this](const CommitPublication& pub) {
            submissions->observe(pub);
            for (const auto& change : pub.changes) {
                if (const auto* tc = std::get_if<TableCommitChange>(&change)) {
                    if (tc->type == "task" && tc->task.has_value()) {
                        if (tc->task->state.status == "terminal") {
                            notify_waiters(*tc->task);
                        } else if (tc->task->abort_requested) {
                            // A new abort mark ends a held run invocation; its
                            // abort handler runs once the owned work below it
                            // drains (frozen pi commit listener ->
                            // `#invocations.get(id).controller.abort()`).
                            std::optional<std::stop_source> source;
                            {
                                std::lock_guard lock(mutex);
                                auto invocation = active_invocations.find(tc->task->id.value);
                                if (invocation != active_invocations.end()) source = invocation->second;
                            }
                            if (source) source->request_stop();
                        }
                    }
                }
            }
            std::lock_guard lock(mutex);
            dirty = true;
            cv.notify_all();
        });

        worker = std::jthread([this](std::stop_token stop) { run_worker(stop); });
        return {};
    }

    void shutdown() {
        registry.unsubscribe(registry_sub_id);
        registry_sub_id = 0;
        std::vector<std::stop_source> sources;
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            closed = true;
            for (const auto& [_, stop_src] : active_invocations)
                sources.push_back(stop_src);
        }
        for (auto& source : sources)
            source.request_stop();
        if (worker.joinable()) {
            worker.request_stop();
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return active_invocations.empty(); });
            for (auto& task_thread : execution_threads) {
                if (task_thread.thread.joinable()) {
                    task_thread.thread.join();
                }
            }
            execution_threads.clear();
        }
    }

    void run_worker(std::stop_token stop) {
        while (!stop.stop_requested()) {
            bool running = false;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop.stop_requested() || closed || dirty; });
                if (stop.stop_requested() || closed) {
                    break;
                }
                dirty = false;
                running = enabled;
            }

            // Derived state (abort cascades, `failFast` marks, held outcomes,
            // idle waits) follows the durable records whether or not new work
            // may start, so a paused Harness still cascades and finalizes.
            if (running) {
                schedule_step();
            } else {
                reconcile();
                settle_idle();
            }

            std::unique_lock wait_lock(mutex);
            cv.wait_for(
                    wait_lock, std::chrono::milliseconds(10), [&] { return stop.stop_requested() || closed || dirty; });
        }
    }

    void schedule_step() {
        for (auto thread = execution_threads.begin(); thread != execution_threads.end();) {
            if (thread->finished->load(std::memory_order_acquire)) {
                if (thread->thread.joinable()) {
                    thread->thread.join();
                }
                thread = execution_threads.erase(thread);
            } else {
                ++thread;
            }
        }

        auto scan_res = storage->scan_tasks(256);
        if (!scan_res) return;

        auto conversations = storage->scan_conversations(256);
        if (!conversations) return;
        const OwnershipIndex index(*scan_res, *conversations);

        // Reconcile first: cascade marks, `failFast` marks, and the terminal
        // record of every drained `completing` task. The durable records are
        // the intent, so this also repairs whatever a reopen left unapplied.
        reconcile();
        auto refreshed = storage->scan_tasks(256);
        if (!refreshed) return;
        auto refreshed_conversations = storage->scan_conversations(256);
        if (!refreshed_conversations) return;
        const OwnershipIndex world(*refreshed, *refreshed_conversations);

        std::optional<Registry::Snapshot> snapshot_opt;

        for (const auto& task : *refreshed) {
            if (task.state.status == "terminal") {
                notify_waiters(task);
                continue;
            }

            // A live owner's cancellation intent marks the work below it before
            // that work runs a phase; a task the cascade already marked runs its
            // abort handler instead.
            if (!task.abort_requested && !task.background && task.owner.has_value()) {
                const TaskRecord* owner = world.task(*task.owner);
                if (owner != nullptr && cancellation_intent(*owner)) continue;
            }

            // A waiting task resumes once every member is terminal; a `failFast`
            // wait whose member failed is handled by `reconcile()`.
            if (task.state.status == "waiting") {
                {
                    // A phase that committed `waiting` may still be active;
                    // resuming it to `pending` under a live identity would let
                    // a second invocation start (issue #1165). Cleanup dirties
                    // the scheduler, so the resume happens on a later pass.
                    std::lock_guard lock(mutex);
                    if (closed || active_invocations.contains(task.id.value)) continue;
                }
                bool all_terminal = true;
                for (const TaskId member_id : task.state.on) {
                    const TaskRecord* member = world.task(member_id);
                    if (member == nullptr || task_live(*member)) all_terminal = false;
                }
                if (!all_terminal) continue;
                (void)await_async_void(session->commit([id = task.id](Transaction& tx) -> support::ExpectedVoid {
                    auto cur = tx.task(id);
                    if (!cur || !cur->has_value()) return {};
                    TaskRecord resumed = **cur;
                    if (resumed.state.status != "waiting") return {};
                    resumed.state.status = "pending";
                    resumed.state.on.clear();
                    resumed.state.policy = std::nullopt;
                    return tx.set_task(std::move(resumed));
                }));
                continue;
            }

            // A `completing` task's terminal record is written by
            // `finalize_completing()` once its ordinary owned work is gone.
            if (task.state.status == "completing") continue;

            // A marked task runs its abort handler only after its ordinary
            // owned work has drained, so descendants' handlers run first
            // (frozen pi `#runnable` -> `#waitingOn` returns the owned live
            // work while the record carries the abort mark).
            if (task.abort_requested && !world.owned_live(task.id).empty()) continue;

            // A marked task of any live status is reserved for its abort
            // invocation once the owned work below it is gone.
            if (task.state.status != "pending" && !task.abort_requested) continue;

            if (task.state.status == "pending" || task.abort_requested) {
                {
                    std::lock_guard lock(mutex);
                    if (closed || active_invocations.contains(task.id.value)) {
                        continue;
                    }
                }

                if (!snapshot_opt.has_value()) {
                    snapshot_opt.emplace(registry.snapshot());
                }
                auto resolution = resolve_task(task, *snapshot_opt);
                if (resolution.fit.blocked_reason.has_value()) {
                    if (task.abort_requested && !task.abort_reason.has_value()) {
                        settle_orphaned(task.id, *resolution.fit.blocked_reason);
                    }
                    continue;
                }

                std::stop_source stop_src;
                {
                    std::lock_guard lock(mutex);
                    if (closed || active_invocations.contains(task.id.value)) {
                        continue;
                    }
                }

                auto tid = task.id;
                TaskRecord resolved_record = std::move(resolution.record);
                // A stale scheduling snapshot can reach this reservation after
                // the task's abort invocation already committed terminal. The
                // reservation must not rewrite the terminal record: Storage
                // rejects that write and permanently fails the Session.
                // The flag is stored by the transaction body (on the line) and
                // loaded by this worker after the commit settles; the
                // release/acquire pair makes the staged-write observation
                // ordered before the publication decision. No other thread
                // touches it.
                auto reserved = std::make_shared<std::atomic_bool>(false);
                auto trans_res = await_async_void(
                        session->commit([this, tid, resolved_record = std::move(resolved_record), reserved](
                                                Transaction& tx) -> support::ExpectedVoid {
                            auto cur_opt = tx.task(tid);
                            if (!cur_opt || !cur_opt->has_value()) return {};
                            auto cur = *cur_opt.value();
                            if (cur.state.status == "terminal") return {};
                            // A marked task may be reserved from any live
                            // status for its abort invocation; the abort
                            // commit then replaces the whole state.
                            if (cur.state.status != "pending" && !cur.abort_requested) return {};
                            cur.version = resolved_record.version;
                            cur.input = resolved_record.input;
                            cur.state.checkpoint = resolved_record.state.checkpoint;
                            cur.state.status = "running";
                            if (!cur.started_at.has_value()) {
                                cur.started_at = current_time(clock);
                            }
                            auto written = tx.set_task(cur);
                            if (written) reserved->store(true, std::memory_order_release);
                            return written;
                        }));

                if (!trans_res) {
                    // The reservation commit failed: surface the original
                    // Storage error to the report sink (issue #1166: the
                    // failing caller retains the original error) and publish
                    // nothing. The Session also seals with the frozen message.
                    report(trans_res.error());
                    continue;
                }
                if (!reserved->load(std::memory_order_acquire)) {
                    // The reservation body no-oped (the record was no longer
                    // reservable when the commit ran): publish nothing.
                    continue;
                }

                {
                    std::lock_guard l(mutex);
                    if (closed) {
                        // The reservation settled but the Harness closed before
                        // publication: discard it and never start the phase
                        // (frozen pi: a held reservation at close).
                        cv.notify_all();
                        continue;
                    }
                    // Publish the invocation identity only now, after Storage
                    // settled the reservation commit: an inspection can never
                    // pair a `pending` record with a live identity, and the
                    // phase starts only from a published identity (#1165).
                    active_invocations[tid.value] = stop_src;
                    {
                        auto finished = std::make_shared<std::atomic_bool>(false);
                        ExecutionThread task_thread;
                        task_thread.finished = finished;
                        task_thread.thread = std::thread([this,
                                                                 tid,
                                                                 def = std::move(resolution.fit.definition),
                                                                 phase_snapshot = *snapshot_opt,
                                                                 stop_src,
                                                                 finished]() mutable {
                            run_task_execution(tid, std::move(def), std::move(phase_snapshot), stop_src);
                            finished->store(true, std::memory_order_release);
                        });
                        execution_threads.push_back(std::move(task_thread));
                    }
                }
            }
        }
    }

    void run_task_execution(TaskId tid,
            std::shared_ptr<const TaskDefinition> def,
            Registry::Snapshot phase_snapshot,
            std::stop_source stop_src) {
        auto cleanup = [&]() {
            std::lock_guard lock(mutex);
            active_invocations.erase(tid.value);
            dirty = true;
            cv.notify_all();
        };

        {
            // A close can land between publication and the first phase
            // instruction. The execution thread must erase its published
            // identity, wake the shutdown wait, and return without starting a
            // phase (frozen pi fail -> close -> beforeClose; the empty-table
            // shutdown wait depends on this second close path).
            std::lock_guard lock(mutex);
            if (closed) {
                active_invocations.erase(tid.value);
                dirty = true;
                cv.notify_all();
                return;
            }
        }

        bool reported_replacement = false;
        std::shared_ptr<const TaskDefinition> reported_def;

        // The abort invocation: the abort handler commits the terminal
        // outcome with the abort-marked commit allowed (frozen pi task
        // `abort`).
        auto run_abort_handler = [&](const TaskRecord& task) {
            HarnessTaskRuntime runtime(
                    *session,
                    tid,
                    task.conversation_id,
                    *def,
                    phase_snapshot.hooks,
                    stop_src.get_token(),
                    clock,
                    [this](const support::Error& err) { report(err); },
                    true);
            auto abort_result = await_async_void(def->run_abort(task, runtime));
            if (closed) {
                return;
            }
            auto after_opt = storage->task(tid);
            if (after_opt && after_opt->has_value()) {
                TaskRecord after = *after_opt.value();
                if (after.state.status != "terminal") {
                    const std::string message = abort_result ? "Abort handler of task " + std::to_string(tid.value) +
                                                                       " returned without a terminal outcome"
                                                             : abort_result.error().message;
                    settle_faulted(tid, message);
                }
            }
        };

        while (!closed) {
            auto task_opt = storage->task(tid);
            if (!task_opt || !task_opt->has_value()) {
                cleanup();
                return;
            }
            TaskRecord task = *task_opt.value();
            if (task.state.status == "terminal") {
                notify_waiters(task);
                cleanup();
                return;
            }

            if (task.abort_requested) {
                // The abort handler runs only once the ordinary owned work
                // below the task has drained, so descendants' handlers run
                // first (frozen pi `#runnable` -> `#waitingOn`). Until then
                // this invocation ends and the scheduler reserves the task for
                // its abort invocation once the owned work is gone.
                auto world = load_world();
                if (world && !world->index.owned_live(task.id).empty()) {
                    cleanup();
                    return;
                }
                run_abort_handler(task);
                cleanup();
                return;
            }

            std::string phase_name = "run";
            if (task.state.checkpoint.has_value()) {
                if (const auto* obj = task.state.checkpoint->as_object()) {
                    auto it = obj->find("phase");
                    if (it != obj->end() && it->second.as_string()) {
                        phase_name = *it->second.as_string();
                    }
                }
            }

            std::optional<chord::Json> cp_before = task.state.checkpoint;
            HarnessTaskRuntime runtime(*session,
                    tid,
                    task.conversation_id,
                    *def,
                    phase_snapshot.hooks,
                    stop_src.get_token(),
                    clock,
                    [this](const support::Error& err) { report(err); });

            auto phase_res = await_async_void(def->run_phase(phase_name, task, runtime));

            if (closed) {
                cleanup();
                return;
            }

            auto after_opt = storage->task(tid);
            if (!after_opt || !after_opt->has_value()) {
                cleanup();
                return;
            }
            TaskRecord after = *after_opt.value();
            if (after.state.status == "terminal") {
                notify_waiters(after);
                cleanup();
                return;
            }

            if (after.abort_requested) {
                auto abort_snapshot = registry.snapshot();
                auto resolution = resolve_task(after, abort_snapshot);
                if (resolution.fit.blocked_reason.has_value()) {
                    settle_orphaned(tid, *resolution.fit.blocked_reason);
                    cleanup();
                    return;
                }
                if (resolution.migrated) {
                    TaskRecord migrated = std::move(resolution.record);
                    (void)await_async_void(session->commit(
                            [tid, migrated = std::move(migrated)](Transaction& tx) -> support::ExpectedVoid {
                                auto cur = tx.task(tid);
                                if (!cur || !cur->has_value()) return {};
                                auto t = *cur.value();
                                t.version = migrated.version;
                                t.input = migrated.input;
                                t.state.checkpoint = migrated.state.checkpoint;
                                return tx.set_task(t);
                            }));
                }
                def = std::move(resolution.fit.definition);
                phase_snapshot = std::move(abort_snapshot);
                continue;
            }

            if (!phase_res) {
                if (after.abort_requested) {
                    // The phase unwound on the abort mark (its commits reject
                    // while marked); the abort handler settles the run instead
                    // of faulting the task.
                    run_abort_handler(after);
                } else {
                    settle_faulted(tid, phase_res.error().message);
                }
                cleanup();
                return;
            }

            if (after.state.status == "waiting" || after.state.status == "completing") {
                // The invocation ends once its task parks or holds: the
                // scheduler resumes a settled wait, and `finalize_completing`
                // writes the held outcome's terminal record (frozen pi `#step`
                // ends an invocation whose record is no longer `running`).
                cleanup();
                return;
            }

            bool progress = true;
            if (cp_before.has_value() && after.state.checkpoint.has_value()) {
                if (*cp_before == *after.state.checkpoint) {
                    progress = false;
                }
            } else if (!cp_before.has_value() && !after.state.checkpoint.has_value()) {
                progress = false;
            }

            if (!progress) {
                settle_faulted(
                        tid, "Task " + task.kind + " phase " + phase_name + " returned without durable progress");
                cleanup();
                return;
            }

            phase_snapshot = registry.snapshot();
            auto next_def = find_definition(after.kind, phase_snapshot);
            if (next_def.get() != def.get()) {
                if (next_def != nullptr && can_reserve(*next_def, after)) {
                    (void)await_async_void(session->commit([tid](Transaction& tx) -> support::ExpectedVoid {
                        auto cur_opt = tx.task(tid);
                        if (!cur_opt || !cur_opt->has_value()) return {};
                        TaskRecord cur = *cur_opt.value();
                        if (cur.state.status != "running") return {};
                        cur.state.status = "pending";
                        return tx.set_task(cur);
                    }));
                    cleanup();
                    return;
                }
                if (!reported_replacement || reported_def.get() != next_def.get()) {
                    reported_replacement = true;
                    reported_def = next_def;
                    report(support::make_error(support::ErrorCode::Validation,
                            "Task " + std::to_string(tid.value) + " keeps running under its old " + after.kind +
                                    " definition"));
                }
            }
        }
        cleanup();
    }
};

Harness::Harness(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Harness::Harness(Harness&&) noexcept = default;

Harness& Harness::operator=(Harness&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->shutdown();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Harness::~Harness() {
    if (impl_) {
        impl_->shutdown();
    }
}

support::AsyncResult<std::unique_ptr<Harness>> Harness::open(Storage& storage, HarnessOptions options) {
    for (const auto& task : options.tasks) {
        if (task.kind() == "pi.generation") {
            return support::AsyncResult<std::unique_ptr<Harness>>{std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "pi.generation is a reserved Harness task kind"))};
        }
    }
    auto session = Session::create(storage);
    auto impl = std::make_unique<Impl>(&storage, std::move(session), std::move(options));
    if (auto res = impl->init(); !res) {
        return support::AsyncResult<std::unique_ptr<Harness>>{std::unexpected(res.error())};
    }
    return support::AsyncResult<std::unique_ptr<Harness>>{std::unique_ptr<Harness>(new Harness(std::move(impl)))};
}

Session& Harness::session() noexcept { return *impl_->session; }

const TaskDefinition* Harness::task_definition(std::string_view kind) const noexcept {
    return impl_->find_definition(kind);
}

support::AsyncResult<HarnessInspection> Harness::inspect() {
    // The inspection is one line job (issue #1165): it runs after every
    // commit and read admitted before it has settled and observes one
    // committed state, instead of scanning on the caller thread. Failure and
    // close are handled by the seam's admission (frozen `SessionFailed` /
    // "Session is closed") and, for a read already queued when a commit
    // fails, by the Session's pending-line rejection.
    //
    // Contract notes: inspect must not be called from inside a Session line
    // callback (commit body, read callback, completion); such a call admits a
    // job behind its own and self-waits, the same prohibition class as the
    // frozen listener contracts. A callback that delegates to another thread
    // which then calls inspect is an unresolved cross-thread cycle risk. The
    // Harness and its Storage must outlive the operation until its completion
    // callback returns.
    return support::AsyncResult<HarnessInspection>{support::AsyncProducer<HarnessInspection, support::Error>{
            [impl = impl_.get()](support::AsyncCompletion<HarnessInspection, support::Error> completion) noexcept {
                struct Capture {
                    std::optional<HarnessInspection> value{};
                };
                auto capture = std::make_shared<Capture>();
                auto read = detail::SessionLineAccess::read_on_line(
                        *impl->session, [impl, capture](Storage& storage) -> support::Expected<void> {
                            // No Session method may be called here: this job
                            // already runs on the mutation line.
                            auto inspection = impl->capture_inspection(storage);
                            if (!inspection) return std::unexpected(inspection.error());
                            capture->value = std::move(*inspection);
                            return {};
                        });
                std::move(read).start(
                        [capture, completion = std::move(completion)](support::ExpectedVoid outcome) mutable noexcept {
                            if (!outcome) {
                                completion(std::unexpected(outcome.error()));
                                return;
                            }
                            completion(std::move(*capture->value));
                        });
            }}};
}

support::AsyncResult<void> Harness::commit(std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return impl_->session->commit(std::move(change));
}

support::ExpectedVoid Harness::resume() {
    std::lock_guard lock(impl_->mutex);
    impl_->enabled = true;
    impl_->dirty = true;
    impl_->cv.notify_all();
    return {};
}

support::AsyncResult<void> Harness::open_tasks() {
    return impl_->session->commit([](Transaction& tx) -> support::ExpectedVoid {
        auto tasks = tx.scan_tasks(1024);
        if (!tasks) return std::unexpected(tasks.error());
        for (const auto& task : *tasks) {
            if (task.state.status == "running") {
                TaskRecord pending = task;
                pending.state.status = "pending";
                auto res = tx.set_task(pending);
                if (!res) return res;
            }
        }
        return {};
    });
}

support::AsyncResult<TaskRecord> Harness::wait_for_task(TaskId id, std::stop_token stop) {
    return support::AsyncResult<TaskRecord>{support::AsyncProducer<TaskRecord, support::Error>{
            [this, id, stop = std::move(stop)](
                    support::AsyncCompletion<TaskRecord, support::Error> completion) mutable noexcept {
                // Check and register under the lock so no terminal publication
                // falls between them (frozen pi `TaskScheduler.waitForTask`).
                std::optional<support::Error> immediate_error;
                std::optional<TaskRecord> immediate_value;
                std::shared_ptr<TaskWaiter> waiter;
                {
                    std::lock_guard lock(impl_->mutex);
                    if (impl_->wait_error) {
                        immediate_error = *impl_->wait_error;
                    } else if (auto cur_opt = impl_->storage->task(id); !cur_opt) {
                        immediate_error = cur_opt.error();
                    } else if (!cur_opt->has_value()) {
                        immediate_error = support::make_error(
                                support::ErrorCode::Validation, "Task " + std::to_string(id.value) + " does not exist");
                    } else if ((*cur_opt)->state.status == "terminal") {
                        immediate_value = **cur_opt;
                    } else if (stop.stop_requested()) {
                        immediate_error = support::make_error(support::ErrorCode::Cancelled, "stop waiting");
                    } else {
                        waiter = std::make_shared<TaskWaiter>();
                        waiter->completion = std::move(completion);
                        impl_->waiters[id.value].push_back(waiter);
                    }
                }
                if (immediate_value.has_value()) {
                    completion(std::move(*immediate_value));
                    return;
                }
                if (immediate_error.has_value()) {
                    completion(std::unexpected(*immediate_error));
                    return;
                }
                // Cancelling this wait rejects only this waiter; the task keeps
                // running and settles on its own (frozen pi's abort signal on a
                // wait, not on the task).
                waiter->cancellation = std::make_unique<std::stop_callback<std::move_only_function<void()>>>(
                        stop, [weak = std::weak_ptr<TaskWaiter>{waiter}] {
                            const std::shared_ptr<TaskWaiter> claimed = weak.lock();
                            if (claimed == nullptr) return;
                            if (claimed->outcome.exchange(2) != 0) return;
                            auto done = std::move(claimed->completion);
                            if (done.has_value()) {
                                (*done)(std::unexpected(
                                        support::make_error(support::ErrorCode::Cancelled, "stop waiting")));
                            }
                        });
            }}};
}

support::AsyncResult<void> Harness::wait_for_idle(std::optional<ConversationId> conversation, std::stop_token stop) {
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [this, conversation, stop = std::move(stop)](
                    support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                const std::optional<std::int64_t> scope =
                        conversation.has_value() ? std::optional<std::int64_t>{conversation->value} : std::nullopt;
                {
                    std::lock_guard lock(impl_->mutex);
                    if (impl_->wait_error) {
                        const auto error = *impl_->wait_error;
                        done(std::unexpected(error));
                        return;
                    }
                }
                if (stop.stop_requested()) {
                    done(std::unexpected(support::make_error(support::ErrorCode::Cancelled, "stop waiting")));
                    return;
                }
                auto world = impl_->load_world();
                if (!world) {
                    done(std::unexpected(world.error()));
                    return;
                }
                const bool idle = conversation.has_value() ? world->index.idle(*conversation) : world->index.all_idle();
                if (idle) {
                    done(support::ExpectedVoid{});
                    return;
                }
                auto waiter = std::make_shared<IdleWaiter>();
                waiter->completion = std::move(done);
                {
                    std::lock_guard lock(impl_->mutex);
                    impl_->idle_waiters[scope].push_back(waiter);
                }
                waiter->cancellation = std::make_unique<std::stop_callback<std::move_only_function<void()>>>(
                        stop, [weak = std::weak_ptr<IdleWaiter>{waiter}] {
                            const std::shared_ptr<IdleWaiter> claimed = weak.lock();
                            if (claimed == nullptr) return;
                            if (claimed->outcome.exchange(2) != 0) return;
                            auto completion = std::move(claimed->completion);
                            if (completion.has_value()) {
                                (*completion)(std::unexpected(
                                        support::make_error(support::ErrorCode::Cancelled, "stop waiting")));
                            }
                        });
                // The scope may have become idle before the registration landed.
                impl_->settle_idle();
            }}};
}

support::AsyncResult<std::string> Harness::abort_task(TaskId id) {
    return support::AsyncResult<std::string>{support::AsyncProducer<std::string, support::Error>{
            [this, id](support::AsyncCompletion<std::string, support::Error> completion) mutable noexcept {
                auto current_result = impl_->storage->task(id);
                if (!current_result) {
                    completion(std::unexpected(current_result.error()));
                    return;
                }
                if (!current_result->has_value()) {
                    completion(std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found")));
                    return;
                }
                const TaskRecord current = **current_result;
                if (current.state.status == "terminal") {
                    completion("terminal");
                    return;
                }

                bool has_active_invocation = false;
                {
                    std::lock_guard lock(impl_->mutex);
                    has_active_invocation = impl_->active_invocations.contains(id.value);
                }
                std::optional<std::string> orphan_reason;
                if (!has_active_invocation && current.state.status != "completing") {
                    const auto resolution = impl_->resolve_task(current, impl_->registry.snapshot());
                    orphan_reason = resolution.fit.blocked_reason;
                }
                const std::int64_t ended_at = current_time(impl_->clock);
                auto result = std::make_shared<std::string>("marked");
                impl_->session
                        ->commit([id, orphan_reason = std::move(orphan_reason), ended_at, result](
                                         Transaction& tx) -> support::ExpectedVoid {
                            auto cur = tx.task(id);
                            if (!cur || !cur->has_value()) {
                                return std::unexpected(
                                        support::make_error(support::ErrorCode::Validation, "Task not found"));
                            }
                            TaskRecord task = *cur.value();
                            if (task.state.status == "terminal") {
                                *result = "terminal";
                                return {};
                            }
                            if (orphan_reason.has_value()) {
                                task.state.status = "terminal";
                                task.state.outcome = TaskOutcome{
                                        .status = "orphaned",
                                        .reason = *orphan_reason,
                                };
                                task.ended_at = ended_at;
                                task.memos.clear();
                                return tx.set_task(task);
                            }
                            task.abort_requested = true;
                            return tx.set_task(task);
                        })
                        .start([impl = impl_.get(), id, completion = std::move(completion), result](
                                       support::ExpectedVoid outcome) mutable noexcept {
                            if (!outcome) {
                                completion(std::unexpected(outcome.error()));
                                return;
                            }
                            std::optional<std::stop_source> source;
                            {
                                std::lock_guard lock(impl->mutex);
                                auto invocation = impl->active_invocations.find(id.value);
                                if (invocation != impl->active_invocations.end()) source = invocation->second;
                            }
                            if (source) source->request_stop();
                            impl->cv.notify_all();
                            completion(std::move(*result));
                        });
            }}};
}

support::AsyncResult<std::optional<TaskRecord>> Harness::get_task(TaskId id) {
    auto res = impl_->storage->task(id);
    if (!res) {
        return support::AsyncResult<std::optional<TaskRecord>>{std::unexpected(res.error())};
    }
    return support::AsyncResult<std::optional<TaskRecord>>{*res};
}

support::AsyncResult<ConversationId> Harness::root(RootOptions options) {
    return impl_->workflow->root(std::move(options));
}

support::AsyncResult<ConversationRecord> Harness::fork_conversation(ConversationId parent, EntryId at) {
    return impl_->workflow->fork_conversation(parent, at);
}

support::AsyncResult<void> Harness::reset(ConversationId conversation_id, std::optional<std::string> handoff) {
    return impl_->workflow->reset(conversation_id, std::move(handoff));
}

support::Expected<TaskId> Harness::compact(ConversationId conversation_id, std::optional<std::string> instructions) {
    return impl_->compact(conversation_id, instructions);
}

support::AsyncResult<Submission> Harness::submit(ConversationId conversation_id, SubmissionDraft draft) {
    auto operation = impl_->workflow->submit(conversation_id, std::move(draft));
    // Harness outlives the producer and its completion (submit's declaration).
    return support::AsyncResult<Submission>{support::AsyncProducer<Submission, support::Error>{
            [this, conversation_id, operation = std::move(operation)](
                    support::AsyncCompletion<Submission, support::Error> done) mutable noexcept {
                std::move(operation).start([this, conversation_id, done = std::move(done)](
                                                   support::Expected<SubmissionId> result) mutable noexcept {
                    if (!result) {
                        done(std::unexpected(result.error()));
                        return;
                    }
                    if (auto resumed = resume(); !resumed) {
                        done(std::unexpected(resumed.error()));
                        return;
                    }
                    done(Submission{*result, conversation_id, *impl_->submissions});
                });
            }}};
}

support::AsyncResult<std::string> Harness::abort_submission(
        SubmissionId id, std::optional<ConversationId> conversation_id) {
    auto operation = impl_->submissions->abort_submission(id, conversation_id);
    return support::AsyncResult<std::string>{support::AsyncProducer<std::string, support::Error>{
            [this, operation = std::move(operation)](
                    support::AsyncCompletion<std::string, support::Error> done) mutable noexcept {
                std::move(operation).start(
                        [this, done = std::move(done)](support::Expected<std::string> result) mutable noexcept {
                            if (!result) {
                                done(std::unexpected(result.error()));
                                return;
                            }
                            // Withdrawing a queued item settles a submission; nothing
                            // new becomes schedulable.
                            if (*result == "aborted") {
                                if (auto resumed = resume(); !resumed) {
                                    done(std::unexpected(resumed.error()));
                                    return;
                                }
                            }
                            done(std::move(result));
                        });
            }}};
}

support::AsyncResult<void> Harness::abort_conversation(ConversationId conversation_id, bool background) {
    // One commit withdraws the in-scope queued inputs and marks every live task
    // ordinary traversal reaches; the abort handlers then settle those tasks
    // bottom-up (frozen pi `Conversation.abort`). Resolves once the scope is
    // idle, and with `background` also once every reached task is terminal.
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [this, conversation_id, background](support::AsyncCompletion<void, support::Error> done) mutable noexcept {
                abort_conversation_pass(conversation_id, background, std::move(done));
            }}};
}

void Harness::abort_conversation_pass(
        ConversationId conversation_id, bool background, support::AsyncCompletion<void, support::Error> done) {
    // One abort pass's shared state: the reached tasks it waits for, and the
    // single completion that resolves once they and the scope are idle.
    struct Pass {
        Harness* harness{nullptr};
        ConversationId conversation{};
        bool background{false};
        std::size_t pending{0};
        std::optional<support::Error> failed{};
        support::AsyncCompletion<void, support::Error> done{};

        // `self` keeps the pass state alive until the idle wait resolves; the
        // callback may run long after the initiating call returned.
        static void finish(const std::shared_ptr<Pass>& self) {
            if (--self->pending != 0) return;
            if (self->failed.has_value()) {
                auto completion = std::move(self->done);
                completion(std::unexpected(*self->failed));
                return;
            }
            std::move(self->harness->wait_for_idle(self->conversation))
                    .start([self](support::ExpectedVoid idle) mutable noexcept {
                        if (!idle) {
                            auto completion = std::move(self->done);
                            completion(std::unexpected(idle.error()));
                            return;
                        }
                        // A run that settled before the mark landed can have
                        // started a follow-up run; abort again until the scope
                        // is genuinely idle.
                        auto world = self->harness->impl_->load_world();
                        if (!world) {
                            auto completion = std::move(self->done);
                            completion(std::unexpected(world.error()));
                            return;
                        }
                        if (!world->index.idle(self->conversation)) {
                            self->harness->abort_conversation_pass(
                                    self->conversation, self->background, std::move(self->done));
                            return;
                        }
                        auto completion = std::move(self->done);
                        completion(support::ExpectedVoid{});
                    });
        }
    };

    // The read of the ownership scope, the queued-input withdrawal, and the
    // abort marks share one Session commit, so a run rollover cannot start new
    // work between the read and the marks.
    auto pass = std::make_shared<Pass>();
    pass->harness = this;
    pass->conversation = conversation_id;
    pass->background = background;
    pass->done = std::move(done);
    const auto reached = std::make_shared<std::vector<TaskId>>();
    std::move(impl_->session->commit([conversation_id, background, reached](Transaction& tx) -> support::ExpectedVoid {
        auto tasks = tx.scan_tasks(1024);
        if (!tasks) return std::unexpected(tasks.error());
        auto conversations = tx.scan_conversations(1024);
        if (!conversations) return std::unexpected(conversations.error());
        const OwnershipIndex index(*tasks, *conversations);
        // Mark the reached live tasks. A background task is
        // reached but not marked unless the walk crosses
        // background boundaries.
        for (const TaskId id : index.reachable(conversation_id, background)) {
            reached->push_back(id);
            auto cur = tx.task(id);
            if (!cur) return std::unexpected(cur.error());
            if (!cur->has_value()) continue;
            TaskRecord task = **cur;
            if (task.state.status == "terminal" || task.abort_requested) continue;
            task.abort_requested = true;
            auto written = tx.set_task(std::move(task));
            if (!written) return written;
        }
        auto inbox = tx.doc(inbox_document(), DocumentAddress{.conversation_id = conversation_id});
        if (!inbox) return std::unexpected(inbox.error());
        return withdraw_queued_inputs(tx, *inbox);
    })).start([this, pass, reached](support::ExpectedVoid withdrawn) mutable noexcept {
        if (!withdrawn) {
            auto completion = std::move(pass->done);
            completion(std::unexpected(withdrawn.error()));
            return;
        }
        // The marked tasks' abort handlers run bottom-up: a task's
        // handler waits for its ordinary owned work. With `background`,
        // the reached tasks are waited for directly; otherwise the idle
        // wait covers the ordinary scope only. Ending a held invocation
        // lets its abort handler run (mirrors `abort_task`).
        std::vector<std::stop_source> sources;
        {
            std::lock_guard lock(impl_->mutex);
            for (const TaskId id : *reached) {
                auto invocation = impl_->active_invocations.find(id.value);
                if (invocation != impl_->active_invocations.end()) sources.push_back(invocation->second);
            }
        }
        for (auto& source : sources)
            source.request_stop();
        impl_->cv.notify_all();
        pass->pending = pass->background ? reached->size() : 1;
        if (pass->pending == 0) {
            auto completion = std::move(pass->done);
            completion(support::ExpectedVoid{});
            return;
        }
        if (!pass->background) {
            Pass::finish(pass);
            return;
        }
        for (const TaskId id : *reached) {
            std::move(pass->harness->wait_for_task(id))
                    .start([pass](support::Expected<TaskRecord> settled) mutable noexcept {
                        if (!settled && !pass->failed.has_value()) pass->failed = settled.error();
                        Pass::finish(pass);
                    });
        }
    });
}

support::AsyncResult<void> Harness::close() {
    impl_->shutdown();
    return impl_->session->close();
}

} // namespace cch::durable
