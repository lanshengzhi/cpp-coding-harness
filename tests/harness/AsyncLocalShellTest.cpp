#include "support/EnvVarGuard.hpp"
#include "support/ProcessProbe.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/FileSystem.hpp>
#include <cch/agent/harness/LocalShell.hpp>
#include "agent/harness/ShellResolver.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "support/AsyncResultBridge.hpp"
#include "agent/harness/OutputLimiter.hpp"
#include "agent/harness/Process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>
#include <thread>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>

using namespace cch;

namespace {

class FakeAsyncProcessRunner final : public harness::AsyncProcessRunner {
public:
    boost::asio::awaitable<support::Expected<harness::ProcessResult>> run(harness::ProcessRequest request) override {
        requests.push_back(std::move(request));
        if (!error.empty()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Process, "fake process failed", error));
        }
        co_return next;
    }

    harness::ProcessResult next;
    std::string error;
    std::vector<harness::ProcessRequest> requests;
};

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
class ThrowingAsyncProcessRunner final : public harness::AsyncProcessRunner {
public:
    boost::asio::awaitable<support::Expected<harness::ProcessResult>> run(harness::ProcessRequest) override {
        throw std::runtime_error{"output callback failed"};
        co_return harness::ProcessResult{};
    }
};
#endif

template <typename T, typename Start> support::Expected<T> run_awaitable(Start start) {
    boost::asio::io_context io;
    std::optional<support::Expected<T>> result;
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

class TestRuntime final {
public:
    TestRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()), work_guard_(boost::asio::make_work_guard(*loop_)),
          root_(loop_, harness::RuntimeLimits{}), loop_thread_([this] { loop_->run(); }) {}

    ~TestRuntime() {
        work_guard_.reset();
        loop_->stop();
    }

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() { return root_.make_target(); }

private:
    std::shared_ptr<boost::asio::io_context> loop_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    harness::RuntimeRoot root_;
    std::jthread loop_thread_;
};

[[nodiscard]] std::shared_ptr<harness::RuntimeTarget> test_runtime_target() {
    static TestRuntime runtime;
    return runtime.make_target();
}

template <typename T, typename E> std::expected<T, E> run_awaitable_pi(support::AsyncResult<T, E> operation) {
    boost::asio::io_context io;
    std::optional<std::expected<T, E>> result;
    boost::asio::co_spawn(
            io,
            [operation = std::move(operation), &result]() mutable -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(std::move(operation));
                co_return;
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    CHECK_FALSE(error);
    return exists;
}

} // namespace

TEST_CASE("process runner capability follows async naming and ownership rules", "[harness][async][issue75]") {
    static_assert(std::is_abstract_v<harness::AsyncProcessRunner>);
    static_assert(std::is_final_v<harness::DefaultAsyncProcessRunner>);
}

TEST_CASE("async local shell runs shell commands concurrently", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    boost::asio::io_context io;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> first;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> second;
    const auto started = std::chrono::steady_clock::now();

    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                first = co_await support::detail::await_async_result(shell.exec("sleep 0.6; echo first"));
                co_return;
            },
            boost::asio::detached);
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                second = co_await support::detail::await_async_result(shell.exec("sleep 0.6; echo second"));
                co_return;
            },
            boost::asio::detached);

    io.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->has_value());
    REQUIRE(second->has_value());
    CHECK((*first)->stdout_output.find("first") != std::string::npos);
    CHECK((*second)->stdout_output.find("second") != std::string::npos);
    CHECK(elapsed < std::chrono::milliseconds(1100));
}

