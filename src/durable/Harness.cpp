#include <cch/durable/Harness.hpp>

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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

[[nodiscard]] std::int64_t current_time(const std::shared_ptr<std::move_only_function<std::int64_t()>>& clock) {
    return (*clock)();
}

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
            bool allow_abort_commit = false)
        : session_(session), task_id_(id), conversation_id_(conv_id), task_def_(task_def),
          phase_hooks_(std::move(phase_hooks)), stop_token_(std::move(stop_token)), now_fn_(std::move(now_fn)),
          report_fn_(std::move(report_fn)), allow_abort_commit_(allow_abort_commit) {}

    [[nodiscard]] TaskId task_id() const noexcept override { return task_id_; }
    [[nodiscard]] ConversationId conversation_id() const noexcept override { return conversation_id_; }
    [[nodiscard]] const std::stop_token& stop_token() const noexcept override { return stop_token_; }

    [[nodiscard]] support::AsyncResult<void> commit(CommitCallback change) override {
        const TaskId task_id = task_id_;
        const bool allow_abort_commit = allow_abort_commit_;
        const auto now_fn = now_fn_;
        return session_.commit([task_id, allow_abort_commit, now_fn, change = std::move(change)](
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
                    current.state.status = "terminal";
                    current.state.outcome = std::move(next.outcome);
                    current.ended_at = current_time(now_fn);
                    current.memos.clear();
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
};

[[nodiscard]] bool can_reserve(const TaskDefinition& def, const TaskRecord& record) noexcept {
    return def.version() == record.version || (def.version() > record.version && def.has_migrate());
}

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
    std::shared_ptr<TaskDefinition> builtin_generation_task;

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
    std::map<std::int64_t, std::vector<support::AsyncCompletion<TaskRecord, support::Error>>> waiters;
    std::optional<support::Error> wait_error;

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

    [[nodiscard]] TaskInspectionState inspect_task(const TaskRecord& record, const Registry::Snapshot& snapshot) const {
        {
            std::lock_guard lock(mutex);
            if (active_invocations.contains(record.id.value)) {
                return TaskInspectionState{.kind = "running"};
            }
        }
        if (record.state.status == "completing") {
            return TaskInspectionState{.kind = "completing"};
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
        std::vector<support::AsyncCompletion<TaskRecord, support::Error>> comps;
        {
            std::lock_guard lock(mutex);
            failed_migrations.erase(record.id.value);
            auto it = waiters.find(record.id.value);
            if (it != waiters.end()) {
                comps = std::move(it->second);
                waiters.erase(it);
            }
        }
        for (auto& comp : comps) {
            comp(record);
        }
    }

    support::ExpectedVoid init() {
        if (!options.tasks.empty()) {
            Extension initial_ext{.name = ".harness.tasks", .tasks = std::move(options.tasks)};
            if (auto res = registry.install(std::move(initial_ext)); !res) {
                return res;
            }
        }
        workflow = std::make_unique<WorkflowRuntime>(*session, *storage, options, clock);
        builtin_generation_task = std::make_shared<TaskDefinition>(workflow->definition());
        registry_sub_id = registry.subscribe([this] {
            std::lock_guard lock(mutex);
            dirty = true;
            cv.notify_all();
        });
        (void)session->subscribe_close([this] {
            std::map<std::int64_t, std::vector<support::AsyncCompletion<TaskRecord, support::Error>>> pending;
            const auto error = support::make_error(support::ErrorCode::Validation,
                    session->failed() ? "Session failed after a storage error; close and reopen it" : "Session is closed");
            {
                std::lock_guard lock(mutex);
                wait_error = error;
                pending = std::move(waiters);
                waiters.clear();
            }
            for (auto& [id, completions] : pending) {
                (void)id;
                for (auto& completion : completions) completion(std::unexpected(error));
            }
        });
        (void)session->subscribe_commits([this](const CommitPublication& pub) {
            for (const auto& change : pub.changes) {
                if (const auto* tc = std::get_if<TableCommitChange>(&change)) {
                    if (tc->type == "task" && tc->task.has_value()) {
                        if (tc->task->state.status == "terminal") {
                            notify_waiters(*tc->task);
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
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            closed = true;
            for (auto& [_, stop_src] : active_invocations) {
                stop_src.request_stop();
            }
        }
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
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return stop.stop_requested() || closed || (enabled && !closed); });
            if (stop.stop_requested() || closed) {
                break;
            }
            dirty = false;
            lock.unlock();

            schedule_step();

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

        std::optional<Registry::Snapshot> snapshot_opt;

        for (const auto& task : *scan_res) {
            if (task.state.status == "terminal") {
                notify_waiters(task);
                continue;
            }

            if (task.state.status == "pending") {
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
                    active_invocations[task.id.value] = stop_src;
                }

                auto tid = task.id;
                TaskRecord resolved_record = std::move(resolution.record);
                auto trans_res =
                        await_async_void(session->commit([this, tid, resolved_record = std::move(resolved_record)](
                                                                 Transaction& tx) -> support::ExpectedVoid {
                            auto cur_opt = tx.task(tid);
                            if (!cur_opt || !cur_opt->has_value()) return {};
                            auto cur = *cur_opt.value();
                            if (cur.state.status != "pending") return {};
                            cur.version = resolved_record.version;
                            cur.input = resolved_record.input;
                            cur.state.checkpoint = resolved_record.state.checkpoint;
                            cur.state.status = "running";
                            if (!cur.started_at.has_value()) {
                                cur.started_at = current_time(clock);
                            }
                            return tx.set_task(cur);
                        }));

                if (!trans_res) {
                    std::lock_guard l(mutex);
                    active_invocations.erase(tid.value);
                    continue;
                }

                {
                    std::lock_guard l(mutex);
                    if (closed) {
                        active_invocations.erase(tid.value);
                        cv.notify_all();
                    } else {
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

        bool reported_replacement = false;
        std::shared_ptr<const TaskDefinition> reported_def;

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
                    cleanup();
                    return;
                }
                auto after_opt = storage->task(tid);
                if (after_opt && after_opt->has_value()) {
                    TaskRecord after = *after_opt.value();
                    if (after.state.status != "terminal") {
                        const std::string message = abort_result
                                                            ? "Abort handler of task " + std::to_string(tid.value) +
                                                                      " returned without a terminal outcome"
                                                            : abort_result.error().message;
                        settle_faulted(tid, message);
                    }
                }
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
                settle_faulted(tid, phase_res.error().message);
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
    auto scan_res = impl_->storage->scan_tasks(256);
    if (!scan_res) {
        return support::AsyncResult<HarnessInspection>{std::unexpected(scan_res.error())};
    }
    const auto snapshot = impl_->registry.snapshot();
    HarnessInspection inspection;
    {
        std::lock_guard lock(impl_->mutex);
        inspection.scheduling = impl_->closed ? "closing" : (impl_->enabled ? "running" : "paused");
    }
    for (const auto& task : *scan_res) {
        if (task.state.status == "terminal") {
            continue;
        }
        inspection.tasks.push_back(TaskInspection{
                .record = task,
                .state = impl_->inspect_task(task, snapshot),
        });
    }
    return support::AsyncResult<HarnessInspection>{std::move(inspection)};
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

support::AsyncResult<TaskRecord> Harness::wait_for_task(TaskId id) {
    return support::AsyncResult<TaskRecord>{support::AsyncProducer<TaskRecord, support::Error>{
            [this, id](support::AsyncCompletion<TaskRecord, support::Error> completion) mutable noexcept {
                std::unique_lock lock(impl_->mutex);
                if (impl_->wait_error) {
                    const auto error = *impl_->wait_error;
                    lock.unlock();
                    completion(std::unexpected(error));
                    return;
                }
                auto cur_opt = impl_->storage->task(id);
                if (!cur_opt || !cur_opt->has_value()) {
                    const auto error = !cur_opt ? cur_opt.error() : support::make_error(support::ErrorCode::Validation, "Task not found");
                    lock.unlock();
                    completion(std::unexpected(error));
                    return;
                }
                if ((*cur_opt)->state.status == "terminal") {
                    lock.unlock();
                    completion(**cur_opt);
                    return;
                }
                impl_->waiters[id.value].push_back(std::move(completion));
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
                            {
                                std::lock_guard lock(impl->mutex);
                                auto invocation = impl->active_invocations.find(id.value);
                                if (invocation != impl->active_invocations.end()) {
                                    invocation->second.request_stop();
                                }
                            }
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

support::AsyncResult<Submission> Harness::submit(ConversationId conversation_id, std::string text) {
    auto operation = impl_->workflow->submit(conversation_id, std::move(text), *task_definition("pi.generation"));
    // Harness outlives the producer and its completion (submit's declaration).
    return support::AsyncResult<Submission>{support::AsyncProducer<Submission, support::Error>{
            [this, operation = std::move(operation)](support::AsyncCompletion<Submission, support::Error> done) mutable noexcept {
                std::move(operation).start([this, done = std::move(done)](support::Expected<Submission> result) mutable noexcept {
                    if (result) {
                        if (auto resumed = resume(); !resumed) {
                            done(std::unexpected(resumed.error()));
                            return;
                        }
                    }
                    done(std::move(result));
                });
            }}};
}

support::AsyncResult<SettledSubmission> Harness::wait_for_submission(Submission submission) {
    auto operation = wait_for_task(submission.id);
    return support::AsyncResult<SettledSubmission>{support::AsyncProducer<SettledSubmission, support::Error>{
            [operation = std::move(operation)](support::AsyncCompletion<SettledSubmission, support::Error> done) mutable noexcept {
                std::move(operation).start([done = std::move(done)](support::Expected<TaskRecord> result) mutable noexcept {
                    if (!result) done(std::unexpected(result.error()));
                    else done(WorkflowRuntime::receipt(*result));
                });
            }}};
}

support::AsyncResult<void> Harness::close() {
    impl_->shutdown();
    return impl_->session->close();
}

} // namespace cch::durable
