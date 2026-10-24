#include <cch/durable/Harness.hpp>

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

[[nodiscard]] std::int64_t current_time(const std::shared_ptr<std::move_only_function<std::int64_t()>>& clock) {
    return (*clock)();
}

class HarnessTaskRuntime final : public TaskRuntime {
public:
    HarnessTaskRuntime(Session& session,
            TaskId id,
            ConversationId conv_id,
            std::stop_token stop_token,
            std::shared_ptr<std::move_only_function<std::int64_t()>> now_fn,
            bool allow_abort_commit = false)
        : session_(session), task_id_(id), conversation_id_(conv_id), stop_token_(std::move(stop_token)),
          now_fn_(std::move(now_fn)), allow_abort_commit_(allow_abort_commit) {}

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

private:
    Session& session_; // must outlive every pending operation returned by this runtime
    TaskId task_id_;
    ConversationId conversation_id_;
    std::stop_token stop_token_;
    std::shared_ptr<std::move_only_function<std::int64_t()>> now_fn_;
    bool allow_abort_commit_{false};
};

} // namespace

struct Harness::Impl {
    Storage* storage{nullptr}; // must outlive this Harness and all of its operations
    std::unique_ptr<Session> session;
    HarnessOptions options;
    std::shared_ptr<std::move_only_function<std::int64_t()>> clock;

    struct ExecutionThread {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::jthread worker;
    std::vector<ExecutionThread> execution_threads;
    bool enabled{false};
    std::atomic_bool closed{false};

    std::map<std::int64_t, std::stop_source> active_invocations;
    std::map<std::int64_t, std::vector<support::AsyncCompletion<TaskRecord, support::Error>>> waiters;

    Impl(Storage* s, std::unique_ptr<Session> sess, HarnessOptions opt)
        : storage(s), session(std::move(sess)), options(std::move(opt)),
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

    [[nodiscard]] const TaskDefinition* find_definition(std::string_view kind) const {
        for (const auto& def : options.tasks) {
            if (def.kind() == kind) {
                return &def;
            }
        }
        return nullptr;
    }

    void notify_waiters(const TaskRecord& record) {
        std::vector<support::AsyncCompletion<TaskRecord, support::Error>> comps;
        {
            std::lock_guard lock(mutex);
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

    void init() {
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
            cv.notify_all();
        });

        worker = std::jthread([this](std::stop_token stop) { run_worker(stop); });
    }

