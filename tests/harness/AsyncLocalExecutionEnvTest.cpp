#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include <cch/harness/LocalExecutionEnv.hpp>
#include "harness/SyncLocalExecutionEnv.hpp"
#include "util/Process.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

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

TEST_CASE("process runner capability follows async naming and ownership rules", "[harness][async][issue75]") {
    static_assert(std::is_abstract_v<util::AsyncProcessRunner>);
    static_assert(std::is_final_v<util::DefaultAsyncProcessRunner>);
}

TEST_CASE("async local execution env preserves file read and write safety", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path());

    auto written = run_awaitable_pi(env.writeFile("nested/note.txt", std::string{"hello"}));
    REQUIRE(written);

    auto read = run_awaitable_pi(env.readTextFile("nested/note.txt"));
    REQUIRE(read);
    CHECK(*read == "hello");

    auto escaped = run_awaitable_pi(env.readTextFile("../outside.txt"));
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == harness::FileErrorCode::PermissionDenied);
}

TEST_CASE("async local execution env runs shell commands concurrently", "[harness][async][u6]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    boost::asio::io_context io;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> first;
    std::optional<std::expected<harness::ShellExecResult, harness::ExecutionError>> second;
    const auto started = std::chrono::steady_clock::now();

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            first = co_await env.exec("sleep 0.6; echo first");
            co_return;
        },
        boost::asio::detached);
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            second = co_await env.exec("sleep 0.6; echo second");
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
    harness::SyncLocalExecutionEnv env(workspace.path(), true, {"CCH_CREDENTIAL", "KIMI_API_KEY"}, runner);

    harness::ExecOptions opts;
    opts.timeout = std::chrono::milliseconds(123);
    auto shell = env.exec("env", std::move(opts));

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
    util::DefaultAsyncProcessRunner runner;
    util::ProcessRequest request;
    request.command = "printf '%60000s' '' | tr ' ' x";
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