TEST_CASE("Shell adapter expands a custom path and joins a non-empty prefix at launch", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard home_guard{"HOME", workspace.path().string()};
    workspace.write("bin/custom-shell", "#!/bin/sh\nexit 0\n");
    REQUIRE(::chmod((workspace.path() / "bin/custom-shell").c_str(), 0700) == 0);
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    harness::ShellConfig shell_config{
            .shell_path = "~/bin/custom-shell",
            .command_prefix = "export CCH_READY=1",
    };
    harness::AsyncLocalShell shell(
            test_runtime_target(), workspace.path(), true, {"HOME"}, std::move(shell_config), runner);

    auto result = run_awaitable_pi(shell.exec("printf '%s' \"$CCH_READY\""));

    REQUIRE(result);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].executable == workspace.path() / "bin/custom-shell");
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[0] == "-c");
    CHECK(runner->requests[0].arguments[1] == "export CCH_READY=1\nprintf '%s' \"$CCH_READY\"");
    CHECK(runner->requests[0].working_directory == workspace.path());
}

TEST_CASE("Shell resolution expands home when HOME is absent from the environment", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard home_guard{"HOME", std::nullopt};
    const auto* user = ::getpwuid(::getuid());
    REQUIRE(user != nullptr);
    REQUIRE(user->pw_dir != nullptr);
    const std::string missing_name = "cch-issue84-missing-shell";

    const auto resolved = harness::resolve_shell_executable("~/" + missing_name, workspace.path(), {});

    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().code == harness::ExecutionErrorCode::ShellUnavailable);
    CHECK(resolved.error().message.find((std::filesystem::path{user->pw_dir} / missing_name).string()) !=
            std::string::npos);
    CHECK(resolved.error().message.find((workspace.path() / "~").string()) == std::string::npos);
}

TEST_CASE("Shell adapter defaults to non-login system bash", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true, {}, {}, runner);

    auto result = run_awaitable_pi(shell.exec("printf default"));

    REQUIRE(result);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].executable == "/bin/bash");
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[0] == "-c");
    CHECK(runner->requests[0].arguments[1] == "printf default");
}

TEST_CASE("Shell adapter ignores an empty command prefix", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    harness::ShellConfig shell_config{.command_prefix = ""};
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true, {}, std::move(shell_config), runner);

    auto result = run_awaitable_pi(shell.exec("printf plain"));

    REQUIRE(result);
    REQUIRE(runner->requests.size() == 1);
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[1] == "printf plain");
}

TEST_CASE("Shell resolution prefers PATH bash before sh when system bash is unavailable", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    workspace.write("bin/bash", "#!/bin/sh\nexit 0\n");
    workspace.write("bin/sh", "#!/bin/sh\nexit 0\n");
    REQUIRE(::chmod((workspace.path() / "bin/bash").c_str(), 0700) == 0);
    REQUIRE(::chmod((workspace.path() / "bin/sh").c_str(), 0700) == 0);
    const std::map<std::string, std::string> environment{
            {"PATH", (workspace.path() / "bin").string()},
    };

    const auto resolved = harness::resolve_shell_executable(
            std::nullopt, workspace.path(), environment, workspace.path() / "missing-system-bash");

    REQUIRE(resolved);
    CHECK(*resolved == workspace.path() / "bin/bash");
}

TEST_CASE("Shell resolution falls back to PATH sh", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    workspace.write("bin/sh", "#!/bin/sh\nexit 0\n");
    REQUIRE(::chmod((workspace.path() / "bin/sh").c_str(), 0700) == 0);
    const std::map<std::string, std::string> environment{
            {"PATH", (workspace.path() / "bin").string()},
    };

    const auto resolved = harness::resolve_shell_executable(
            std::nullopt, workspace.path(), environment, workspace.path() / "missing-system-bash");

    REQUIRE(resolved);
    CHECK(*resolved == workspace.path() / "bin/sh");
}

TEST_CASE("a stale configured Shell path fails only attempted execution", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    harness::ShellConfig shell_config{
            .shell_path = (workspace.path() / "stale-shell").string(),
            .command_prefix = "prefix-must-not-appear-in-diagnostics",
    };
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true, {}, std::move(shell_config));

    const auto result = run_awaitable_pi(shell.exec("printf command"));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::ShellUnavailable);
    CHECK(result.error().message.find("stale-shell") != std::string::npos);
    CHECK(result.error().message.find("prefix-must-not-appear") == std::string::npos);
}

