#include "support/ExpectedMacros.hpp"
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <optional>
#include <utility>

using namespace cch;

namespace {

/// Minimal test-local eager coroutine that runs a `co_return`-shaped body to
/// completion and exposes its terminal `std::expected` value. It exercises the
/// `CCH_TRY` / `CCH_TRY_VOID` macros in a real coroutine without pulling
/// Boost.Asio into the support shard (ADR 0042: the shard links only
/// `cch_support` and must not depend on the private Boost completion bridge).
///
/// The initial suspension is `suspend_never` and every awaited `AsyncResult`
/// below is a ready value (no suspension), so the body runs synchronously up
/// to the always-suspended `final_suspend`; the driver then reads the promise
/// and the destructor destroys the frame.
template <typename T>
struct TestTask {
    struct promise_type {
        std::optional<T> value;

        TestTask get_return_object() {
            return TestTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T result) { value.emplace(std::move(result)); }
        void unhandled_exception() { std::terminate(); }
    };

    explicit TestTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
    // §7.3 owning-type order: move constructor/assignment, destructor, then
    // deleted copy operations.
    TestTask(TestTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    TestTask& operator=(TestTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~TestTask() {
        if (handle_) handle_.destroy();
    }
    TestTask(const TestTask&) = delete;
    TestTask& operator=(const TestTask&) = delete;

    std::coroutine_handle<promise_type> handle_;
};

template <typename T>
[[nodiscard]] T run_async(TestTask<T> task) {
    REQUIRE(task.handle_.promise().value.has_value());
    return std::move(*task.handle_.promise().value);
}

support::AsyncResult<int> success_int() {
    return support::AsyncResult<int>{42};
}

support::AsyncResult<int> failure_int() {
    return support::AsyncResult<int>{
        std::unexpected(support::make_error(support::ErrorCode::Validation, "int failure"))};
}

support::AsyncResult<void> success_void() {
    return support::AsyncResult<void>{support::ExpectedVoid{}};
}

support::AsyncResult<void> failure_void() {
    return support::AsyncResult<void>{
        std::unexpected(support::make_error(support::ErrorCode::Validation, "void failure"))};
}

TestTask<support::Expected<int>> try_success() {
    CCH_TRY(value, co_await success_int());
    co_return value * 2;
}

TestTask<support::Expected<int>> try_failure() {
    CCH_TRY(value, co_await failure_int());
    co_return value * 2;
}

TestTask<support::Expected<int>> try_void_success() {
    CCH_TRY_VOID(co_await success_void());
    co_return 100;
}

TestTask<support::Expected<int>> try_void_failure() {
    CCH_TRY_VOID(co_await failure_void());
    co_return 100;
}

TestTask<support::Expected<int>> multiple_try_same_scope() {
    CCH_TRY(a, co_await success_int());
    CCH_TRY(b, co_await success_int());
    co_return a + b;
}

TestTask<support::Expected<int>> try_moved_value() {
    CCH_TRY(value, co_await success_int());
    auto moved = std::move(value);
    co_return moved * 2;
}

} // namespace

TEST_CASE("CCH_TRY unwraps successful expected and continues", "[support][expected][macros]") {
    auto result = run_async(try_success());
    REQUIRE(result);
    CHECK(*result == 84);
}

TEST_CASE("CCH_TRY propagates failed expected and short-circuits", "[support][expected][macros]") {
    auto result = run_async(try_failure());
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "int failure");
}

TEST_CASE("CCH_TRY_VOID continues on successful expected void", "[support][expected][macros]") {
    auto result = run_async(try_void_success());
    REQUIRE(result);
    CHECK(*result == 100);
}

TEST_CASE("CCH_TRY_VOID propagates failed expected void", "[support][expected][macros]") {
    auto result = run_async(try_void_failure());
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "void failure");
}

TEST_CASE("multiple CCH_TRY in same scope compile without collision", "[support][expected][macros]") {
    auto result = run_async(multiple_try_same_scope());
    REQUIRE(result);
    CHECK(*result == 84);
}

TEST_CASE("CCH_TRY moved value does not collide with subsequent uses", "[support][expected][macros]") {
    auto result = run_async(try_moved_value());
    REQUIRE(result);
    CHECK(*result == 84);
}
