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

/// Like run_awaitable but for pi-shaped methods that return std::expected<T, E>
/// (not util::Expected<T>). Returns the raw std::expected<T, E>.
template <typename T>
T run_awaitable_pi(boost::asio::awaitable<T> start) {
    boost::asio::io_context io;
    std::optional<T> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await std::move(start);
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
    CHECK(bin.size() == 3);
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

TEST_CASE("focused fake can override only tool-shaped methods", "[harness][u1]") {
    // A fake that only implements the required pure virtual methods.
    // The new pi-shaped methods inherit default not_supported implementations.
    struct FocusedFake : harness::AsyncExecutionEnv {
        std::filesystem::path ws{"/tmp/ws"};
        const std::filesystem::path& workspace() const override { return ws; }
        bool bash_enabled() const override { return false; }

        boost::asio::awaitable<util::Expected<harness::AsyncFileReadResult>> read_file(
            std::string, int, int) override {
            co_return harness::AsyncFileReadResult{"fake"};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncFileWriteResult>> write_file(
            std::string, std::string, bool) override {
            co_return harness::AsyncFileWriteResult{4};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncFileEditResult>> edit_file(
            std::string, std::string, std::string) override {
            co_return harness::AsyncFileEditResult{};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncShellResult>> run_shell(
            std::string, std::chrono::milliseconds) override {
            co_return harness::AsyncShellResult{};
        }
    };

    FocusedFake fake;
    CHECK(fake.workspace() == "/tmp/ws");
    CHECK_FALSE(fake.bash_enabled());

    // Tool-shaped methods work.
    auto read = run_awaitable<harness::AsyncFileReadResult>(
        [&]() { return fake.read_file("x", 1, 0); });
    REQUIRE(read);
    CHECK(read->content == "fake");

    // Pi-shaped methods return not_supported.
    auto info = run_awaitable_pi(fake.fileInfo("x"));
    REQUIRE_FALSE(info);
    CHECK(info.error().code == harness::FileErrorCode::NotSupported);

    auto exec_result = run_awaitable_pi(fake.exec("ls"));
    REQUIRE_FALSE(exec_result);
    CHECK(exec_result.error().code == harness::ExecutionErrorCode::NotSupported);
}

TEST_CASE("complete fake can override full pi-shaped capability surface", "[harness][u1]") {
    struct CompleteFake : harness::AsyncExecutionEnv {
        std::filesystem::path ws{"/ws"};
        const std::filesystem::path& workspace() const override { return ws; }
        bool bash_enabled() const override { return true; }

        boost::asio::awaitable<util::Expected<harness::AsyncFileReadResult>> read_file(
            std::string, int, int) override {
            co_return harness::AsyncFileReadResult{};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncFileWriteResult>> write_file(
            std::string, std::string, bool) override {
            co_return harness::AsyncFileWriteResult{};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncFileEditResult>> edit_file(
            std::string, std::string, std::string) override {
            co_return harness::AsyncFileEditResult{};
        }
        boost::asio::awaitable<util::Expected<harness::AsyncShellResult>> run_shell(
            std::string, std::chrono::milliseconds) override {
            co_return harness::AsyncShellResult{};
        }

        // Override a sampling of pi-shaped methods.
        boost::asio::awaitable<std::expected<harness::FileInfo, harness::FileError>> fileInfo(
            std::string path) override {
            co_return harness::FileInfo{.name = path, .path = "/ws/" + path, .kind = harness::FileKind::File};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> readTextFile(
            std::string path) override {
            co_return path + "-content";
        }
        boost::asio::awaitable<std::expected<harness::ShellExecResult, harness::ExecutionError>> exec(
            std::string cmd, harness::ExecOptions) override {
            co_return harness::ShellExecResult{cmd, "", 0};
        }
    };

    CompleteFake fake;

    auto info = run_awaitable_pi(fake.fileInfo("readme.md"));
    REQUIRE(info);
    CHECK(info->name == "readme.md");
    CHECK(info->kind == harness::FileKind::File);

    auto text = run_awaitable_pi(fake.readTextFile("f"));
    REQUIRE(text);
    CHECK(*text == "f-content");

    auto sh = run_awaitable_pi(fake.exec("pwd", harness::ExecOptions{}));
    REQUIRE(sh);
    CHECK(sh->stdout_output == "pwd");
    CHECK(sh->exitCode == 0);
}