TEST_CASE("async local shell sanitizes shell environment through process capability", "[harness][async][u2]") {
    setenv("OPENAI_API_KEY", "sk-test-secret", 1);
    setenv("KIMI_API_KEY", "kimi-secret-value", 1);
    setenv("CCH_VISIBLE_ENV", "visible", 1);
    setenv("CCH_CREDENTIAL", "plain-secret-value", 1);
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    runner->next.stdout_output = "ok";
    harness::AsyncLocalShell shell(
            test_runtime_target(), workspace.path(), true, {"CCH_CREDENTIAL", "KIMI_API_KEY"}, {}, runner);

    std::stop_source stop_source;
    harness::ExecOptions opts;
    opts.timeout = std::chrono::milliseconds(123);
    opts.stop_token = stop_source.get_token();
    auto result = run_awaitable_pi(shell.exec("env", std::move(opts)));

    REQUIRE(result);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].working_directory == workspace.path());
    CHECK(runner->requests[0].timeout.count() == 123);
    CHECK(runner->requests[0].stop_token == stop_source.get_token());
    CHECK(runner->requests[0].use_explicit_environment);
    CHECK(runner->requests[0].environment.find("OPENAI_API_KEY") == runner->requests[0].environment.end());
    CHECK(runner->requests[0].environment.find("KIMI_API_KEY") == runner->requests[0].environment.end());
    CHECK(runner->requests[0].environment.find("CCH_CREDENTIAL") == runner->requests[0].environment.end());
    CHECK(runner->requests[0].environment.find("CCH_VISIBLE_ENV") != runner->requests[0].environment.end());
    unsetenv("OPENAI_API_KEY");
    unsetenv("KIMI_API_KEY");
    unsetenv("CCH_VISIBLE_ENV");
    unsetenv("CCH_CREDENTIAL");
}

TEST_CASE("pi-shaped exec rejects a pre-cancelled request before spawning", "[harness][async][issue40]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);
    std::stop_source stop_source;
    stop_source.request_stop();
    harness::ExecOptions options;
    options.stop_token = stop_source.get_token();

    auto result = run_awaitable_pi(shell.exec("touch should-not-exist", std::move(options)));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::Aborted);
    CHECK_FALSE(path_exists(workspace.path() / "should-not-exist"));
}

TEST_CASE("pi-shaped exec classifies an explicit output callback failure", "[harness][async][process][issue484]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);
    harness::ExecOptions options;
    options.onStdout = [](std::string_view) -> support::ExpectedVoid {
        return std::unexpected(support::make_error(support::ErrorCode::Process, "output sink failed"));
    };

    auto result = run_awaitable_pi(shell.exec("printf stdout; printf stderr >&2", std::move(options)));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::CallbackError);
    CHECK(result.error().message == "output sink failed");
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
TEST_CASE("local shell adapter classifies an exceptional process completion", "[harness][shell][process][issue484]") {
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<ThrowingAsyncProcessRunner>();
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true, {}, {}, runner);

    auto result = run_awaitable_pi(shell.exec("printf output"));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::CallbackError);
    CHECK(result.error().message == "output callback failed");
}
#endif

