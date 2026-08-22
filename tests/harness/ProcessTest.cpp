#include "agent/harness/Process.hpp"

#include "support/ProcessProbe.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>

#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;

namespace {

template <typename T, typename Start>
support::Expected<T> run_awaitable(Start start) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io,
        [start = std::move(start)]() mutable -> boost::asio::awaitable<support::Expected<T>> {
            co_return co_await start();
        },
        boost::asio::use_future);
    io.run();
    return future.get();
}

} // namespace

TEST_CASE("process exit probe treats an unreaped zombie as exited", "[harness][process][issue524]") {
    // The Arch pinned CI container's PID 1 (`tail -f /dev/null`) never reaps,
    // so a terminated descendant stays visible as a zombie and kill(pid, 0)
    // keeps succeeding after the exit (#524). Model that corpse here by killing
    // a child while this test deliberately declines to wait for it.
    const pid_t victim = ::fork();
    REQUIRE(victim != -1);
    if (victim == 0) {
        ::pause();
        _exit(0);
    }
    REQUIRE(::kill(victim, SIGKILL) == 0);

    // The kernel recorded the exit while the PID table still lists the zombie:
    // exactly what the old bare-kill assertion measured on the container, so
    // the honest probe must accept it.
    CHECK(tests::await_process_exit(std::to_string(victim)));
    CHECK(tests::process_state(victim) == 'Z');
    CHECK(::kill(victim, 0) == 0);

    // Reap so the corpse does not outlive the test.
    int status = 0;
    REQUIRE(::waitpid(victim, &status, 0) == victim);
}

TEST_CASE("process runner drains both pipes beyond kernel buffer capacity", "[harness][process][issue458]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    // 256 KiB written concurrently to each stream exceeds the kernel pipe
    // buffer, so either pipe must be drained while the child is still running.
    request.arguments = {"-c", R"((head -c 262144 /dev/zero | tr '\0' 'A') &
(head -c 262144 /dev/zero | tr '\0' 'B' >&2) &
wait)"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{2000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 1000000};

    std::size_t stdout_bytes = 0;
    std::size_t stderr_bytes = 0;
    // The captures remain alive until run_awaitable drives the process to quiescence.
    request.on_stdout = [&](std::string_view chunk) -> support::ExpectedVoid {
        stdout_bytes += chunk.size();
        return {};
    };
    request.on_stderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        stderr_bytes += chunk.size();
        return {};
    };

    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK_FALSE(result->timed_out);
    CHECK(result->exit_code == 0);
    CHECK(result->stdout_truncated);
    CHECK(result->stderr_truncated);
    // Both streams were drained to EOF rather than abandoned at capacity.
    CHECK(stdout_bytes == 262144);
    CHECK(stderr_bytes == 262144);
    CHECK(result->stdout_output.size() <= 1024);
    CHECK(result->stderr_output.size() <= 1024);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
}

TEST_CASE("process runner contains a failing callback and keeps both pipes draining", "[harness][process][issue458]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", R"((head -c 262144 /dev/zero | tr '\0' 'A') &
(head -c 65536 /dev/zero | tr '\0' 'B' >&2) &
wait)"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{5000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 1000000};

    int stdout_calls = 0;
    std::size_t stderr_bytes = 0;
    // The captures remain alive until run_awaitable drives the process to quiescence.
    request.on_stdout = [&](std::string_view) -> support::ExpectedVoid {
        ++stdout_calls;
        return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "consumer failed"));
    };
    request.on_stderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        stderr_bytes += chunk.size();
        return {};
    };

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Process);
    // The failing callback is deactivated after its first error, and the
    // unaffected stderr stream still drains to EOF without a timeout.
    CHECK(stdout_calls == 1);
    CHECK(stderr_bytes == 65536);
    CHECK(elapsed < std::chrono::seconds{3});
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
TEST_CASE("process runner contains a throwing callback and keeps both pipes draining", "[harness][process][issue484]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", R"((head -c 262144 /dev/zero | tr '\0' 'A') &
(head -c 65536 /dev/zero | tr '\0' 'B' >&2) &
wait)"};
    std::error_code current_path_error;
    request.working_directory = std::filesystem::current_path(current_path_error);
    REQUIRE_FALSE(current_path_error);
    request.timeout = std::chrono::milliseconds{5000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 1000000};

    std::size_t stderr_bytes = 0;
    request.on_stdout = [](std::string_view) -> support::ExpectedVoid {
        throw 1;
    };
    // The capture remains alive until run_awaitable drives the process to quiescence.
    request.on_stderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        stderr_bytes += chunk.size();
        return {};
    };

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Process);
    CHECK(result.error().message.find("callback") != std::string::npos);
    CHECK(stderr_bytes == 65536);
    CHECK(elapsed < std::chrono::seconds{3});
}
#endif

