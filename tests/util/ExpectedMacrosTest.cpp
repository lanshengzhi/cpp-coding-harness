#include "util/ExpectedMacros.hpp"
#include <cch/util/Error.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace cch;

namespace {

boost::asio::awaitable<util::Expected<int>> success_int() {
    co_return 42;
}

boost::asio::awaitable<util::Expected<int>> failure_int() {
    co_return std::unexpected(util::make_error(util::ErrorCode::Validation, "int failure"));
}

boost::asio::awaitable<util::ExpectedVoid> success_void() {
    co_return util::ExpectedVoid{};
}

boost::asio::awaitable<util::ExpectedVoid> failure_void() {
    co_return std::unexpected(util::make_error(util::ErrorCode::Validation, "void failure"));
}

boost::asio::awaitable<util::Expected<int>> try_success() {
    CCH_TRY(value, co_await success_int());
    co_return value * 2;
}

boost::asio::awaitable<util::Expected<int>> try_failure() {
    CCH_TRY(value, co_await failure_int());
    co_return value * 2;
}

boost::asio::awaitable<util::Expected<int>> try_void_success() {
    CCH_TRY_VOID(co_await success_void());
    co_return 100;
}

boost::asio::awaitable<util::Expected<int>> try_void_failure() {
    CCH_TRY_VOID(co_await failure_void());
    co_return 100;
}

boost::asio::awaitable<util::Expected<int>> multiple_try_same_scope() {
    CCH_TRY(a, co_await success_int());
    CCH_TRY(b, co_await success_int());
    co_return a + b;
}

boost::asio::awaitable<util::Expected<int>> try_moved_value() {
    CCH_TRY(value, co_await success_int());
    auto moved = std::move(value);
    co_return moved * 2;
}

template <typename T>
T run_async(boost::asio::awaitable<T> task) {
    boost::asio::io_context io;
    std::optional<T> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await std::move(task);
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("CCH_TRY unwraps successful expected and continues", "[util][expected][macros]") {
    auto result = run_async(try_success());
    REQUIRE(result);
    CHECK(*result == 84);
}

TEST_CASE("CCH_TRY propagates failed expected and short-circuits", "[util][expected][macros]") {
    auto result = run_async(try_failure());
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "int failure");
}

TEST_CASE("CCH_TRY_VOID continues on successful expected void", "[util][expected][macros]") {
    auto result = run_async(try_void_success());
    REQUIRE(result);
    CHECK(*result == 100);
}

TEST_CASE("CCH_TRY_VOID propagates failed expected void", "[util][expected][macros]") {
    auto result = run_async(try_void_failure());
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "void failure");
}

TEST_CASE("multiple CCH_TRY in same scope compile without collision", "[util][expected][macros]") {
    auto result = run_async(multiple_try_same_scope());
    REQUIRE(result);
    CHECK(*result == 84);
}

TEST_CASE("CCH_TRY moved value does not collide with subsequent uses", "[util][expected][macros]") {
    auto result = run_async(try_moved_value());
    REQUIRE(result);
    CHECK(*result == 84);
}
