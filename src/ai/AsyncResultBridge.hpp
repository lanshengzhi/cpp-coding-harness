#pragma once

#include <cch/support/AsyncResult.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <expected>
#include <memory>
#include <utility>

namespace cch::ai::detail {

/// The executor of the coroutine currently consuming an `AsyncResult`. Set
/// during producer initiation so implementation producers that must run
/// coroutine work can co_spawn it onto the consuming serialized domain
/// (ADR 0040). Cleared when initiation returns; a producer that needs an
/// executor captures it into its own owned state.
inline thread_local boost::asio::any_io_executor t_initiating_executor;

/// Consume a move-only `AsyncResult` from within a Boost.Asio coroutine,
/// resuming on the coroutine's executor regardless of which thread the
/// producer completes on. Ready results complete inline; pending results
/// suspend and re-enter the serialized domain through `dispatch` (ADR 0040 /
/// #454).
template <typename T, typename E>
[[nodiscard]] boost::asio::awaitable<std::expected<T, E>> await_async_result(
    cch::support::AsyncResult<T, E> result) {
    auto executor = co_await boost::asio::this_coro::executor;
    auto initiate = [executor](auto&& handler, cch::support::AsyncResult<T, E> operation) {
        auto owner = std::make_shared<std::decay_t<decltype(handler)>>(
            std::forward<decltype(handler)>(handler));
        const auto previous_executor = t_initiating_executor;
        t_initiating_executor = executor;
        std::move(operation).start(
            [executor, owner](std::expected<T, E> outcome) noexcept {
                boost::asio::dispatch(
                    executor,
                    [owner, outcome = std::move(outcome)]() mutable noexcept {
                        std::move(*owner)(std::move(outcome));
                    });
            });
        t_initiating_executor = previous_executor;
    };
    co_return co_await boost::asio::async_initiate<
        void(std::expected<T, E>)>(
        initiate, boost::asio::use_awaitable, std::move(result));
}

} // namespace cch::ai::detail
