#include "../support/EnvVarGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <cch/agent/harness/LocalExecutionEnv.hpp>
#include "harness/ShellResolver.hpp"
#include "harness/SyncLocalExecutionEnv.hpp"
#include "harness/RuntimeRoot.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "util/Process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <charconv>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

using namespace cch;

namespace {

class FakeAsyncProcessRunner final : public util::AsyncProcessRunner {
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

/// Like run_awaitable but for pi-shaped methods that return std::expected<T, E>
/// (not util::Expected<T>). Returns the raw std::expected<T, E>.
class TestRuntime final {
public:
    TestRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()),
          work_guard_(boost::asio::make_work_guard(*loop_)),
          root_(loop_, harness::RuntimeLimits{}),
          loop_thread_([this] { loop_->run(); }) {}

    ~TestRuntime() {
        work_guard_.reset();
        loop_->stop();
    }

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() {
        return root_.make_target();
    }

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

template <typename T, typename E>
std::expected<T, E> run_awaitable_pi(support::AsyncResult<T, E> operation) {
    boost::asio::io_context io;
    std::optional<std::expected<T, E>> result;
    boost::asio::co_spawn(
        io,
        [operation = std::move(operation), &result]() mutable -> boost::asio::awaitable<void> {
            result = co_await ai::detail::await_async_result(std::move(operation));
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

template <typename T>
[[nodiscard]] support::AsyncResult<T, harness::FileError> ready_file(T value) {
    return support::AsyncResult<T, harness::FileError>{
        std::expected<T, harness::FileError>{std::move(value)}};
}

[[nodiscard]] inline support::AsyncResult<void, harness::FileError> ready_file() {
    return support::AsyncResult<void, harness::FileError>{
        std::expected<void, harness::FileError>{}};
}

template <typename T>
[[nodiscard]] support::AsyncResult<T, harness::ExecutionError> ready_execution(T value) {
    return support::AsyncResult<T, harness::ExecutionError>{
        std::expected<T, harness::ExecutionError>{std::move(value)}};
}

template <typename T>
void check_file_operation_aborted(const std::expected<T, harness::FileError>& result) {
    CHECK_FALSE(result);
    if (!result) {
        CHECK(result.error().code == harness::FileErrorCode::Aborted);
    }
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    CHECK_FALSE(error);
    return exists;
}

} // namespace

TEST_CASE("process runner capability follows async naming and ownership rules", "[harness][async][issue75]") {
    static_assert(std::is_abstract_v<util::AsyncProcessRunner>);
    static_assert(std::is_final_v<util::DefaultAsyncProcessRunner>);
}

TEST_CASE("every async filesystem operation observes a pre-requested cancellation", "[harness][async][issue40]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "unchanged");
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path());
    std::stop_source stop_source;
    stop_source.request_stop();
    const auto stop_token = stop_source.get_token();

    check_file_operation_aborted(run_awaitable_pi(env.absolutePath("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.joinPath({"nested", "note.txt"}, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.readTextFile("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.readTextLines("note.txt", std::nullopt, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.readBinaryFile("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.writeFile(
        "note.txt", std::string{"changed"}, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.appendFile(
        "note.txt", std::string{"changed"}, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.fileInfo("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.listDir(".", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.canonicalPath("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.exists("note.txt", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.createDir("new-dir", true, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.remove("note.txt", false, stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.createTempDir("tmp-", stop_token)));
    check_file_operation_aborted(run_awaitable_pi(env.createTempFile("tmp-", ".txt", stop_token)));

    CHECK(workspace.read("note.txt") == "unchanged");
    CHECK_FALSE(path_exists(workspace.path() / "new-dir"));
}

TEST_CASE("async local execution env preserves file read and write safety", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path());

    auto written = run_awaitable_pi(env.writeFile(
        "nested/note.txt", std::string{"hello"}, std::stop_token{}));
    REQUIRE(written);

    auto read = run_awaitable_pi(env.readTextFile("nested/note.txt", std::stop_token{}));
    REQUIRE(read);
    CHECK(*read == "hello");

    auto escaped = run_awaitable_pi(env.readTextFile("../outside.txt", std::stop_token{}));
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == harness::FileErrorCode::PermissionDenied);
}

TEST_CASE("async local execution env runs shell commands concurrently", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    boost::asio::io_context io;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> first;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> second;
    const auto started = std::chrono::steady_clock::now();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            first = co_await ai::detail::await_async_result(env.exec("sleep 0.6; echo first"));
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            second = co_await ai::detail::await_async_result(env.exec("sleep 0.6; echo second"));
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
#if defined(__unix__) || defined(__APPLE__)
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
    harness::SyncLocalExecutionEnv env(
        workspace.path(), true, {"HOME"}, std::move(shell_config), runner);

    auto shell = env.exec("printf '%s' \"$CCH_READY\"");

    REQUIRE(shell);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].executable == workspace.path() / "bin/custom-shell");
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[0] == "-c");
    CHECK(runner->requests[0].arguments[1] ==
          "export CCH_READY=1\nprintf '%s' \"$CCH_READY\"");
    CHECK(runner->requests[0].working_directory == workspace.path());
#else
    SUCCEED("custom Shell resolution is supported on Unix platforms");
#endif
}

TEST_CASE("Shell resolution expands home when HOME is absent from the environment", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    tests::EnvVarGuard home_guard{"HOME", std::nullopt};
    const auto* user = ::getpwuid(::getuid());
    REQUIRE(user != nullptr);
    REQUIRE(user->pw_dir != nullptr);
    const std::string missing_name = "cch-issue84-missing-shell";

    const auto resolved = harness::resolve_shell_executable(
        "~/" + missing_name,
        workspace.path(),
        {});

    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().code == harness::ExecutionErrorCode::ShellUnavailable);
    CHECK(resolved.error().message.find(
              (std::filesystem::path{user->pw_dir} / missing_name).string()) != std::string::npos);
    CHECK(resolved.error().message.find((workspace.path() / "~").string()) == std::string::npos);
#else
    SUCCEED("home expansion is supported on Unix platforms");
#endif
}

TEST_CASE("Shell adapter defaults to non-login system bash", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    harness::SyncLocalExecutionEnv env(workspace.path(), true, {}, {}, runner);

    auto shell = env.exec("printf default");

    REQUIRE(shell);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].executable == "/bin/bash");
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[0] == "-c");
    CHECK(runner->requests[0].arguments[1] == "printf default");
#else
    SUCCEED("default Shell resolution is supported on Unix platforms");
#endif
}

TEST_CASE("Shell adapter ignores an empty command prefix", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    harness::ShellConfig shell_config{.command_prefix = ""};
    harness::SyncLocalExecutionEnv env(
        workspace.path(), true, {}, std::move(shell_config), runner);

    auto shell = env.exec("printf plain");

    REQUIRE(shell);
    REQUIRE(runner->requests.size() == 1);
    REQUIRE(runner->requests[0].arguments.size() == 2);
    CHECK(runner->requests[0].arguments[1] == "printf plain");
#else
    SUCCEED("Shell prefix behavior is supported on Unix platforms");
#endif
}

TEST_CASE("Shell resolution prefers PATH bash before sh when system bash is unavailable", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    workspace.write("bin/bash", "#!/bin/sh\nexit 0\n");
    workspace.write("bin/sh", "#!/bin/sh\nexit 0\n");
    REQUIRE(::chmod((workspace.path() / "bin/bash").c_str(), 0700) == 0);
    REQUIRE(::chmod((workspace.path() / "bin/sh").c_str(), 0700) == 0);
    const std::map<std::string, std::string> environment{
        {"PATH", (workspace.path() / "bin").string()},
    };

    const auto resolved = harness::resolve_shell_executable(
        std::nullopt,
        workspace.path(),
        environment,
        workspace.path() / "missing-system-bash");

    REQUIRE(resolved);
    CHECK(*resolved == workspace.path() / "bin/bash");
#else
    SUCCEED("PATH Shell resolution is supported on Unix platforms");
#endif
}

TEST_CASE("Shell resolution falls back to PATH sh", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    workspace.write("bin/sh", "#!/bin/sh\nexit 0\n");
    REQUIRE(::chmod((workspace.path() / "bin/sh").c_str(), 0700) == 0);
    const std::map<std::string, std::string> environment{
        {"PATH", (workspace.path() / "bin").string()},
    };

    const auto resolved = harness::resolve_shell_executable(
        std::nullopt,
        workspace.path(),
        environment,
        workspace.path() / "missing-system-bash");

    REQUIRE(resolved);
    CHECK(*resolved == workspace.path() / "bin/sh");
#else
    SUCCEED("sh fallback is supported on Unix platforms");
#endif
}

TEST_CASE("a stale configured Shell path fails only attempted execution", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    harness::ShellConfig shell_config{
        .shell_path = (workspace.path() / "stale-shell").string(),
        .command_prefix = "prefix-must-not-appear-in-diagnostics",
    };
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true, {}, std::move(shell_config));