TEST_CASE("cancelling exec terminates the process group and reaps the shell", "[harness][async][process][issue40]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);
    std::stop_source stop_source;
    harness::ExecOptions options;
    options.stop_token = stop_source.get_token();
    options.timeout = std::chrono::seconds{30};

    boost::asio::io_context io;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(shell.exec(
                        "echo $$ > shell.pid; sleep 30 & echo $! > descendant.pid; wait", std::move(options)));
                co_return;
            },
            boost::asio::detached);
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                boost::asio::steady_timer timer(
                        co_await boost::asio::this_coro::executor, std::chrono::milliseconds{250});
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
    CHECK(result->error().code == harness::ExecutionErrorCode::Aborted);
    CHECK(result->error().message.find("side effects may remain") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds{2});

    // The shell leader's reaping is owned by the runner's waitpid, so it must
    // demand full removal from the PID table. The descendant is adopted by an
    // external reaper when the leader dies; on the Arch pinned CI container
    // that reaper (PID 1, `tail -f /dev/null`) never waits, so the exited
    // descendant legitimately stays visible as a zombie (#524).
    CHECK(tests::await_process_reaped(workspace.read("shell.pid")));
    CHECK(tests::await_process_exit(workspace.read("descendant.pid")));
}

TEST_CASE("input and timer callbacks progress while a long shell command runs on the runtime",
        "[harness][async][issue459]") {
    tests::TempWorkspace workspace;
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(io,
            harness::RuntimeLimits{
                    .worker_count = 2,
                    .max_admitted_operations = 8,
                    .max_admitted_bytes = 1024 * 1024,
            });
    auto target = root.make_target();
    harness::AsyncLocalShell shell(target, workspace.path(), true);

    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> shell_result;
    std::atomic<bool> input_fired{false};
    std::atomic<bool> timer_fired{false};
    boost::asio::co_spawn(
            *io,
            [&]() -> boost::asio::awaitable<void> {
                shell_result = co_await support::detail::await_async_result(shell.exec("sleep 1; echo done"));
                co_return;
            },
            boost::asio::detached);
    boost::asio::post(*io, [&]() noexcept { input_fired.store(true, std::memory_order_release); });
    boost::asio::steady_timer timer(*io, std::chrono::milliseconds{50});
    timer.async_wait([&](const boost::system::error_code&) { timer_fired.store(true, std::memory_order_release); });

    // The shell process runs through the shared Runtime loop; input posts and
    // timers still make progress while it is in flight.
    const auto started = std::chrono::steady_clock::now();
    while (!shell_result.has_value() && std::chrono::steady_clock::now() - started < std::chrono::seconds{5}) {
        if (io->stopped()) {
            io->restart();
        }
        (void)io->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(shell_result.has_value());
    REQUIRE(*shell_result);
    CHECK((*shell_result)->stdout_output.find("done") != std::string::npos);
    CHECK(input_fired.load());
    CHECK(timer_fired.load());
    root.close();
}

TEST_CASE("default process runner caps newline-free output without waiting for line breaks",
        "[harness][async][process]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", "printf '%60000s' '' | tr ' ' x"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds(5000);
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 2000};
    std::string streamed_output;
    // The capture remains alive until run_awaitable_pi drives the process to quiescence.
    request.on_stdout = [&](std::string_view chunk) -> support::ExpectedVoid {
        streamed_output.append(chunk);
        return {};
    };

    auto result = run_awaitable<harness::ProcessResult>([&]() { return runner.run(std::move(request)); });

    REQUIRE(result);
    CHECK(result->output.size() <= 1100);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
    CHECK(streamed_output.size() == 60000);
}

