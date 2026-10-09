#pragma once

#include "agent/harness/session/TaskStore.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace cch::harness {

namespace session = cch::agent::session;
using cch::support::Expected;
using cch::support::ExpectedVoid;

/// Serialized durable task dispatch. Recovery is performed on construction;
/// callers must explicitly resume before any recovered task is dispatched.
class TaskScheduler final {
public:
    using Handler = std::function<Expected<std::string>(std::stop_token)>;

    explicit TaskScheduler(session::TaskStore& store);
    ~TaskScheduler();
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    [[nodiscard]] ExpectedVoid register_handler(session::DurableTask task, Handler handler);
    [[nodiscard]] ExpectedVoid enqueue(session::DurableTask task, Handler handler);
    [[nodiscard]] ExpectedVoid request_abort(std::string_view id);
    void resume() noexcept;
    void close() noexcept;

private:
    struct Work final {
        session::DurableTask task;
        Handler handler;
        std::stop_source stop;
    };

    void run() noexcept;

    session::TaskStore& store_;
    std::mutex mutex_;
    std::mutex store_mutex_;
    std::condition_variable ready_;
    std::vector<std::shared_ptr<Work>> queue_;
    std::vector<std::shared_ptr<Work>> active_;
    std::vector<std::shared_ptr<Work>> handlers_;
    std::vector<session::DurableTask> recovered_;
    std::jthread worker_;
    bool resumed_{false};
    bool closing_{false};
};

} // namespace cch::harness