    void shutdown() {
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
            lock.unlock();

            schedule_step();

            std::unique_lock wait_lock(mutex);
            cv.wait_for(wait_lock, std::chrono::milliseconds(10), [&] { return stop.stop_requested() || closed; });
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

        for (const auto& task : *scan_res) {
            if (task.state.status == "terminal") {
                notify_waiters(task);
                continue;
            }

            if (task.state.status == "pending") {
                std::unique_lock lock(mutex);
                if (closed || active_invocations.contains(task.id.value)) {
                    continue;
                }

                const TaskDefinition* def = find_definition(task.kind);
                if (def == nullptr) {
                    if (task.abort_requested) {
                        lock.unlock();
                        (void)await_async_void(
                                session->commit([this, tid = task.id](Transaction& tx) -> support::ExpectedVoid {
                                    auto cur = tx.task(tid);
                                    if (cur && cur->has_value()) {
                                        auto t = *cur.value();
                                        t.state.status = "terminal";
                                        t.state.outcome = TaskOutcome{
                                                .status = "orphaned",
                                                .reason = "missing_task",
                                        };
                                        t.ended_at = current_time(clock);
                                        t.memos.clear();
                                        return tx.set_task(t);
                                    }
                                    return {};
                                }));
                    }
                    continue;
                }

                std::stop_source stop_src;
                active_invocations[task.id.value] = stop_src;
                lock.unlock();

                auto tid = task.id;
                auto trans_res =
                        await_async_void(session->commit([this, tid](Transaction& tx) -> support::ExpectedVoid {
                            auto cur_opt = tx.task(tid);
                            if (!cur_opt || !cur_opt->has_value()) return {};
                            auto cur = *cur_opt.value();
                            if (cur.state.status != "pending") return {};
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
                        task_thread.thread = std::thread([this, tid, def, stop_src, finished]() {
                            run_task_execution(tid, def, stop_src);
                            finished->store(true, std::memory_order_release);
                        });
                        execution_threads.push_back(std::move(task_thread));
                    }
                }
            }
        }
    }

    void run_task_execution(TaskId tid, const TaskDefinition* def, std::stop_source stop_src) {
        auto cleanup = [&]() {
            std::lock_guard lock(mutex);
            active_invocations.erase(tid.value);
            cv.notify_all();
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
                HarnessTaskRuntime runtime(*session, tid, task.conversation_id, stop_src.get_token(), clock, true);
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
                        (void)await_async_void(
                                session->commit([this, tid, message](Transaction& tx) -> support::ExpectedVoid {
                                    auto cur = tx.task(tid);
                                    if (cur && cur->has_value()) {
                                        auto t = *cur.value();
                                        t.state.status = "terminal";
                                        t.state.outcome = TaskOutcome{
                                                .status = "faulted",
                                                .error = TaskOutcomeError{.message = message},
                                        };
                                        t.ended_at = current_time(clock);
                                        t.memos.clear();
                                        return tx.set_task(t);
                                    }
                                    return {};
                                }));
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
            HarnessTaskRuntime runtime(*session, tid, task.conversation_id, stop_src.get_token(), clock);

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
                continue;
            }

            if (!phase_res) {
                (void)await_async_void(session->commit(
                        [this, tid, msg = phase_res.error().message](Transaction& tx) -> support::ExpectedVoid {
                            auto cur = tx.task(tid);
                            if (cur && cur->has_value()) {
                                auto t = *cur.value();
                                t.state.status = "terminal";
                                t.state.outcome = TaskOutcome{
                                        .status = "faulted",
                                        .error = TaskOutcomeError{.message = std::move(msg)},
                                };
                                t.ended_at = current_time(clock);
                                t.memos.clear();
                                return tx.set_task(t);
                            }
                            return {};
                        }));
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
                std::string msg = "Task " + task.kind + " phase " + phase_name + " returned without durable progress";
                (void)await_async_void(
                        session->commit([this, tid, msg = std::move(msg)](Transaction& tx) -> support::ExpectedVoid {
                            auto cur = tx.task(tid);
                            if (cur && cur->has_value()) {
                                auto t = *cur.value();
                                t.state.status = "terminal";
                                t.state.outcome = TaskOutcome{
                                        .status = "faulted",
                                        .error = TaskOutcomeError{.message = std::move(msg)},
                                };
                                t.ended_at = current_time(clock);
                                t.memos.clear();
                                return tx.set_task(t);
                            }
                            return {};
                        }));
                cleanup();
                return;
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
    auto session = Session::create(storage);
    auto impl = std::make_unique<Impl>(&storage, std::move(session), std::move(options));
    impl->init();
    return support::AsyncResult<std::unique_ptr<Harness>>{std::unique_ptr<Harness>(new Harness(std::move(impl)))};
}

Session& Harness::session() noexcept { return *impl_->session; }

const TaskDefinition* Harness::task_definition(std::string_view kind) const noexcept {
    return impl_->find_definition(kind);
}

support::AsyncResult<void> Harness::commit(std::move_only_function<support::Expected<void>(Transaction&)> change) {
    return impl_->session->commit(std::move(change));
}

support::ExpectedVoid Harness::resume() {
    std::lock_guard lock(impl_->mutex);
    impl_->enabled = true;
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
                auto cur_opt = impl_->storage->task(id);
                if (cur_opt && cur_opt->has_value() && (*cur_opt)->state.status == "terminal") {
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

                const bool missing_definition = impl_->find_definition(current.kind) == nullptr;
                const std::int64_t ended_at = current_time(impl_->clock);
                auto result = std::make_shared<std::string>("marked");
                impl_->session
                        ->commit([id, missing_definition, ended_at, result](Transaction& tx) -> support::ExpectedVoid {
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
                            if (missing_definition) {
                                task.state.status = "terminal";
                                task.state.outcome = TaskOutcome{
                                        .status = "orphaned",
                                        .reason = "missing_task",
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

support::AsyncResult<void> Harness::close() {
    impl_->shutdown();
    return impl_->session->close();
}

} // namespace cch::durable
