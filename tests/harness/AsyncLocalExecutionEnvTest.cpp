#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../src/harness/LocalExecutionEnv.hpp"
#include "../../src/util/Process.hpp"
#include "../support/TempWorkspace.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

class FakeProcessRunner final : public util::ProcessRunner {
public:
    boost::asio::awaitable<util::Expected<util::ProcessResult>> run(util::ProcessRequest request) override {
        requests.push_back(std::move(request));
        if (!error.empty()) {
            co_return std::unexpected(util::make_error(util::ErrorCode::Process, "fake process failed", error));
        }
        co_return next;
    }

    util::ProcessResult next;
    std::string error;
    std::vector<util::ProcessRequest> requests;
};

template <typename T, typename Start>
util::Expected<T> run_awaitable(Start start) {
    boost::asio::io_context io;
    std::optional<util::Expected<T>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await start();
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("async local execution env preserves file read and write safety", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto written = run_awaitable<harness::AsyncFileWriteResult>([&]() {
        return env.write_file("nested/note.txt", "hello", true);
    });
    REQUIRE(written);
    CHECK(written->bytes_written == 5);

    auto read = run_awaitable<harness::AsyncFileReadResult>([&]() {
        return env.read_file("nested/note.txt", 1, 0);
    });
    REQUIRE(read);
    CHECK(read->content == "hello");

    auto escaped = run_awaitable<harness::AsyncFileReadResult>([&]() {
        return env.read_file("../outside.txt", 1, 0);
    });
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == util::ErrorCode::Workspace);
}

TEST_CASE("async local execution env preserves exact edit ambiguity errors", "[harness][async][u6][ae4]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\nsame\n");
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto edited = run_awaitable<harness::AsyncFileEditResult>([&]() {
        return env.edit_file("note.txt", "same", "new");
    });

    REQUIRE_FALSE(edited);
    CHECK(edited.error().detail.find("multiple") != std::string::npos);
    CHECK(workspace.read("note.txt") == "same\nsame\n");
}

TEST_CASE("async local execution env rejects overlapping exact edit matches", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "aaa");
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto edited = run_awaitable<harness::AsyncFileEditResult>([&]() {
        return env.edit_file("note.txt", "aa", "b");
    });

    REQUIRE_FALSE(edited);
    CHECK(edited.error().detail.find("multiple") != std::string::npos);
    CHECK(workspace.read("note.txt") == "aaa");
}

TEST_CASE("async local execution env returns typed shell unavailable errors", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), false);

    auto shell = run_awaitable<harness::AsyncShellResult>([&]() {
        return env.run_shell("echo blocked", std::chrono::milliseconds(1000));
    });

    REQUIRE_FALSE(shell);
    CHECK(shell.error().code == util::ErrorCode::Process);
    CHECK(shell.error().detail.find("disabled") != std::string::npos);
}

TEST_CASE("async local execution env runs shell commands concurrently", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    boost::asio::io_context io;
    std::optional<util::Expected<harness::AsyncShellResult>> first;
    std::optional<util::Expected<harness::AsyncShellResult>> second;
    const auto started = std::chrono::steady_clock::now();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            first = co_await env.run_shell("sleep 0.6; echo first", std::chrono::milliseconds(3000));
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            second = co_await env.run_shell("sleep 0.6; echo second", std::chrono::milliseconds(3000));
            co_return;
        },
        boost::asio::detached);

    io.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first);
    REQUIRE(*second);
    CHECK((*first)->output.find("first") != std::string::npos);
    CHECK((*second)->output.find("second") != std::string::npos);
    CHECK(elapsed < std::chrono::milliseconds(1100));
}

TEST_CASE("async local execution env times out shell commands without blocking the io context", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);
    const auto started = std::chrono::steady_clock::now();

    auto shell = run_awaitable<harness::AsyncShellResult>([&]() {
        return env.run_shell("sleep 2", std::chrono::milliseconds(100));
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(shell);
    CHECK(shell->timed_out);
    CHECK(elapsed < std::chrono::milliseconds(1500));
}

TEST_CASE("async local execution env sanitizes shell environment through process capability", "[harness][async][u2]") {
#if defined(__unix__) || defined(__APPLE__)
    setenv("OPENAI_API_KEY", "sk-test-secret", 1);
    setenv("KIMI_API_KEY", "kimi-secret-value", 1);
    setenv("CCH_VISIBLE_ENV", "visible", 1);
    setenv("CCH_CREDENTIAL", "plain-secret-value", 1);
#endif
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeProcessRunner>();
    runner->next.exit_code = 0;
    runner->next.output = "ok";
    harness::LocalExecutionEnv env(workspace.path(), true, {"CCH_CREDENTIAL", "KIMI_API_KEY"}, runner);

    auto shell = env.run_shell("env", std::chrono::milliseconds(123));

    REQUIRE(shell);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].working_directory == workspace.path());
    CHECK(runner->requests[0].timeout.count() == 123);
    CHECK(runner->requests[0].use_explicit_environment);
    CHECK(runner->requests[0].environment.find("OPENAI_API_KEY") == runner->requests[0].environment.end());
    CHECK(runner->requests[0].environment.find("KIMI_API_KEY") == runner->requests[0].environment.end());
    CHECK(runner->requests[0].environment.find("CCH_CREDENTIAL") == runner->requests[0].environment.end());
#if defined(__unix__) || defined(__APPLE__)
    CHECK(runner->requests[0].environment.find("CCH_VISIBLE_ENV") != runner->requests[0].environment.end());
    unsetenv("OPENAI_API_KEY");
    unsetenv("KIMI_API_KEY");
    unsetenv("CCH_VISIBLE_ENV");
    unsetenv("CCH_CREDENTIAL");
#endif
}

TEST_CASE("default process runner caps newline-free output without waiting for line breaks", "[harness][async][process]") {
    util::DefaultProcessRunner runner;
    util::ProcessRequest request;
    request.command = "printf '%60000s' '' | tr ' ' x";
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds(5000);
    request.max_output_bytes = 1024;
    request.max_output_lines = 2000;

    auto result = run_awaitable<util::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK(result->output.size() <= 1100);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
}