TEST_CASE("focused fake implements the complete pi-shaped surface", "[harness][u1][issue69]") {
    // ADR 0006: the contract has no tool-shaped methods and no default
    // NotSupported bodies, so a fake must satisfy the full pi-shaped
    // capability set at compile time.
    struct FocusedFake : harness::AsyncExecutionEnv {
        std::filesystem::path ws{"/tmp/ws"};
        const std::filesystem::path& workspace() const override { return ws; }

        boost::asio::awaitable<std::expected<std::string, harness::FileError>> absolutePath(
            std::string path) override {
            co_return path;
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> joinPath(
            std::vector<std::string>) override {
            co_return std::string{"/joined"};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> readTextFile(
            std::string) override {
            co_return std::string{"fake"};
        }
        boost::asio::awaitable<std::expected<std::vector<std::string>, harness::FileError>> readTextLines(
            std::string, std::optional<int>) override {
            co_return std::vector<std::string>{"fake"};
        }
        boost::asio::awaitable<std::expected<harness::BinaryData, harness::FileError>> readBinaryFile(
            std::string) override {
            co_return harness::BinaryData{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> writeFile(
            std::string, harness::WriteContent) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> appendFile(
            std::string, harness::WriteContent) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<harness::FileInfo, harness::FileError>> fileInfo(
            std::string path) override {
            co_return harness::FileInfo{.name = path, .path = "/tmp/ws/" + path};
        }
        boost::asio::awaitable<std::expected<std::vector<harness::FileInfo>, harness::FileError>> listDir(
            std::string) override {
            co_return std::vector<harness::FileInfo>{};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> canonicalPath(
            std::string path) override {
            co_return path;
        }
        boost::asio::awaitable<std::expected<bool, harness::FileError>> exists(
            std::string) override {
            co_return true;
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> createDir(
            std::string, bool) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> remove(
            std::string, bool) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempDir(
            std::optional<std::string>) override {
            co_return std::string{"/tmp/ws/tmp"};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempFile(
            std::optional<std::string>, std::optional<std::string>) override {
            co_return std::string{"/tmp/ws/tmp-file"};
        }

        boost::asio::awaitable<std::expected<harness::ShellExecResult, harness::ExecutionError>> exec(
            std::string command, harness::ExecOptions) override {
            co_return harness::ShellExecResult{.stdout_output = command, .stderr_output = "", .exitCode = 0};
        }
    };

    FocusedFake fake;
    CHECK(fake.workspace() == "/tmp/ws");

    auto read = run_awaitable_pi(fake.readTextFile("x"));
    REQUIRE(read);
    CHECK(*read == "fake");

    auto info = run_awaitable_pi(fake.fileInfo("x"));
    REQUIRE(info);
    CHECK(info->name == "x");

    auto exec_result = run_awaitable_pi(fake.exec("ls", harness::ExecOptions{}));
    REQUIRE(exec_result);
    CHECK(exec_result->stdout_output == "ls");

    // cleanup() keeps its best-effort default body.
    boost::asio::io_context io;
    bool cleaned = false;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            co_await fake.cleanup();
            cleaned = true;
            co_return;
        },
        boost::asio::detached);
    io.run();
    CHECK(cleaned);
}

TEST_CASE("complete fake can override full pi-shaped capability surface", "[harness][u1]") {
    struct CompleteFake : harness::AsyncExecutionEnv {
        std::filesystem::path ws{"/ws"};
        const std::filesystem::path& workspace() const override { return ws; }

        boost::asio::awaitable<std::expected<std::string, harness::FileError>> absolutePath(
            std::string path) override {
            co_return path;
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> joinPath(
            std::vector<std::string>) override {
            co_return std::string{"/joined"};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> readTextFile(
            std::string path) override {
            co_return path + "-content";
        }
        boost::asio::awaitable<std::expected<std::vector<std::string>, harness::FileError>> readTextLines(
            std::string, std::optional<int>) override {
            co_return std::vector<std::string>{};
        }
        boost::asio::awaitable<std::expected<harness::BinaryData, harness::FileError>> readBinaryFile(
            std::string) override {
            co_return harness::BinaryData{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> writeFile(
            std::string, harness::WriteContent) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> appendFile(
            std::string, harness::WriteContent) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<harness::FileInfo, harness::FileError>> fileInfo(
            std::string path) override {
            co_return harness::FileInfo{.name = path, .path = "/ws/" + path, .kind = harness::FileKind::File};
        }
        boost::asio::awaitable<std::expected<std::vector<harness::FileInfo>, harness::FileError>> listDir(
            std::string) override {
            co_return std::vector<harness::FileInfo>{};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> canonicalPath(
            std::string path) override {
            co_return path;
        }
        boost::asio::awaitable<std::expected<bool, harness::FileError>> exists(
            std::string) override {
            co_return true;
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> createDir(
            std::string, bool) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<void, harness::FileError>> remove(
            std::string, bool) override {
            co_return std::expected<void, harness::FileError>{};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempDir(
            std::optional<std::string>) override {
            co_return std::string{"/ws/tmp"};
        }
        boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempFile(
            std::optional<std::string>, std::optional<std::string>) override {
            co_return std::string{"/ws/tmp-file"};
        }

        boost::asio::awaitable<std::expected<harness::ShellExecResult, harness::ExecutionError>> exec(
            std::string cmd, harness::ExecOptions) override {
            co_return harness::ShellExecResult{.stdout_output = cmd, .stderr_output = "", .exitCode = 0};
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

// ---------------------------------------------------------------------------
// U3: pi-shaped shell exec tests
// ---------------------------------------------------------------------------

TEST_CASE("pi-shaped exec returns split stdout and stderr streams", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    auto result = run_awaitable_pi(env.exec("echo hello && echo error >&2"));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->stderr_output.find("error") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec stdout-only and stderr-only preserve empty unused stream", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

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
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "sub";
    auto result = run_awaitable_pi(env.exec("cat note.txt", std::move(opts)));
    REQUIRE(result);
    CHECK(result->stdout_output.find("hello") != std::string::npos);
    CHECK(result->exitCode == 0);
}

TEST_CASE("pi-shaped exec rejects cwd that escapes workspace", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    harness::ExecOptions opts;
    opts.cwd = "../outside";
    auto result = run_awaitable_pi(env.exec("pwd", std::move(opts)));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::SpawnError);
}

TEST_CASE("pi-shaped exec returns shell_unavailable when bash is disabled", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), false);

    auto result = run_awaitable_pi(env.exec("echo nope"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == harness::ExecutionErrorCode::ShellUnavailable);
}

TEST_CASE("pi-shaped exec times out without blocking io context", "[harness][u3]") {
    tests::TempWorkspace workspace;
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);
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
    harness::AsyncLocalExecutionEnv env(workspace.path(), true);

    auto result = run_awaitable_pi(env.exec("exit 42"));
    REQUIRE(result);
    CHECK(result->exitCode == 42);
}