TEST_CASE("process runner timeout includes inherited output pipes after child exit", "[harness][process][issue484]") {
    tests::TempWorkspace workspace;
    const auto descendant_file = (workspace.path() / "descendant.pid").string();

    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", std::format(
        "sleep 30 & echo $! > '{}'; printf done; exit 0",
        descendant_file)};
    std::error_code current_path_error;
    request.working_directory = std::filesystem::current_path(current_path_error);
    REQUIRE_FALSE(current_path_error);
    request.timeout = std::chrono::milliseconds{500};

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result);
    CHECK(result->timed_out);
    CHECK(elapsed < std::chrono::seconds{5});
    CHECK(tests::await_process_exit(workspace.read("descendant.pid")));
}

TEST_CASE("process runner caps newline-free stderr output without stopping the drain", "[harness][process][issue458]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", R"(head -c 60000 /dev/zero | tr '\0' 'x' >&2)"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{5000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 2000};

    std::size_t stderr_bytes = 0;
    // The capture remains alive until run_awaitable drives the process to quiescence.
    request.on_stderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        stderr_bytes += chunk.size();
        return {};
    };

    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK_FALSE(result->timed_out);
    CHECK(result->stderr_truncated);
    CHECK(result->stderr_output.size() <= 1024);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
    CHECK(stderr_bytes == 60000);
}

TEST_CASE("process runner truncates at the line limit and keeps draining to EOF", "[harness][process][issue458]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", R"(for ((i=1; i<=5000; i++)); do echo "line-$i"; done)"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{5000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024 * 1024, .max_lines = 100};

    std::string streamed;
    // The capture remains alive until run_awaitable drives the process to quiescence.
    request.on_stdout = [&](std::string_view chunk) -> support::ExpectedVoid {
        streamed.append(chunk);
        return {};
    };

    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK_FALSE(result->timed_out);
    CHECK(result->stdout_truncated);
    // The live consumer saw the final line (drain reached EOF), while the
    // retained output stopped at the line budget.
    CHECK(streamed.find("line-5000") != std::string::npos);
    CHECK(result->stdout_output.find("line-5000") == std::string::npos);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
}

TEST_CASE("process runner timeout terminates and reaps the child", "[harness][process][issue458]") {
    tests::TempWorkspace workspace;
    const auto pid_file = (workspace.path() / "shell.pid").string();

    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", std::format("echo $$ > '{}'; sleep 30", pid_file)};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{500};

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result);
    CHECK(result->timed_out);
    CHECK(elapsed < std::chrono::seconds{5});
    // The leader's reaping is owned by the runner's waitpid, so demand full
    // removal from the PID table rather than zombie tolerance.
    CHECK(tests::await_process_reaped(workspace.read("shell.pid")));
}

TEST_CASE("process runner reports the terminating signal as exit code", "[harness][process][issue484]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    // The child terminates itself with SIGTERM; the exit code must report the
    // terminating signal (not 128 + signal), preserving the prior Boost.Process
    // `child::exit_code()` mapping.
    request.arguments = {"-c", "kill -TERM $$"};
    std::error_code current_path_error;
    request.working_directory = std::filesystem::current_path(current_path_error);
    REQUIRE_FALSE(current_path_error);
    request.timeout = std::chrono::milliseconds{2000};

    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK_FALSE(result->timed_out);
    CHECK(result->exit_code == SIGTERM);
}

