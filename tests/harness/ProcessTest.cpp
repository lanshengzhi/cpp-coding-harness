#include "harness/Process.hpp"

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

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

#if defined(__unix__) || defined(__APPLE__)
bool process_gone(const std::string& pid_text) {
    pid_t pid{};
    const auto [end, ec] = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
    if (ec != std::errc{} || end == pid_text.data()) {
        return false;
    }
    errno = 0;
    return ::kill(pid, 0) == -1 && errno == ESRCH;
}
#endif

} // namespace

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
    request.on_stdout = [&](std::string_view chunk) { stdout_bytes += chunk.size(); };
    request.on_stderr = [&](std::string_view chunk) { stderr_bytes += chunk.size(); };

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

TEST_CASE("process runner contains a throwing callback and keeps both pipes draining", "[harness][process][issue458]") {
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
    request.on_stdout = [&](std::string_view) {
        ++stdout_calls;
        throw std::runtime_error("consumer failed");
    };
    request.on_stderr = [&](std::string_view chunk) { stderr_bytes += chunk.size(); };

    const auto started = std::chrono::steady_clock::now();
    auto result = run_awaitable<harness::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Process);
    // The throwing callback is deactivated after its first exception, and the
    // unaffected stderr stream still drains to EOF without a timeout.
    CHECK(stdout_calls == 1);
    CHECK(stderr_bytes == 65536);
    CHECK(elapsed < std::chrono::seconds{3});
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
    request.on_stderr = [&](std::string_view chunk) { stderr_bytes += chunk.size(); };

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
    request.on_stdout = [&](std::string_view chunk) { streamed.append(chunk); };

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
#if defined(__unix__) || defined(__APPLE__)
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
    CHECK(process_gone(workspace.read("shell.pid")));
#else
    SUCCEED("process timeout is covered on supported POSIX platforms");
#endif
}

TEST_CASE("process runner cancellation terminates and reaps the process group", "[harness][process][issue458]") {
#if defined(__unix__) || defined(__APPLE__)
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
    CHECK(process_gone(workspace.read("shell.pid")));
    CHECK(process_gone(workspace.read("descendant.pid")));
#else
    SUCCEED("process-group cancellation is covered on supported POSIX platforms");
#endif
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
    request.on_stdout = [&](std::string_view chunk) { streamed_stdout.append(chunk); };
    request.on_stderr = [&](std::string_view chunk) { streamed_stderr.append(chunk); };

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
