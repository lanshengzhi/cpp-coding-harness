#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include "support/AllocationCounter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using cch::support::AsyncCompletion;
using cch::support::AsyncProducer;
using cch::support::AsyncResult;
using cch::support::Error;
using cch::support::ErrorCode;
using cch::support::make_error;

namespace {

// A minimal coroutine task whose frame the test drives and destroys by hand.
// `initial_suspend`/`final_suspend` are both `suspend_always` so the test owns
// every resume and can abandon a suspended frame deterministically.
struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
};

Task co_await_into(
    AsyncResult<int> result,
    std::optional<std::expected<int, Error>>& slot) {
    slot = co_await std::move(result);
    co_return;
}

Task co_await_void_into(
    AsyncResult<void> result,
    std::optional<std::expected<void, Error>>& slot) {
    slot = co_await std::move(result);
    co_return;
}

#if defined(__unix__) || defined(__APPLE__)
/// Run `body` in a forked child with stderr silenced. Returns true iff the
/// child died from SIGABRT — the default `std::terminate` handler — so each
/// fatal-contract violation is proven in an isolated process.
template <typename F>
bool dies_from_abort(F&& body) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // The forked child inherits Catch2's fatal-signal handlers; restore
        // the default disposition so std::terminate (abort -> SIGABRT) kills
        // the child cleanly instead of being caught and longjmp'd.
        (void)std::signal(SIGABRT, SIG_DFL);
        (void)std::signal(SIGSEGV, SIG_DFL);
        (void)std::signal(SIGILL, SIG_DFL);
        (void)std::signal(SIGFPE, SIG_DFL);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)::dup2(devnull, STDERR_FILENO);
            (void)::close(devnull);
        }
        body();
        ::_exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return false;
    }
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

} // namespace

TEST_CASE("a ready AsyncResult delivers its value to a completion callback inline", "[support][issue451]") {
    AsyncResult<int> result(std::expected<int, Error>{42});
    int observed = 0;
    result.start([&observed](std::expected<int, Error> value) noexcept {
        observed = value.has_value() ? *value : 0;
    });
    CHECK(observed == 42);
}

TEST_CASE("a ready AsyncResult delivers a terminal error inline", "[support][issue451]") {
    AsyncResult<int> result(
        std::expected<int, Error>{std::unexpect, make_error(ErrorCode::Process, "boom")});
    std::optional<std::expected<int, Error>> outcome;
    result.start([&outcome](std::expected<int, Error> value) noexcept {
        outcome = std::move(value);
    });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::Process);
    CHECK(outcome->error().message == "boom");
}

TEST_CASE("the ready path completes without allocation", "[support][issue451]") {
    // Build the completion first so the measured region isolates AsyncResult's
    // own ready path, not the callback's move-only-function construction.
    int observed = 0;
    AsyncCompletion<int, Error> completion =
        [&observed](std::expected<int, Error> value) noexcept {
            observed = value.has_value() ? *value : 0;
        };

    cch::tests::reset_allocation_count();
    AsyncResult<int> result(std::expected<int, Error>{42});
    result.start(std::move(completion));
    const auto allocations = cch::tests::allocation_count();

    CHECK(allocations == 0);
    CHECK(observed == 42);
}

TEST_CASE("co_await on a ready AsyncResult completes without suspending", "[support][issue451]") {
    std::optional<std::expected<int, Error>> slot;
    auto task = co_await_into(AsyncResult<int>(std::expected<int, Error>{7}), slot);

    // One resume runs the whole body: the ready co_await does not suspend.
    task.handle.resume();
    CHECK(task.handle.done());

    REQUIRE(slot.has_value());
    REQUIRE(slot->has_value());
    CHECK(slot->value() == 7);
    task.handle.destroy();
}

TEST_CASE("a pending AsyncResult initiates its producer exactly once and delivers once", "[support][issue451]") {
    int initiations = 0;
    int observed = 0;
    AsyncCompletion<int, Error> held;

    AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
        ++initiations;
        held = std::move(done);
    });

    result.start([&observed](std::expected<int, Error> value) noexcept {
        observed = value.has_value() ? *value : 0;
    });

    CHECK(initiations == 1);
    REQUIRE(held);
    std::move(held)(std::expected<int, Error>{23});
    CHECK(observed == 23);
}