    auto read = run_awaitable_pi(env.absolutePath("ordinary.txt", std::stop_token{}));
    REQUIRE(read);
    const auto shell = run_awaitable_pi(env.exec("printf command"));

    REQUIRE_FALSE(shell);
    CHECK(shell.error().code == harness::ExecutionErrorCode::ShellUnavailable);
    CHECK(shell.error().message.find("stale-shell") != std::string::npos);
    CHECK(shell.error().message.find("prefix-must-not-appear") == std::string::npos);
#else
    SUCCEED("custom Shell resolution is supported on Unix platforms");
#endif
}

TEST_CASE("async local execution env sanitizes shell environment through process capability", "[harness][async][u2]") {
#if defined(__unix__) || defined(__APPLE__)
    setenv("OPENAI_API_KEY", "sk-test-secret", 1);
    setenv("KIMI_API_KEY", "kimi-secret-value", 1);
    setenv("CCH_VISIBLE_ENV", "visible", 1);
    setenv("CCH_CREDENTIAL", "plain-secret-value", 1);
#endif
    tests::TempWorkspace workspace;
    auto runner = std::make_shared<FakeAsyncProcessRunner>();
    runner->next.exit_code = 0;
    runner->next.stdout_output = "ok";
    harness::SyncLocalExecutionEnv env(
        workspace.path(), true, {"CCH_CREDENTIAL", "KIMI_API_KEY"}, {}, runner);

    std::stop_source stop_source;
    harness::ExecOptions opts;
    opts.timeout = std::chrono::milliseconds(123);
    opts.stop_token = stop_source.get_token();
    auto shell = env.exec("env", std::move(opts));

    REQUIRE(shell);
    REQUIRE(runner->requests.size() == 1);
    CHECK(runner->requests[0].working_directory == workspace.path());
    CHECK(runner->requests[0].timeout.count() == 123);
    CHECK(runner->requests[0].stop_token == stop_source.get_token());
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

TEST_CASE("pi-shaped exec rejects a pre-cancelled request before spawning", "[harness][async][issue40]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);
    std::stop_source stop_source;
    stop_source.request_stop();
    harness::ExecOptions options;
    options.stop_token = stop_source.get_token();

    auto result = run_awaitable_pi(env.exec("touch should-not-exist", std::move(options)));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::Aborted);
    CHECK_FALSE(path_exists(workspace.path() / "should-not-exist"));
}

TEST_CASE("cancelling exec terminates the process group and reaps the shell", "[harness][async][process][issue40]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);
    std::stop_source stop_source;
    harness::ExecOptions options;
    options.stop_token = stop_source.get_token();
    options.timeout = std::chrono::seconds{30};

    boost::asio::io_context io;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await ai::detail::await_async_result(env.exec(
                "echo $$ > shell.pid; sleep 30 & echo $! > descendant.pid; wait",
                std::move(options)));
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
    CHECK(result->error().code == harness::ExecutionErrorCode::Aborted);
    CHECK(result->error().message.find("side effects may remain") != std::string::npos);
    CHECK(elapsed < std::chrono::seconds{2});

    auto check_process_absent = [&](const std::string& pid_file) {
        const auto pid_text = workspace.read(pid_file);
        pid_t pid{};
        const auto [end, parse_error] = std::from_chars(
            pid_text.data(), pid_text.data() + pid_text.size(), pid);
        REQUIRE(parse_error == std::errc{});
        CHECK(end != pid_text.data());
        errno = 0;
        CHECK(::kill(pid, 0) == -1);
        CHECK(errno == ESRCH);
    };
    check_process_absent("shell.pid");
    check_process_absent("descendant.pid");
#else
    SUCCEED("process-group cancellation is covered on supported POSIX platforms");
#endif
}

TEST_CASE(
    "async filesystem submission returns typed Busy when the runtime is saturated",
    "[harness][async][issue459]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "original");
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 1,
            .max_admitted_bytes = 1024 * 1024,
        });
    auto target = root.make_target();
    harness::AsyncLocalExecutionEnv env(target, workspace.path());

    std::atomic<bool> release_worker{false};
    auto gate = target->try_admit(4);
    REQUIRE(gate.has_value());
    REQUIRE(gate->post_worker([&]() noexcept {
        while (!release_worker.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }));

    // The single admission is occupied; the read is rejected as typed Busy
    // before any worker runs it, and the file is untouched (no inline
    // fallback, no silent drop).
    auto result = run_awaitable_pi(env.readTextFile("note.txt", std::stop_token{}));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::FileErrorCode::Busy);
    CHECK(workspace.read("note.txt") == "original");

    release_worker.store(true, std::memory_order_release);
    root.close();
}