TEST_CASE("default process runner bounds truncated output on UTF-8 character boundaries",
        "[harness][async][process][issue72]") {
    harness::DefaultAsyncProcessRunner runner;
    harness::ProcessRequest request;
    request.executable = "/bin/bash";
    // 500 euro signs (3 bytes each); a byte-exact 1024 cap would split the last one.
    request.arguments = {"-c", "printf '€%.0s' {1..500}"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds(5000);
    request.output_limit = harness::OutputLimit{.max_bytes = 1024, .max_lines = 2000};

    auto result = run_awaitable<harness::ProcessResult>([&]() { return runner.run(std::move(request)); });

    REQUIRE(result);
    REQUIRE(result->stdout_truncated);
    std::string expected;
    for (int i = 0; i < 341; ++i) {
        expected += "€";
    }
    CHECK(result->stdout_output == expected);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// U1: pi-shaped public contract compile / construction tests
// ---------------------------------------------------------------------------

TEST_CASE("pi-shaped public types compile with aggregate construction", "[harness][u1]") {
    // FileKind
    CHECK(static_cast<int>(harness::FileKind::File) != static_cast<int>(harness::FileKind::Directory));
    CHECK(static_cast<int>(harness::FileKind::Symlink) != static_cast<int>(harness::FileKind::File));

    // FileInfo aggregate
    harness::FileInfo info{
            .name = "note.txt",
            .path = "/ws/note.txt",
            .kind = harness::FileKind::File,
            .size = 42,
            .mtimeMs = 1700000000000,
    };
    CHECK(info.name == "note.txt");
    CHECK(info.path == "/ws/note.txt");
    CHECK(info.kind == harness::FileKind::File);
    CHECK(info.size == 42);
    CHECK(info.mtimeMs == 1700000000000);

    // FileError
    harness::FileError fe{harness::FileErrorCode::NotFound, "missing", std::string{"/tmp/x"}};
    CHECK(fe.code == harness::FileErrorCode::NotFound);
    CHECK(fe.message == "missing");
    CHECK(fe.path == "/tmp/x");

    // ExecutionError
    harness::ExecutionError ee{harness::ExecutionErrorCode::Timeout, "timed out"};
    CHECK(ee.code == harness::ExecutionErrorCode::Timeout);
    CHECK(ee.message == "timed out");

    // ExecOptions
    harness::ExecOptions opts;
    opts.cwd = "subdir";
    opts.timeout = std::chrono::milliseconds(5000);
    CHECK(opts.cwd == "subdir");
    CHECK(opts.timeout->count() == 5000);

    // ShellExecResult
    harness::ShellExecResult sr{"out", "err", 0};
    CHECK(sr.stdout_output == "out");
    CHECK(sr.stderr_output == "err");
    CHECK(sr.exitCode == 0);

    // BinaryData
    harness::BinaryData bin{std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}};
    REQUIRE(bin.size() == 3);
    CHECK(bin[1] == std::byte{0xFF});

    // WriteContent variant
    harness::WriteContent text = std::string{"hello"};
    harness::WriteContent bytes = harness::BinaryData{std::byte{0x01}};
    CHECK(std::holds_alternative<std::string>(text));
    CHECK(std::holds_alternative<harness::BinaryData>(bytes));
}

TEST_CASE("pi-shaped error conversion helpers map to support::Error", "[harness][u1]") {
    // FileError → support::Error
    auto ue = harness::to_util_error(harness::FileError{
            .code = harness::FileErrorCode::PermissionDenied, .message = "denied", .path = std::string{"/x"}});
    CHECK(ue.code == support::ErrorCode::Workspace);
    CHECK(ue.detail.find("denied") != std::string::npos);
    CHECK(ue.context == "/x");

    // ExecutionError → support::Error
    auto ee = harness::to_util_error(
            harness::ExecutionError{.code = harness::ExecutionErrorCode::Timeout, .message = "too slow"});
    CHECK(ee.code == support::ErrorCode::Timeout);
    CHECK(ee.detail.find("too slow") != std::string::npos);

    // Every FileErrorCode maps without unknown fallback (except Unknown/NotSupported)
    for (auto code : {harness::FileErrorCode::Aborted,
                 harness::FileErrorCode::NotFound,
                 harness::FileErrorCode::PermissionDenied,
                 harness::FileErrorCode::NotDirectory,
                 harness::FileErrorCode::IsDirectory,
                 harness::FileErrorCode::Invalid,
                 harness::FileErrorCode::NotSupported,
                 harness::FileErrorCode::Unknown}) {
        auto err = harness::to_util_error(harness::FileError{.code = code, .message = "test", .path = std::nullopt});
        bool allowed_unknown =
                (code == harness::FileErrorCode::Unknown || code == harness::FileErrorCode::NotSupported);
        CHECK((static_cast<int>(err.code) != static_cast<int>(support::ErrorCode::Unknown) || allowed_unknown));
    }

    // Every ExecutionErrorCode maps without unknown fallback
    for (auto code : {harness::ExecutionErrorCode::Aborted,
                 harness::ExecutionErrorCode::Timeout,
                 harness::ExecutionErrorCode::ShellUnavailable,
                 harness::ExecutionErrorCode::SpawnError,
                 harness::ExecutionErrorCode::CallbackError,
                 harness::ExecutionErrorCode::NotSupported,
                 harness::ExecutionErrorCode::Unknown}) {
        auto err = harness::to_util_error(harness::ExecutionError{.code = code, .message = "test"});
        bool allowed_unknown =
                (code == harness::ExecutionErrorCode::Unknown || code == harness::ExecutionErrorCode::NotSupported);
        CHECK((static_cast<int>(err.code) != static_cast<int>(support::ErrorCode::Unknown) || allowed_unknown));
    }
}

// ---------------------------------------------------------------------------
// U3: pi-shaped shell exec tests
// ---------------------------------------------------------------------------

TEST_CASE("pi-shaped exec returns split stdout and stderr streams", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    auto result = run_awaitable_pi(shell.exec("echo hello && echo error >&2"));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->stderr_output.find("error") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec stdout-only and stderr-only preserve empty unused stream", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    auto stdout_only = run_awaitable_pi(shell.exec("echo only"));
    REQUIRE(stdout_only);
    CHECK(stdout_only->stdout_output.find("only") != std::string::npos);
    CHECK(stdout_only->stderr_output.empty());

    auto stderr_only = run_awaitable_pi(shell.exec("echo only_err >&2"));
    REQUIRE(stderr_only);
    CHECK(stderr_only->stdout_output.empty());
    CHECK(stderr_only->stderr_output.find("only_err") != std::string::npos);
}

TEST_CASE("pi-shaped exec honors cwd override", "[harness][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("sub/note.txt", "hello");
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "sub";
    auto result = run_awaitable_pi(shell.exec("cat note.txt", std::move(opts)));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec rejects cwd that escapes workspace", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "../outside";
    auto result = run_awaitable_pi(shell.exec("pwd", std::move(opts)));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::SpawnError);
}

