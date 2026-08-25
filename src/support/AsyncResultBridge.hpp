#pragma once

#include <cch/support/AsyncResult.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <expected>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace cch::support::detail {

template <typename T> struct AwaitableTerminal;

template <typename T, typename Executor> struct AwaitableTerminal<boost::asio::awaitable<T, Executor>> {
    using type = T;
};

template <typename AwaitableFactory, typename Terminal>
[[nodiscard]] boost::asio::awaitable<Terminal> invoke_awaitable(std::shared_ptr<AwaitableFactory> shared) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
        co_return co_await (*shared)();
    } catch (const std::exception& error) {
        co_return std::unexpected(
                cch::support::make_error(cch::support::ErrorCode::Unknown, "async operation failed", error.what()));
    } catch (...) {
        co_return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Unknown, "async operation failed"));
    }
#else
    co_return co_await (*shared)();
#endif
}

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
[[nodiscard]] boost::asio::awaitable<std::expected<T, E>> await_async_result(cch::support::AsyncResult<T, E> result) {
    auto executor = co_await boost::asio::this_coro::executor;
    auto initiate = [executor](auto&& handler, cch::support::AsyncResult<T, E> operation) {
        auto owner = std::make_shared<std::decay_t<decltype(handler)>>(std::forward<decltype(handler)>(handler));
        auto initiating = std::make_shared<std::atomic<bool>>(true);
        const auto previous_executor = t_initiating_executor;
        t_initiating_executor = executor;
        std::move(operation).start([executor, owner, initiating](std::expected<T, E> outcome) noexcept {
            if (initiating->load(std::memory_order_acquire)) {
                std::move (*owner)(std::move(outcome));
                return;
            }
            boost::asio::dispatch(executor, [owner, outcome = std::move(outcome)]() mutable noexcept {
                std::move (*owner)(std::move(outcome));
            });
        });
        initiating->store(false, std::memory_order_release);
        t_initiating_executor = previous_executor;
    };
    co_return co_await boost::asio::async_initiate<void(std::expected<T, E>)>(
            initiate, boost::asio::use_awaitable, std::move(result));
}

/// Wrap a one-shot `boost::asio::awaitable` producer into a move-only
/// `AsyncResult` (ADR 0040), running the wrapped coroutine on an explicit
/// executor (for example a private worker `io_context`). `make_awaitable` is
/// invoked exactly once at consumption and must return a fresh awaitable
/// whose terminal value is `std::expected<T, cch::support::Error>` (or
/// `std::expected<void, ...>`). The terminal outcome is delivered exactly
/// once through the move-only completion contract.
template <typename AwaitableFactory>
[[nodiscard]] auto make_async_result_on(boost::asio::any_io_executor executor, AwaitableFactory make_awaitable) {
    using Awaitable = std::invoke_result_t<AwaitableFactory&>;
    using Terminal = typename AwaitableTerminal<Awaitable>::type;
    using Value = typename Terminal::value_type;

    auto shared = std::make_shared<AwaitableFactory>(std::move(make_awaitable));
    return cch::support::AsyncResult<Value, cch::support::Error>{
            cch::support::AsyncProducer<Value, cch::support::Error>{
                    [executor, shared](
                            cch::support::AsyncCompletion<Value, cch::support::Error> completion) mutable noexcept {
                        using Completion = cch::support::AsyncCompletion<Value, cch::support::Error>;
                        std::shared_ptr<Completion> completion_owner;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        // The staged build still permits setup exceptions; convert them before
                        // the producer's no-exception completion contract takes over.
                        try {
#endif
                            completion_owner = std::make_shared<Completion>(std::move(completion));
                            boost::asio::co_spawn(executor,
                                    invoke_awaitable<AwaitableFactory, Terminal>(shared),
                                    boost::asio::bind_executor(executor,
                                            [shared, completion_owner](
                                                    std::exception_ptr exception, Terminal result) mutable noexcept {
                                                if (exception) {
#if defined(BOOST_ASIO_NO_EXCEPTIONS)
                                                    // Asio cannot produce an exception pointer in this mode. A
                                                    // non-null value is a Runtime invariant violation.
                                                    std::terminate();
#else
                                    // The staged exception-enabled build retains the same
                                    // explicit error channel without rethrowing across the bridge.
                                    std::move(*completion_owner)(std::unexpected(cch::support::make_error(
                                        cch::support::ErrorCode::Unknown,
                                        "async operation failed")));
#endif
                                                    return;
                                                }
                                                std::move (*completion_owner)(std::move(result));
                                            }));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                        } catch (...) {
                            auto failure = std::unexpected(cch::support::make_error(
                                    cch::support::ErrorCode::Unknown, "async operation initiation failed"));
                            if (completion_owner) {
                                std::move (*completion_owner)(std::move(failure));
                            } else {
                                completion(std::move(failure));
                            }
                        }
#endif
                    }}};
}

/// Wrap a one-shot `boost::asio::awaitable` producer into a move-only
/// `AsyncResult` (ADR 0040). `make_awaitable` is invoked exactly once at
/// consumption and must return a fresh awaitable whose terminal value is
/// `std::expected<T, cch::support::Error>` (or `std::expected<void, ...>`).
/// The producer reads the initiating executor captured by `await_async_result`
/// (or the ModelStream `consume` bridge) so the wrapped coroutine runs in the
/// consuming serialized domain; the terminal outcome is delivered exactly once.
template <typename AwaitableFactory> [[nodiscard]] auto make_async_result(AwaitableFactory make_awaitable) {
    using Awaitable = std::invoke_result_t<AwaitableFactory&>;
    using Terminal = typename AwaitableTerminal<Awaitable>::type;
    using Value = typename Terminal::value_type;

    // Read the initiating executor inside the producer (at initiation, not at
    // construction): `await_async_result` / the ModelStream `consume` bridge
    // publish it right before the producer runs, so the wrapped coroutine runs
    // on the consuming serialized domain (ADR 0040).
    return cch::support::AsyncResult<Value, cch::support::Error>{
            cch::support::AsyncProducer<Value, cch::support::Error>{
                    [make_awaitable = std::move(make_awaitable)](
                            cch::support::AsyncCompletion<Value, cch::support::Error> completion) mutable noexcept {
                        auto executor = t_initiating_executor;
                        auto bridged = make_async_result_on(executor, std::move(make_awaitable));
                        std::move(bridged).start(std::move(completion));
                    }}};
}

} // namespace cch::support::detail
