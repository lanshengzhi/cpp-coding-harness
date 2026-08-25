#include "support/AsyncResultBridge.hpp"

#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <expected>
#include <optional>
#include <utility>

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#error "Async bridge tests must compile Boost.Asio with exceptions disabled"
#endif

namespace {

template <typename T> struct SpawnResult {
    bool completed{false};
    bool exception{false};
    std::optional<T> value;
};

template <typename T> SpawnResult<T> run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    SpawnResult<T> result;
    boost::asio::co_spawn(io, std::move(operation), [&result](auto exception, T value) noexcept {
        result.completed = true;
        if (exception) {
            result.exception = true;
            return;
        }
        result.value.emplace(std::move(value));
    });
    io.run();
    return result;
}

} // namespace

TEST_CASE("the no-exception AsyncResult bridge delivers a successful terminal outcome", "[support][bridge][issue482]") {
    auto operation = cch::support::detail::make_async_result(
            []() -> boost::asio::awaitable<cch::support::Expected<int>> { co_return 42; });

    const auto run = run_awaitable(cch::support::detail::await_async_result(std::move(operation)));

    REQUIRE(run.completed);
    CHECK_FALSE(run.exception);
    REQUIRE(run.value.has_value());
    REQUIRE(run.value->has_value());
    CHECK(run.value->value() == 42);
}

TEST_CASE("the no-exception AsyncResult bridge preserves an explicit terminal error", "[support][bridge][issue482]") {
    auto operation =
            cch::support::detail::make_async_result([]() -> boost::asio::awaitable<cch::support::Expected<int>> {
                co_return std::unexpected(
                        cch::support::make_error(cch::support::ErrorCode::Cancelled, "cancelled by the producer"));
            });

    const auto run = run_awaitable(cch::support::detail::await_async_result(std::move(operation)));

    REQUIRE(run.completed);
    CHECK_FALSE(run.exception);
    REQUIRE(run.value.has_value());
    REQUIRE_FALSE(run.value->has_value());
    CHECK(run.value->error().code == cch::support::ErrorCode::Cancelled);
    CHECK(run.value->error().message == "cancelled by the producer");
}