TEST_CASE("process runner cancellation terminates and reaps the process group", "[harness][process][issue458]") {
    tests::TempWorkspace workspace;
    const auto pid_file = (workspace.path() / "shell.pid").string();
    const auto descendant_file = (workspace.path() / "descendant.pid").string();

    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c",
        std::format("echo $$ > '{}'; sleep 30 & echo $! > '{}'; wait", pid_file, descendant_file)};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::seconds{30};
    std::stop_source stop_source;
    request.stop_token = stop_source.get_token();

    boost::asio::io_context io;
    std::optional<support::Expected<harness::ProcessResult>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await runner.run(std::move(request));
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            boost::asio::steady_timer timer(
                co_await boost::asio::this_coro::executor,
                std::chrono::milliseconds{250});
            co_await timer.async_wait(boost::asio::use_awaitable);
            stop_source.request_stop();
            co_return;
        },
        boost::asio::detached);

    const auto started = std::chrono::steady_clock::now();
    io.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    CHECK((*result).error().code == support::ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::seconds{2});
    // The leader's reaping is owned by the runner's waitpid; only the orphaned
    // descendant depends on an external reaper.
    CHECK(tests::await_process_reaped(workspace.read("shell.pid")));
    CHECK(tests::await_process_exit(workspace.read("descendant.pid")));
}

TEST_CASE("process runner cancellation terminates descendants holding output pipes", "[harness][process][issue484]") {
    tests::TempWorkspace workspace;
    const auto descendant_file = (workspace.path() / "descendant.pid").string();

    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", std::format(
        "sleep 30 & echo $! > '{}'; printf done; exit 0",
        descendant_file)};
    std::error_code current_path_error;
    request.working_directory = std::filesystem::current_path(current_path_error);
    REQUIRE_FALSE(current_path_error);
    request.timeout = std::chrono::seconds{30};
    std::stop_source stop_source;
    request.stop_token = stop_source.get_token();

    boost::asio::io_context io;
    std::optional<support::Expected<harness::ProcessResult>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await runner.run(std::move(request));
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            boost::asio::steady_timer timer(
                co_await boost::asio::this_coro::executor,
                std::chrono::milliseconds{250});
            co_await timer.async_wait(boost::asio::use_awaitable);
            stop_source.request_stop();
            co_return;
        },
        boost::asio::detached);

    const auto started = std::chrono::steady_clock::now();
    io.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.has_value());
    REQUIRE_FALSE(*result);
    CHECK((*result).error().code == support::ErrorCode::Cancelled);
    CHECK(elapsed < std::chrono::seconds{3});
    CHECK(tests::await_process_exit(workspace.read("descendant.pid")));
}

TEST_CASE("process runner keeps stdout and stderr independent for mixed traffic", "[harness][process][issue458]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", R"(for ((i=1; i<=100; i++)); do echo "out-$i"; echo "err-$i" >&2; done)"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds{5000};
    request.output_limit = harness::OutputLimit{.max_bytes = 1024 * 1024, .max_lines = 100000};

    std::string streamed_stdout;
    std::string streamed_stderr;
    // The captures remain alive until run_awaitable drives the process to quiescence.
    request.on_stdout = [&](std::string_view chunk) -> support::ExpectedVoid {
        streamed_stdout.append(chunk);
        return {};
    };
    request.on_stderr = [&](std::string_view chunk) -> support::ExpectedVoid {
        streamed_stderr.append(chunk);
        return {};
    };

    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK_FALSE(result->timed_out);
    CHECK(result->exit_code == 0);
    CHECK_FALSE(result->stdout_truncated);
    CHECK_FALSE(result->stderr_truncated);
    CHECK(streamed_stdout.find("out-1") != std::string::npos);
    CHECK(streamed_stdout.find("err-") == std::string::npos);
    CHECK(streamed_stderr.find("err-100") != std::string::npos);
    CHECK(streamed_stderr.find("out-") == std::string::npos);
    CHECK(result->stdout_output == streamed_stdout);
    CHECK(result->stderr_output == streamed_stderr);
}