TEST_CASE("pi-shaped exec returns shell_unavailable when bash is disabled", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), false);

    auto result = run_awaitable_pi(shell.exec("echo nope"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::ShellUnavailable);
}

TEST_CASE("pi-shaped exec times out without blocking io context", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);
    const auto started = std::chrono::steady_clock::now();

    harness::ExecOptions opts;
    opts.timeout = std::chrono::milliseconds(100);
    auto result = run_awaitable_pi(shell.exec("sleep 2", std::move(opts)));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::Timeout);
    CHECK(elapsed < std::chrono::milliseconds(1500));
}

TEST_CASE("pi-shaped exec preserves nonzero exit codes", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    auto result = run_awaitable_pi(shell.exec("exit 42"));
    REQUIRE(result);
    CHECK(result->exitCode == 42);
}

TEST_CASE("separate Shell executions restart from the canonical workspace", "[harness][shell][issue84]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directory(workspace.path() / "nested");
    harness::AsyncLocalShell shell(test_runtime_target(), workspace.path(), true);

    const auto changed = run_awaitable_pi(shell.exec("cd nested && pwd"));
    const auto restarted = run_awaitable_pi(shell.exec("pwd"));

    REQUIRE(changed);
    REQUIRE(restarted);
    CHECK(changed->stdout_output.find((workspace.path() / "nested").string()) != std::string::npos);
    CHECK(restarted->stdout_output.find(workspace.path().string()) != std::string::npos);
    CHECK(restarted->stdout_output.find((workspace.path() / "nested").string()) == std::string::npos);
}
