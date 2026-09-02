#pragma once

#include "support/RuntimeFixture.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace cch::tests {

/// Drive a RuntimeFixture's interaction loop on a helper thread while a test
/// exercises a Session through another synchronous or asynchronous seam. The
/// fixture remains the owner of the loop and RuntimeRoot; this driver only
/// keeps the loop serviced until it is destroyed.
class RuntimeLoopDriver final {
public:
    explicit RuntimeLoopDriver(RuntimeFixture& runtime) : runtime_(&runtime), thread_([this] { drive(); }) {
        while (!started_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    ~RuntimeLoopDriver() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RuntimeLoopDriver(const RuntimeLoopDriver&) = delete;
    RuntimeLoopDriver& operator=(const RuntimeLoopDriver&) = delete;
    RuntimeLoopDriver(RuntimeLoopDriver&&) = delete;
    RuntimeLoopDriver& operator=(RuntimeLoopDriver&&) = delete;

private:
    void drive() noexcept {
        started_.store(true, std::memory_order_release);
        const auto result = runtime_->run(
                support::detail::make_async_result([this]() -> boost::asio::awaitable<support::ExpectedVoid> {
                    while (!stop_.load(std::memory_order_acquire)) {
                        boost::asio::steady_timer timer(
                                co_await boost::asio::this_coro::executor, std::chrono::milliseconds{1});
                        boost::system::error_code error;
                        co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                    }
                    co_return support::ExpectedVoid{};
                }));
        completed_.store(result.has_value(), std::memory_order_release);
    }

    RuntimeFixture* runtime_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> completed_{false};
    std::thread thread_;
};

} // namespace cch::tests
