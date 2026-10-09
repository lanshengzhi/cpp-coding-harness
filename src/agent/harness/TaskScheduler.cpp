#include "agent/harness/TaskScheduler.hpp"

#include <algorithm>
#include <utility>

namespace cch::harness {

TaskScheduler::TaskScheduler(durable_session::TaskStore& store) : store_(store) {
    auto recovered = store_.recover_tasks();
    if (recovered) recovered_ = std::move(*recovered);
    worker_ = std::jthread([this](std::stop_token) { run(); });
}

TaskScheduler::~TaskScheduler() { close(); }

ExpectedVoid TaskScheduler::register_handler(durable_session::DurableTask task, Handler handler) {
    if (!handler) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler requires a handler"));
    }
    auto work = std::make_shared<Work>(Work{std::move(task), std::move(handler), {}});
    std::lock_guard lock(mutex_);
    if (closing_) return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler is closed"));
    handlers_.push_back(work);
    const auto recovered = std::ranges::find_if(recovered_, [&](const auto& item) { return item.id == work->task.id; });
    if (recovered != recovered_.end()) {
        queue_.push_back(std::make_shared<Work>(Work{*recovered, work->handler, {}}));
        recovered_.erase(recovered);
    }
    return {};
}

ExpectedVoid TaskScheduler::enqueue(durable_session::DurableTask task, Handler handler) {
    if (task.state != "pending" || !handler) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Scheduler requires a pending task and handler"));
    }
    auto work = std::make_shared<Work>(Work{std::move(task), std::move(handler), {}});
    std::lock_guard lock(mutex_);
    if (closing_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler is closed"));
    }
    const auto id = work->task.id;
    queue_.push_back(std::move(work));
    std::erase_if(recovered_, [&](const auto& item) { return item.id == id; });
    ready_.notify_one();
    return {};
}

ExpectedVoid TaskScheduler::request_abort(std::string_view id) {
    std::vector<std::string> affected;
    {
        std::lock_guard lock(store_mutex_);
        auto descendants = store_.task_descendants(id);
        if (!descendants) return std::unexpected(descendants.error());
        affected = std::move(*descendants);
        if (auto result = store_.request_abort(id); !result) return result;
    }
    std::lock_guard lock(mutex_);
    for (const auto& work : active_) {
        if (std::ranges::find(affected, work->task.id) != affected.end()) work->stop.request_stop();
    }
    std::erase_if(
            queue_, [&](const auto& work) { return std::ranges::find(affected, work->task.id) != affected.end(); });
    std::erase_if(recovered_, [&](const auto& task) { return std::ranges::find(affected, task.id) != affected.end(); });
    ready_.notify_one();
    return {};
}

void TaskScheduler::resume() noexcept {
    std::lock_guard lock(mutex_);
    for (const auto& recovered : recovered_) {
        const auto registered =
                std::ranges::find_if(handlers_, [&](const auto& work) { return work->task.id == recovered.id; });
        const auto queued =
                std::ranges::find_if(queue_, [&](const auto& work) { return work->task.id == recovered.id; });
        if (registered != handlers_.end() && queued == queue_.end()) {
            queue_.push_back(std::make_shared<Work>(Work{recovered, (*registered)->handler, {}}));
        }
    }
    resumed_ = true;
    ready_.notify_one();
}

void TaskScheduler::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        closing_ = true;
        for (const auto& work : active_)
            work->stop.request_stop();
        queue_.clear();
        ready_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void TaskScheduler::run() noexcept {
    for (;;) {
        std::shared_ptr<Work> work;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [&] { return closing_ || (resumed_ && !queue_.empty()); });
            if (closing_) return;
            if (!resumed_ || queue_.empty()) continue;
            work = std::move(queue_.front());
            queue_.erase(queue_.begin());
            active_.push_back(work);
        }

        ExpectedVoid reserved;
        {
            std::lock_guard lock(store_mutex_);
            reserved = store_.transition(work->task.id, "pending", "running", work->task.checkpoint);
        }
        if (reserved) {
            auto result = work->stop.stop_requested()
                                  ? Expected<std::string>{std::unexpected(support::make_error(
                                            support::ErrorCode::Validation, "Task aborted before execution"))}
                                  : work->handler(work->stop.get_token());
            const bool abort_requested = work->stop.stop_requested();
            const auto checkpoint = result ? *result : work->task.checkpoint;
            std::lock_guard lock(store_mutex_);
            if (abort_requested) {
                if (store_.finish_aborted(work->task.id, checkpoint)) {
                    (void)store_.update_submission_state(work->task.id, "unanswered");
                }
            } else if (store_.transition(work->task.id, "running", "completing", checkpoint)) {
                const auto terminal_state = result ? "completed" : "failed";
                if (store_.transition(work->task.id, "completing", terminal_state, checkpoint)) {
                    (void)store_.update_submission_state(work->task.id, "done");
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            std::erase(active_, work);
            ready_.notify_all();
        }
    }
}

} // namespace cch::harness
