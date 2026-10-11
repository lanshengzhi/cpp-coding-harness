#pragma once

#include <chrono>
#include <functional>

namespace cch::tui {

using TuiScheduledWork = std::move_only_function<void()>;

/// Execution capabilities used by the toolkit's render policy. One scheduler
/// belongs to one TUI instance. post() must defer work to the next execution
/// turn; schedule() replaces its pending timer; cancel() cancels that timer.
class TuiScheduler {
public:
    virtual ~TuiScheduler() = default;

    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
    virtual void post(TuiScheduledWork work) = 0;
    virtual void schedule(std::chrono::steady_clock::duration delay, TuiScheduledWork work) = 0;
    virtual void cancel() noexcept = 0;
};

} // namespace cch::tui
