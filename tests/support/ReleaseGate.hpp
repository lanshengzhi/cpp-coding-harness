#pragma once

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <system_error>

namespace cch::tests {

/// Counted, latched-release gate for hold-and-release test doubles
/// (CODING_STANDARDS.md §11.8).
///
/// release() records one permit instead of cancelling whatever timer happens
/// to exist: a release that arrives before the double arms the gate is stored
/// and consumed by the next wait, so release/arm ordering cannot hang a test
/// (the lost-cancel race that made the issue-87/88 User Bash overlap tests
/// flaky under Release and parallel load). One permit releases one waiter, so
/// later operations still gate until their own release — the re-arm behavior
/// tests rely on to observe intermediate states is unchanged.
///
/// interrupt() wakes the current waiter without recording a permit (stop-token
/// cancellation): the waiter re-checks its own cancellation state. An
/// interrupt that arrives before arming is the caller's responsibility —
/// guard wait() with a stop_requested() check, as the doubles do.
///
/// Same threading contract as the doubles it serves: the test thread calls
/// release()/interrupt() while it pumps the coroutine's io_context; the gate
/// is not a cross-thread synchronization primitive.
class ReleaseGate final {
public:
    ReleaseGate() = default;
    ReleaseGate(const ReleaseGate&) = delete;
    ReleaseGate& operator=(const ReleaseGate&) = delete;
    ~ReleaseGate() = default;

    /// Record one release permit and wake the current waiter, if any.
    void release() {
        ++permits_;
        if (timer_) {
            (void)timer_->cancel();
        }
    }

    /// Wake the current waiter without recording a permit.
    void interrupt() {
        if (timer_) {
            (void)timer_->cancel();
        }
    }

    /// Return immediately when an unconsumed release exists; otherwise wait
    /// until release() or interrupt().
    boost::asio::awaitable<void> wait() {
        if (permits_ > 0) {
            --permits_;
            co_return;
        }
        const auto executor = co_await boost::asio::this_coro::executor;
        timer_.emplace(executor);
        timer_->expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code error;
        co_await timer_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        // The timer is bound to the waiting coroutine's executor; release it
        // before that io_context dies so the gate can outlive the run.
        timer_.reset();
        if (permits_ > 0) {
            --permits_;
        }
    }

private:
    std::size_t permits_{0};
    std::optional<boost::asio::steady_timer> timer_;
};

} // namespace cch::tests
