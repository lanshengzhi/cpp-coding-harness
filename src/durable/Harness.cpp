#include <cch/durable/Harness.hpp>

#include <cch/durable/Records.hpp>
#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/durable/Task.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

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

template <typename T>
support::Expected<T> await_async(support::AsyncResult<T> result) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    support::Expected<T> outcome = std::unexpected(support::Error{});
    std::move(result).start([&](support::Expected<T> terminal) mutable noexcept {
        std::lock_guard l(m);
        outcome = std::move(terminal);
        done = true;
        cv.notify_one();
    });
    std::unique_lock l(m);
    cv.wait(l, [&] { return done; });
    return outcome;
}

inline support::ExpectedVoid await_async_void(support::AsyncResult<void> result) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    support::ExpectedVoid outcome = std::unexpected(support::Error{});
    std::move(result).start([&](support::ExpectedVoid terminal) mutable noexcept {
        std::lock_guard l(m);
        outcome = std::move(terminal);
        done = true;
        cv.notify_one();
    });
    std::unique_lock l(m);
    cv.wait(l, [&] { return done; });
    return outcome;
}

class HarnessTaskRuntime final : public TaskRuntime {
public:
    HarnessTaskRuntime(
            Session& session,
            TaskId id,
            ConversationId conv_id,
            std::stop_token stop_token,
            std::function<std::int64_t()> now_fn)
        : session_(session),
          task_id_(id),
          conversation_id_(conv_id),
          stop_token_(std::move(stop_token)),
          now_fn_(std::move(now_fn)) {}

    [[nodiscard]] TaskId task_id() const noexcept override { return task_id_; }
    [[nodiscard]] ConversationId conversation_id() const noexcept override { return conversation_id_; }
    [[nodiscard]] const std::stop_token& stop_token() const noexcept override { return stop_token_; }