TEST_CASE(
    "queued filesystem work cancelled before start performs no side effects",
    "[harness][async][issue459]") {
    tests::TempWorkspace workspace;
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 1,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 1024 * 1024,
        });
    auto target = root.make_target();
    harness::AsyncLocalExecutionEnv env(target, workspace.path());

    // Occupy the single worker so the write below stays queued (not started).
    std::atomic<bool> release_gate{false};
    auto gate = target->try_admit(4);
    REQUIRE(gate.has_value());
    REQUIRE(gate->post_worker([&]() noexcept {
        while (!release_gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }));
    // The gate holds mailbox sequence 0; complete it with a no-op terminal so
    // the admission-order mailbox does not hold back the write's outcome.
    std::move(*gate).complete([]() noexcept {});

    std::stop_source stop_source;
    std::optional<std::expected<void, harness::FileError>> outcome;
    auto pending = env.writeFile(
        "note.txt", std::string{"changed"}, stop_source.get_token());
    std::move(pending).start(
        [&](std::expected<void, harness::FileError> result) noexcept {
            outcome.emplace(std::move(result));
        });
    // Cancel before the queued write reaches a worker, then release the gate.
    stop_source.request_stop();
    release_gate.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!outcome.has_value() && std::chrono::steady_clock::now() < deadline) {
        if (io->stopped()) {
            io->restart();
        }
        (void)io->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(*outcome);
    CHECK(outcome->error().code == harness::FileErrorCode::Aborted);
    CHECK_FALSE(path_exists(workspace.path() / "note.txt"));
    root.close();
}

