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

ExpectedVoid TaskScheduler::register_handler(
        durable_session::DurableTask task, Handler handler, std::int64_t definition_version) {
    if (!handler || definition_version < 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler handler is invalid"));
    }
    auto work = std::make_shared<Work>(Work{std::move(task), std::move(handler), definition_version, {}});
    std::lock_guard lock(mutex_);
    if (closing_) return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler is closed"));
    const auto registered =
            std::ranges::find_if(handlers_, [&](const auto& item) { return item->task.kind == work->task.kind; });
    if (registered != handlers_.end()) {
        if (definition_version <= (*registered)->definition_version) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "Task definition version must increase"));
        }
        *registered = work;
    } else {
        handlers_.push_back(work);
    }
    const auto recovered = std::ranges::find_if(recovered_, [&](const auto& item) {
        return item.kind == work->task.kind && item.definition_version == definition_version;
    });
    if (recovered != recovered_.end()) {
        queue_.push_back(std::make_shared<Work>(Work{*recovered, work->handler, definition_version, {}}));
        recovered_.erase(recovered);
        ready_.notify_one();
    }
    return {};
}

void TaskScheduler::register_before_hook(BeforeHook hook) {
    if (!hook) return;
    std::lock_guard lock(mutex_);
    before_hooks_.push_back(std::move(hook));
}

void TaskScheduler::register_after_hook(AfterHook hook) {
    if (!hook) return;
    std::lock_guard lock(mutex_);
    after_hooks_.push_back(std::move(hook));
}

void TaskScheduler::set_diagnostics_sink(DiagnosticsSink sink) {
    std::lock_guard lock(mutex_);
    diagnostics_sink_ = std::move(sink);
}

ExpectedVoid TaskScheduler::enqueue(durable_session::DurableTask task) {
    if (task.state != "pending") {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Scheduler requires a pending task and handler"));
    }
    std::lock_guard lock(mutex_);
    if (closing_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Scheduler is closed"));
    }
    const auto registered =
            std::ranges::find_if(handlers_, [&](const auto& item) { return item->task.kind == task.kind; });
    if (registered == handlers_.end() || (*registered)->definition_version != task.definition_version) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Task kind or definition version is unknown"));
    }
    auto work = std::make_shared<Work>(
            Work{std::move(task), (*registered)->handler, (*registered)->definition_version, {}});
    const auto id = work->task.id;
    queue_.push_back(std::move(work));
    std::erase_if(recovered_, [&](const auto& item) { return item.id == id; });
    ready_.notify_one();
    return {};
}

ExpectedVoid TaskScheduler::migrate_task(std::string_view id, std::int64_t definition_version) {
    std::lock_guard store_lock(store_mutex_);
    auto loaded = store_.load_task(id);
    if (!loaded) return std::unexpected(loaded.error());
    {
        std::lock_guard lock(mutex_);
        const auto registered = std::ranges::find_if(handlers_, [&](const auto& item) {
            return item->task.kind == loaded->kind && item->definition_version == definition_version;
        });
        if (registered == handlers_.end()) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "Task kind or definition version is unknown"));
        }
    }
    if (auto result = store_.migrate_task(id, definition_version); !result) return result;
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
        const auto registered = std::ranges::find_if(handlers_, [&](const auto& work) {
            return work->task.kind == recovered.kind && work->definition_version == recovered.definition_version;
        });
        const auto queued =
                std::ranges::find_if(queue_, [&](const auto& work) { return work->task.id == recovered.id; });
        if (registered != handlers_.end() && queued == queue_.end()) {
            queue_.push_back(std::make_shared<Work>(
                    Work{recovered, (*registered)->handler, (*registered)->definition_version, {}}));
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
        worker_.request_stop();
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

        std::vector<BeforeHook> before_hooks;
        std::vector<AfterHook> after_hooks;
        DiagnosticsSink diagnostics_sink;
        {
            std::lock_guard lock(mutex_);
            before_hooks = before_hooks_;
            after_hooks = after_hooks_;
            diagnostics_sink = diagnostics_sink_;
        }
        const auto report_hook_failure = [&](const durable_session::DurableTask& task, const support::Error& error) {
            if (!diagnostics_sink) return;
            (void)diagnostics_sink(task, error);
        };
        ExpectedVoid reserved;
        {
            std::lock_guard lock(store_mutex_);
            reserved = store_.transition(work->task.id, "pending", "running", work->task.checkpoint);
        }
        if (reserved) {
            std::stop_callback close_cancellation(worker_.get_stop_token(), [work] { work->stop.request_stop(); });
            {
                std::lock_guard lock(mutex_);
                if (closing_) work->stop.request_stop();
            }
            for (const auto& hook : before_hooks) {
                auto hook_result = hook(work->task);
                if (!hook_result) report_hook_failure(work->task, hook_result.error());
                std::lock_guard lock(mutex_);
                if (closing_) work->stop.request_stop();
            }
            {
                std::lock_guard lock(store_mutex_);
                auto current = store_.load_task(work->task.id);
                if (current && current->abort_requested) work->stop.request_stop();
            }
            auto result = work->stop.stop_requested()
                                  ? Expected<std::string>{std::unexpected(support::make_error(
                                            support::ErrorCode::Validation, "Task aborted before execution"))}
                                  : work->handler(work->stop.get_token());
            const bool abort_requested = work->stop.stop_requested();
            const auto checkpoint = result ? *result : work->task.checkpoint;
            {
                std::lock_guard lock(store_mutex_);
                if (abort_requested) {
                    if (store_.finish_aborted(work->task.id, checkpoint)) {
                        (void)store_.update_submission_state(work->task.id, "unanswered");
                    } else {
                        auto current = store_.load_task(work->task.id);
                        if (current && current->abort_requested && current->state == "aborted") {
                            (void)store_.transition(work->task.id, "aborted", "aborted", checkpoint);
                            (void)store_.update_submission_state(work->task.id, "unanswered");
                        }
                    }
                } else if (store_.transition(work->task.id, "running", "completing", checkpoint)) {
                    const auto terminal_state = result ? "completed" : "failed";
                    if (store_.transition(work->task.id, "completing", terminal_state, checkpoint)) {
                        (void)store_.update_submission_state(work->task.id, "done");
                    }
                }
            }
            auto terminal = store_.load_task(work->task.id);
            if (terminal) {
                for (const auto& hook : after_hooks) {
                    auto hook_result = hook(*terminal);
                    if (!hook_result) report_hook_failure(*terminal, hook_result.error());
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