    [[nodiscard]] support::AsyncResult<void> commit(CommitCallback change) override {
        return session_.commit([this, change = std::move(change)](Transaction& tx) mutable -> support::ExpectedVoid {
            auto cur_opt = tx.task(task_id_);
            if (!cur_opt || !cur_opt->has_value()) {
                return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found"));
            }
            TaskRecord current = *cur_opt.value();
            auto next_res = change(tx, current);
            if (!next_res) {
                return std::unexpected(next_res.error());
            }
            if (next_res->has_value()) {
                auto next = std::move(*next_res.value());
                if (next.status == "terminal") {
                    current.state.status = "terminal";
                    current.state.outcome = std::move(next.outcome);
                    current.ended_at = now_fn_();
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
        auto commit_res = session_.commit(
                [this, key = std::move(key), candidate = std::move(candidate), memo_val](
                        Transaction& tx) mutable -> support::ExpectedVoid {
                    auto cur_opt = tx.task(task_id_);
                    if (!cur_opt || !cur_opt->has_value()) {
                        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found"));
                    }
                    TaskRecord current = *cur_opt.value();
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
                    std::move(commit_res).start([completion = std::move(completion), memo_val](
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
    Session& session_;
    TaskId task_id_;
    ConversationId conversation_id_;
    std::stop_token stop_token_;
    std::function<std::int64_t()> now_fn_;
};

} // namespace

struct Harness::Impl {
    Storage* storage{nullptr};
    std::unique_ptr<Session> session;
    HarnessOptions options;

    std::mutex mutex;
    std::condition_variable cv;
    std::jthread worker;
    std::vector<std::thread> execution_threads;
    bool enabled{false};
    bool closed{false};

    std::map<std::int64_t, std::stop_source> active_invocations;
    std::map<std::int64_t, std::vector<support::AsyncCompletion<TaskRecord, support::Error>>> waiters;

    Impl(Storage* s, std::unique_ptr<Session> sess, HarnessOptions opt)
        : storage(s), session(std::move(sess)), options(std::move(opt)) {}

    [[nodiscard]] std::int64_t current_time() const {
        if (options.now) {
            return options.now();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

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

        worker = std::jthread([this](std::stop_token stop) {
            run_worker(stop);
        });
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
            for (auto& t : execution_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            execution_threads.clear();
        }
    }

    void run_worker(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] {
                return stop.stop_requested() || closed || (enabled && !closed);
            });
            if (stop.stop_requested() || closed) {
                break;
            }
            lock.unlock();

            schedule_step();

            std::unique_lock wait_lock(mutex);
            cv.wait_for(wait_lock, std::chrono::milliseconds(10), [&] {
                return stop.stop_requested() || closed;
            });
        }
    }

    void schedule_step() {
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
                        (void)await_async_void(session->commit([this, tid = task.id](Transaction& tx) -> support::ExpectedVoid {
                            auto cur = tx.task(tid);
                            if (cur && cur->has_value()) {
                                auto t = *cur.value();
                                t.state.status = "terminal";
                                t.state.outcome = TaskOutcome{
                                    .status = "orphaned",
                                    .reason = "missing_task",
                                };
                                t.ended_at = current_time();
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
                auto trans_res = await_async_void(session->commit([this, tid](Transaction& tx) -> support::ExpectedVoid {
                    auto cur_opt = tx.task(tid);
                    if (!cur_opt || !cur_opt->has_value()) return {};
                    auto cur = *cur_opt.value();
                    if (cur.state.status != "pending") return {};
                    cur.state.status = "running";
                    if (!cur.started_at.has_value()) {
                        cur.started_at = current_time();
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
                        execution_threads.emplace_back([this, tid, def, stop_src]() {
                            run_task_execution(tid, def, stop_src);
                        });
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
                HarnessTaskRuntime runtime(*session, tid, task.conversation_id, stop_src.get_token(), [this]() {
                    return current_time();
                });
                auto abort_res = await_async_void(def->run_abort(task, runtime));
                (void)abort_res;
                if (closed) {
                    cleanup();
                    return;
                }
                auto after_opt = storage->task(tid);
                if (after_opt && after_opt->has_value()) {
                    TaskRecord after = *after_opt.value();
                    if (after.state.status != "terminal") {
                        (void)await_async_void(session->commit([this, tid](Transaction& tx) -> support::ExpectedVoid {
                            auto cur = tx.task(tid);
                            if (cur && cur->has_value()) {
                                auto t = *cur.value();
                                t.state.status = "terminal";
                                t.state.outcome = TaskOutcome{
                                    .status = "faulted",
                                    .error = TaskOutcomeError{
                                        .message = "Abort handler of task " + std::to_string(tid.value) + " returned without a terminal outcome",
                                    },
                                };
                                t.ended_at = current_time();
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
            HarnessTaskRuntime runtime(*session, tid, task.conversation_id, stop_src.get_token(), [this]() {
                return current_time();
            });

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
                (void)await_async_void(session->commit([this, tid, msg = phase_res.error().message](Transaction& tx) -> support::ExpectedVoid {
                    auto cur = tx.task(tid);
                    if (cur && cur->has_value()) {
                        auto t = *cur.value();
                        t.state.status = "terminal";
                        t.state.outcome = TaskOutcome{
                            .status = "faulted",
                            .error = TaskOutcomeError{.message = std::move(msg)},
                        };
                        t.ended_at = current_time();
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
                (void)await_async_void(session->commit([this, tid, msg = std::move(msg)](Transaction& tx) -> support::ExpectedVoid {
                    auto cur = tx.task(tid);
                    if (cur && cur->has_value()) {
                        auto t = *cur.value();
                        t.state.status = "terminal";
                        t.state.outcome = TaskOutcome{
                            .status = "faulted",
                            .error = TaskOutcomeError{.message = std::move(msg)},
                        };
                        t.ended_at = current_time();
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
Harness& Harness::operator=(Harness&&) noexcept = default;

Harness::~Harness() {
    if (impl_) {
        impl_->shutdown();
    }
}

support::AsyncResult<std::unique_ptr<Harness>> Harness::open(
        Storage& storage, HarnessOptions options) {
    auto session = Session::create(storage);
    auto impl = std::make_unique<Impl>(&storage, std::move(session), std::move(options));
    impl->init();
    return support::AsyncResult<std::unique_ptr<Harness>>{
            std::unique_ptr<Harness>(new Harness(std::move(impl)))};
}

Session& Harness::session() noexcept {
    return *impl_->session;
}

const TaskDefinition* Harness::task_definition(std::string_view kind) const noexcept {
    return impl_->find_definition(kind);
}

support::AsyncResult<void> Harness::commit(
        std::move_only_function<support::Expected<void>(Transaction&)> change) {
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
                auto current_opt = impl_->storage->task(id);
                if (!current_opt || !current_opt->has_value()) {
                    completion(std::unexpected(support::make_error(support::ErrorCode::Validation, "Task not found")));
                    return;
                }
                if ((*current_opt)->state.status == "terminal") {
                    completion("terminal");
                    return;
                }

                {
                    std::lock_guard lock(impl_->mutex);
                    auto it = impl_->active_invocations.find(id.value);
                    if (it != impl_->active_invocations.end()) {
                        it->second.request_stop();
                    }
                }

                const TaskDefinition* def = impl_->find_definition((*current_opt)->kind);
                if (def == nullptr) {
                    auto term_res = await_async_void(impl_->session->commit([this, id](Transaction& tx) -> support::ExpectedVoid {
                        auto cur = tx.task(id);
                        if (cur && cur->has_value()) {
                            auto t = *cur.value();
                            t.state.status = "terminal";
                            t.state.outcome = TaskOutcome{
                                .status = "orphaned",
                                .reason = "missing_task",
                            };
                            t.ended_at = impl_->current_time();
                            t.memos.clear();
                            return tx.set_task(t);
                        }
                        return {};
                    }));
                    if (!term_res) {
                        completion(std::unexpected(term_res.error()));
                        return;
                    }
                    impl_->cv.notify_all();
                    completion("marked");
                    return;
                }

                auto mark_res = await_async_void(impl_->session->commit([id](Transaction& tx) -> support::ExpectedVoid {
                    auto cur = tx.task(id);
                    if (cur && cur->has_value()) {
                        auto t = *cur.value();
                        t.abort_requested = true;
                        return tx.set_task(t);
                    }
                    return {};
                }));
                if (!mark_res) {
                    completion(std::unexpected(mark_res.error()));
                    return;
                }
                impl_->cv.notify_all();
                completion("marked");
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
