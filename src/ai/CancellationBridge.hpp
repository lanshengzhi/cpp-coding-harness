#pragma once

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::ai {

/// Bridges one caller `std::stop_token` to one Asio `cancellation_signal`:
/// when the token stops, an `emit(all)` is posted to the owning executor.
/// The stop callback captures only shared state and posts (never touching
/// coroutine-frame locals), so a cross-thread `request_stop` cannot race
/// frame destruction.
class CancellationSignalBridge {
public:
    CancellationSignalBridge(std::stop_token stop_token, boost::asio::any_io_executor executor)
        : signal_(std::make_shared<boost::asio::cancellation_signal>()),
          callback_(std::move(stop_token), SignalEmitter{std::move(executor), signal_}) {}

    CancellationSignalBridge(const CancellationSignalBridge&) = delete;
    CancellationSignalBridge& operator=(const CancellationSignalBridge&) = delete;

    /// Binds the signal slot to one completion token.
    template <typename CompletionToken> [[nodiscard]] auto bind(CompletionToken&& token) const {
        return boost::asio::bind_cancellation_slot(signal_->slot(), std::forward<CompletionToken>(token));
    }

    /// Shared ownership of the underlying signal, for handlers that outlive
    /// the owning coroutine frame (e.g. a timeout timer emitting into the
    /// same signal).
    [[nodiscard]] std::shared_ptr<boost::asio::cancellation_signal> signal_ptr() const { return signal_; }

private:
    struct SignalEmitter {
        boost::asio::any_io_executor executor;
        std::shared_ptr<boost::asio::cancellation_signal> signal;

        void operator()() const {
            boost::asio::post(executor, [signal = signal] { signal->emit(boost::asio::cancellation_type::all); });
        }
    };

    std::shared_ptr<boost::asio::cancellation_signal> signal_;
    std::stop_callback<SignalEmitter> callback_;
};

/// Sleeps on the current executor, waking early with a Cancelled error
/// carrying `cancel_message` when the stop token fires.
///
/// `steady_timer::async_wait` completes only with success or
/// `operation_aborted`, and the only cancellation source here is the
/// stop-token bridge — so an aborted wait always coincides with
/// `stop_requested()`, and no non-abort error state exists. The pre-unified
/// retry wait additionally mapped that unreachable non-abort state to a
/// "Retry wait failed" stream error; dropping it is an intentional,
/// semantics-preserving narrowing (adversarial-review finding, #714).
[[nodiscard]] inline boost::asio::awaitable<support::ExpectedVoid> interruptible_sleep(
        std::chrono::milliseconds duration, std::stop_token stop_token, std::string cancel_message) {
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, std::move(cancel_message)));
    }
    auto executor = co_await boost::asio::this_coro::executor;
    CancellationSignalBridge cancellation(stop_token, executor);
    boost::asio::steady_timer timer(executor, duration);
    boost::system::error_code error;
    co_await timer.async_wait(cancellation.bind(boost::asio::redirect_error(boost::asio::use_awaitable, error)));
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, std::move(cancel_message)));
    }
    co_return support::ExpectedVoid{};
}

} // namespace cch::ai
