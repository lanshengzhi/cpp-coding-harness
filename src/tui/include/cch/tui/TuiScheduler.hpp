#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace cch::tui {

using TuiScheduledWork = std::move_only_function<void()>;

/// Handle to one schedule_delayed() work item. Destroying the handle cancels
/// the work (destruction IS cancellation); after the work has fired or the
/// owning scheduler died, cancel() and destruction are no-ops. Handles never
/// affect render or interval timers.
class TuiScheduledHandle {
public:
    virtual ~TuiScheduledHandle() = default;
    virtual void cancel() noexcept = 0;
};

/// Execution capabilities used by the toolkit's render policy. One scheduler
/// belongs to one TUI instance. post() must defer work to the next execution
/// turn; schedule() replaces its pending timer; cancel() cancels that timer.
/// schedule_interval() replaces its pending interval; cancel_interval() stops
/// it. An interval invokes its work every period until it is cancelled.
/// schedule_delayed() owns an independent one-shot timer and returns a handle
/// that cancels only that work without affecting render or interval timers.
class TuiScheduler {
public:
    virtual ~TuiScheduler() = default;

    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
    virtual void post(TuiScheduledWork work) = 0;
    virtual void schedule(std::chrono::steady_clock::duration delay, TuiScheduledWork work) = 0;
    [[nodiscard]] virtual std::unique_ptr<TuiScheduledHandle> schedule_delayed(
            std::chrono::steady_clock::duration delay, TuiScheduledWork work) = 0;
    virtual void cancel() noexcept = 0;
    virtual void schedule_interval(std::chrono::steady_clock::duration period, TuiScheduledWork work) = 0;
    virtual void cancel_interval() noexcept = 0;
};

} // namespace cch::tui