TEST_CASE(
    "input and timer callbacks progress while a long shell command runs on the runtime",
    "[harness][async][issue459]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    auto io = std::make_shared<boost::asio::io_context>();
    harness::RuntimeRoot root(
        io,
        harness::RuntimeLimits{
            .worker_count = 2,
            .max_admitted_operations = 8,
            .max_admitted_bytes = 1024 * 1024,
        });
    auto target = root.make_target();
    harness::AsyncLocalExecutionEnv env(target, workspace.path(), true);

    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> shell_result;
    std::atomic<bool> input_fired{false};
    std::atomic<bool> timer_fired{false};
    boost::asio::co_spawn(
        *io,
        [&]() -> boost::asio::awaitable<void> {
            shell_result = co_await ai::detail::await_async_result(
                env.exec("sleep 1; echo done"));
            co_return;
        },
        boost::asio::detached);
    boost::asio::post(*io, [&]() noexcept { input_fired.store(true, std::memory_order_release); });
    boost::asio::steady_timer timer(*io, std::chrono::milliseconds{50});
    timer.async_wait([&](const boost::system::error_code&) {
        timer_fired.store(true, std::memory_order_release);
    });

    // The shell process runs through the shared Runtime loop; input posts and
    // timers still make progress while it is in flight.
    const auto started = std::chrono::steady_clock::now();
    while (!shell_result.has_value() &&
           std::chrono::steady_clock::now() - started < std::chrono::seconds{5}) {
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
#else
    SUCCEED("Shell progress on the shared runtime is covered on supported POSIX platforms");
#endif
}

TEST_CASE("default process runner caps newline-free output without waiting for line breaks", "[harness][async][process]") {
    util::DefaultAsyncProcessRunner runner;
    util::ProcessRequest request;
    request.executable = "/bin/bash";
    request.arguments = {"-c", "printf '%60000s' '' | tr ' ' x"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds(5000);
    request.output_limit = util::OutputLimit{.max_bytes = 1024, .max_lines = 2000};
    std::string streamed_output;
    request.on_stdout = [&](std::string_view chunk) {
        streamed_output.append(chunk);
    };

    auto result = run_awaitable<util::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

    REQUIRE(result);
    CHECK(result->output.size() <= 1100);
    CHECK(result->output.find("[output truncated]") != std::string::npos);
    CHECK(streamed_output.size() == 60000);
}

TEST_CASE("default process runner bounds truncated output on UTF-8 character boundaries", "[harness][async][process][issue72]") {
    util::DefaultAsyncProcessRunner runner;
    util::ProcessRequest request;
    request.executable = "/bin/bash";
    // 500 euro signs (3 bytes each); a byte-exact 1024 cap would split the last one.
    request.arguments = {"-c", "printf '€%.0s' {1..500}"};
    request.working_directory = std::filesystem::current_path();
    request.timeout = std::chrono::milliseconds(5000);
    request.output_limit = util::OutputLimit{.max_bytes = 1024, .max_lines = 2000};

    auto result = run_awaitable<util::ProcessResult>([&]() {
        return runner.run(std::move(request));
    });

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

TEST_CASE("pi-shaped error conversion helpers map to util::Error", "[harness][u1]") {
    // FileError → util::Error
    auto ue = harness::to_util_error(harness::FileError{
        harness::FileErrorCode::PermissionDenied, "denied", std::string{"/x"}});
    CHECK(ue.code == util::ErrorCode::Workspace);
    CHECK(ue.detail.find("denied") != std::string::npos);
    CHECK(ue.context == "/x");

    // ExecutionError → util::Error
    auto ee = harness::to_util_error(harness::ExecutionError{
        harness::ExecutionErrorCode::Timeout, "too slow"});
    CHECK(ee.code == util::ErrorCode::Timeout);
    CHECK(ee.detail.find("too slow") != std::string::npos);

    // Every FileErrorCode maps without unknown fallback (except Unknown/NotSupported)
    for (auto code : {harness::FileErrorCode::Aborted, harness::FileErrorCode::NotFound,
                      harness::FileErrorCode::PermissionDenied, harness::FileErrorCode::NotDirectory,
                      harness::FileErrorCode::IsDirectory, harness::FileErrorCode::Invalid,
                      harness::FileErrorCode::NotSupported, harness::FileErrorCode::Unknown}) {
        auto err = harness::to_util_error(harness::FileError{code, "test", std::nullopt});
        bool allowed_unknown = (code == harness::FileErrorCode::Unknown ||
                                code == harness::FileErrorCode::NotSupported);
        CHECK((static_cast<int>(err.code) != static_cast<int>(util::ErrorCode::Unknown) || allowed_unknown));
    }

    // Every ExecutionErrorCode maps without unknown fallback
    for (auto code : {harness::ExecutionErrorCode::Aborted, harness::ExecutionErrorCode::Timeout,
                      harness::ExecutionErrorCode::ShellUnavailable, harness::ExecutionErrorCode::SpawnError,
                      harness::ExecutionErrorCode::CallbackError,
                      harness::ExecutionErrorCode::NotSupported,
                      harness::ExecutionErrorCode::Unknown}) {
        auto err = harness::to_util_error(harness::ExecutionError{code, "test"});
        bool allowed_unknown = (code == harness::ExecutionErrorCode::Unknown ||
                                code == harness::ExecutionErrorCode::NotSupported);
        CHECK((static_cast<int>(err.code) != static_cast<int>(util::ErrorCode::Unknown) || allowed_unknown));
    }
}

TEST_CASE("ready fakes implement the complete pi-shaped AsyncResult surface", "[harness][u1][issue69]") {
    struct CompleteFake final : harness::AsyncExecutionEnv {
        std::filesystem::path ws{"/tmp/ws"};
        const std::filesystem::path& workspace() const override { return ws; }
        support::AsyncResult<std::string, harness::FileError> absolutePath(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
        support::AsyncResult<std::string, harness::FileError> joinPath(std::vector<std::string>, std::stop_token) override { return ready_file(std::string{"/joined"}); }
        support::AsyncResult<std::string, harness::FileError> readTextFile(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
        support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(std::string, std::optional<int>, std::stop_token) override { return ready_file(std::vector<std::string>{}); }
        support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(std::string, std::stop_token) override { return ready_file(harness::BinaryData{}); }
        support::AsyncResult<void, harness::FileError> writeFile(std::string, harness::WriteContent, std::stop_token) override { return ready_file(); }
        support::AsyncResult<void, harness::FileError> appendFile(std::string, harness::WriteContent, std::stop_token) override { return ready_file(); }
        support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(std::string path, std::stop_token) override { return ready_file(harness::FileInfo{.name = path, .path = "/tmp/ws/" + path, .kind = harness::FileKind::File}); }
        support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(std::string, std::stop_token) override { return ready_file(std::vector<harness::FileInfo>{}); }
        support::AsyncResult<std::string, harness::FileError> canonicalPath(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
        support::AsyncResult<bool, harness::FileError> exists(std::string, std::stop_token) override { return ready_file(true); }
        support::AsyncResult<void, harness::FileError> createDir(std::string, bool, std::stop_token) override { return ready_file(); }
        support::AsyncResult<void, harness::FileError> remove(std::string, bool, std::stop_token) override { return ready_file(); }
        support::AsyncResult<std::string, harness::FileError> createTempDir(std::optional<std::string>, std::stop_token) override { return ready_file(std::string{"/tmp/ws/tmp"}); }
        support::AsyncResult<std::string, harness::FileError> createTempFile(std::optional<std::string>, std::optional<std::string>, std::stop_token) override { return ready_file(std::string{"/tmp/ws/tmp-file"}); }
        support::AsyncResult<harness::ShellExecResult, harness::ExecutionError> exec(std::string command, harness::ExecOptions) override { return ready_execution(harness::ShellExecResult{.stdout_output = std::move(command), .stderr_output = "", .exitCode = 0}); }
    };

    CompleteFake fake;
    auto read = run_awaitable_pi(fake.readTextFile("fake", {}));
    REQUIRE(read);
    CHECK(*read == "fake");
    auto info = run_awaitable_pi(fake.fileInfo("note.txt", {}));
    REQUIRE(info);
    CHECK(info->name == "note.txt");
    auto result = run_awaitable_pi(fake.exec("pwd", {}));
    REQUIRE(result);
    CHECK(result->stdout_output == "pwd");
    auto cleanup = run_awaitable_pi(fake.cleanup());
    CHECK(cleanup);
}

// ---------------------------------------------------------------------------
// U3: pi-shaped shell exec tests
// ---------------------------------------------------------------------------

TEST_CASE("pi-shaped exec returns split stdout and stderr streams", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    auto result = run_awaitable_pi(env.exec("echo hello && echo error >&2"));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->stderr_output.find("error") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec stdout-only and stderr-only preserve empty unused stream", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    auto stdout_only = run_awaitable_pi(env.exec("echo only"));
    REQUIRE(stdout_only);
    CHECK(stdout_only->stdout_output.find("only") != std::string::npos);
    CHECK(stdout_only->stderr_output.empty());

    auto stderr_only = run_awaitable_pi(env.exec("echo only_err >&2"));
    REQUIRE(stderr_only);
    CHECK(stderr_only->stdout_output.empty());
    CHECK(stderr_only->stderr_output.find("only_err") != std::string::npos);
}

TEST_CASE("pi-shaped exec honors cwd override", "[harness][u3]") {
    tests::TempWorkspace workspace;
    workspace.write("sub/note.txt", "hello");
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "sub";
    auto result = run_awaitable_pi(env.exec("cat note.txt", std::move(opts)));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec rejects cwd that escapes workspace", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "../outside";
    auto result = run_awaitable_pi(env.exec("pwd", std::move(opts)));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::SpawnError);
}

TEST_CASE("pi-shaped exec returns shell_unavailable when bash is disabled", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), false);

    auto result = run_awaitable_pi(env.exec("echo nope"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::ShellUnavailable);
}

TEST_CASE("pi-shaped exec times out without blocking io context", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);
    const auto started = std::chrono::steady_clock::now();

    harness::ExecOptions opts;
    opts.timeout = std::chrono::milliseconds(100);
    auto result = run_awaitable_pi(env.exec("sleep 2", std::move(opts)));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::Timeout);
    CHECK(elapsed < std::chrono::milliseconds(1500));
}

TEST_CASE("pi-shaped exec preserves nonzero exit codes", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    auto result = run_awaitable_pi(env.exec("exit 42"));
    REQUIRE(result);
    CHECK(result->exitCode == 42);
}

TEST_CASE("separate Shell executions restart from the canonical workspace", "[harness][shell][issue84]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    std::filesystem::create_directory(workspace.path() / "nested");
    harness::AsyncLocalExecutionEnv env(test_runtime_target(), workspace.path(), true);

    const auto changed = run_awaitable_pi(env.exec("cd nested && pwd"));
    const auto restarted = run_awaitable_pi(env.exec("pwd"));

    REQUIRE(changed);
    REQUIRE(restarted);
    CHECK(changed->stdout_output.find((workspace.path() / "nested").string()) != std::string::npos);
    CHECK(restarted->stdout_output.find(workspace.path().string()) != std::string::npos);
    CHECK(restarted->stdout_output.find((workspace.path() / "nested").string()) == std::string::npos);
#else
    SUCCEED("Shell execution is supported on Unix platforms");
#endif
}