TEST_CASE("a pending operation owns its inputs after initiation returns", "[support][issue451]") {
    int observed = 0;
    AsyncCompletion<int, Error> held;

    // The producer owns `input` by move and carries it into the terminal
    // delivery, so after start() returns the operation borrows nothing.
    AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
        held = [input = std::make_unique<int>(42), done = std::move(done)](
                   std::expected<int, Error>) mutable noexcept {
            done(std::expected<int, Error>{*input});
        };
    });

    result.start([&observed](std::expected<int, Error> value) noexcept {
        observed = value.has_value() ? *value : 0;
    });

    REQUIRE(held);
    std::move(held)(std::expected<int, Error>{});
    CHECK(observed == 42);
}

TEST_CASE("co_await on a pending AsyncResult suspends and resumes with the terminal outcome", "[support][issue451]") {
    AsyncCompletion<int, Error> held;
    AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
        held = std::move(done);
    });

    std::optional<std::expected<int, Error>> slot;
    auto task = co_await_into(std::move(result), slot);

    task.handle.resume(); // suspends at the pending co_await
    CHECK_FALSE(task.handle.done());

    REQUIRE(held);
    std::move(held)(std::expected<int, Error>{31});
    REQUIRE(slot.has_value());
    REQUIRE(slot->has_value());
    CHECK(slot->value() == 31);
    CHECK(task.handle.done());
    task.handle.destroy();
}

TEST_CASE("co_await on a pending void AsyncResult resumes with the terminal outcome", "[support][issue451]") {
    AsyncCompletion<void, Error> held;
    AsyncResult<void> result([&](AsyncCompletion<void, Error> done) noexcept {
        held = std::move(done);
    });

    std::optional<std::expected<void, Error>> slot;
    auto task = co_await_void_into(std::move(result), slot);

    task.handle.resume();
    CHECK_FALSE(task.handle.done());

    REQUIRE(held);
    std::move(held)(std::expected<void, Error>{});
    REQUIRE(slot.has_value());
    CHECK(slot->has_value());
    CHECK(task.handle.done());
    task.handle.destroy();
}

TEST_CASE("an explicit stop token resolves cancellation into the terminal outcome", "[support][issue451]") {
    std::stop_source source;
    const auto token = source.get_token();

    // Cancellation is supplied explicitly (std::stop_token), never baked into
    // AsyncResult; the producer resolves it into its own terminal outcome.
    AsyncResult<int> result([token](AsyncCompletion<int, Error> done) noexcept {
        if (token.stop_requested()) {
            done(std::unexpected(make_error(ErrorCode::Cancelled, "cancelled")));
            return;
        }
        done(std::expected<int, Error>{42});
    });

    source.request_stop();

    std::optional<std::expected<int, Error>> outcome;
    result.start([&outcome](std::expected<int, Error> value) noexcept {
        outcome = std::move(value);
    });

    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->has_value());
    CHECK(outcome->error().code == ErrorCode::Cancelled);
}

TEST_CASE("abandoning a suspended co_await discards a late completion safely", "[support][issue451]") {
    AsyncCompletion<int, Error> held;
    AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
        held = std::move(done);
    });

    bool resumed = false;
    {
        std::optional<std::expected<int, Error>> slot;
        // A task whose body records whether it resumed past the co_await.
        auto task = [&](AsyncResult<int> r) -> Task {
            slot = co_await std::move(r);
            resumed = true;
            co_return;
        }(std::move(result));

        task.handle.resume(); // suspends at the pending co_await
        CHECK_FALSE(task.handle.done());
        REQUIRE(held);
        task.handle.destroy(); // abandon the frame
    }

    // A late first completion after established abandonment is discarded, never
    // resuming a dead frame.
    std::move(held)(std::expected<int, Error>{5});
    CHECK_FALSE(resumed);
}

TEST_CASE("a late completion after the result handle is gone still reaches its owned callback", "[support][issue451]") {
    AsyncCompletion<int, Error> held;
    int observed = 0;

    {
        AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
            held = std::move(done);
        });
        result.start([&observed](std::expected<int, Error> value) noexcept {
            observed = value.has_value() ? *value : 0;
        });
    } // the result handle is destroyed; the operation owns the completion

    REQUIRE(held);
    std::move(held)(std::expected<int, Error>{6});
    CHECK(observed == 6);
}

TEST_CASE("AsyncResult preserves typed outcomes with a custom error type", "[support][issue451]") {
    enum class MyError { Bad };
    AsyncResult<int, MyError> result(
        std::expected<int, MyError>{std::unexpect, MyError::Bad});

    int saw_error = 0;
    AsyncCompletion<int, MyError> completion =
        [&saw_error](std::expected<int, MyError> value) noexcept {
            if (!value.has_value()) {
                saw_error = 1;
            }
        };

    result.start(std::move(completion));
    CHECK(saw_error == 1);
}

TEST_CASE("AsyncResult is move-only and names the shared error default", "[support][issue451]") {
    static_assert(!std::is_copy_constructible_v<AsyncResult<int>>);
    static_assert(!std::is_copy_assignable_v<AsyncResult<int>>);
    static_assert(std::is_move_constructible_v<AsyncResult<int>>);
    static_assert(std::is_move_assignable_v<AsyncResult<int>>);

    static_assert(std::is_same_v<
                  typename AsyncResult<int>::completion_type,
                  AsyncCompletion<int, Error>>);
    static_assert(std::is_same_v<
                  typename AsyncResult<int>::producer_type,
                  AsyncProducer<int, Error>>);
}

TEST_CASE("AsyncResult exposes no executor, event bus, polymorphic box, or Boost.Asio", "[support][issue451]") {
    const auto path = std::filesystem::path(CCH_SOURCE_DIR) /
        "src" / "support" / "include" / "cch" / "support" / "AsyncResult.hpp";
    REQUIRE(std::filesystem::exists(path));

    std::ifstream input(path);
    std::ostringstream output;
    output << input.rdbuf();
    const auto text = output.str();

    CHECK(text.find("boost/") == std::string::npos);
    CHECK(text.find("boost::") == std::string::npos);
    CHECK(text.find("asio") == std::string::npos);
    CHECK(text.find("executor") == std::string::npos);
    CHECK(text.find("scheduler") == std::string::npos);
    CHECK(text.find("event_bus") == std::string::npos);
    CHECK(text.find("virtual") == std::string::npos);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("starting an AsyncResult twice terminates the process", "[support][fatal][issue451]") {
    const bool aborted = dies_from_abort([] {
        AsyncResult<int> result(std::expected<int, Error>{1});
        result.start([](std::expected<int, Error>) noexcept {});
        result.start([](std::expected<int, Error>) noexcept {});
    });
    CHECK(aborted);
}

TEST_CASE("starting a moved-from AsyncResult terminates the process", "[support][fatal][issue451]") {
    const bool aborted = dies_from_abort([] {
        AsyncResult<int> source(std::expected<int, Error>{1});
        AsyncResult<int> moved(std::move(source));
        source.start([](std::expected<int, Error>) noexcept {});
    });
    CHECK(aborted);
}

TEST_CASE("a producer that completes twice terminates the process", "[support][fatal][issue451]") {
    const bool aborted = dies_from_abort([] {
        AsyncResult<int> result([&](AsyncCompletion<int, Error> done) noexcept {
            done(std::expected<int, Error>{1});
            done(std::expected<int, Error>{2}); // at-most-once violation
        });
        result.start([](std::expected<int, Error>) noexcept {});
    });
    CHECK(aborted);
}

TEST_CASE("an empty producer terminates the process", "[support][fatal][issue451]") {
    const bool aborted = dies_from_abort([] {
        AsyncResult<int> result(AsyncProducer<int, Error>{});
        result.start([](std::expected<int, Error>) noexcept {});
    });
    CHECK(aborted);
}
#endif
