#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/LocalFileSystem.hpp>
#include <cch/agent/harness/LocalShell.hpp>
#include <cch/agent/tools/ToolFactories.hpp>
#include "agent/ToolArgumentPreparation.hpp"
#include "support/AsyncResultBridge.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "support/Json.hpp"
#include "agent/harness/OutputLimiter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <thread>

using namespace cch;

namespace {

template <typename T>
[[nodiscard]] support::AsyncResult<T, harness::FileError> ready_file(T value) {
    return support::AsyncResult<T, harness::FileError>{
        std::expected<T, harness::FileError>{std::move(value)}};
}

[[nodiscard]] inline support::AsyncResult<void, harness::FileError> ready_file() {
    return support::AsyncResult<void, harness::FileError>{
        std::expected<void, harness::FileError>{}};
}

class CapturingFileSystem final : public harness::AsyncFileSystem {
public:
    explicit CapturingFileSystem(std::filesystem::path workspace_path) : workspace_path_(std::move(workspace_path)) {}
    const std::filesystem::path& workspace() const override { return workspace_path_; }
    support::AsyncResult<std::string, harness::FileError> absolutePath(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
    support::AsyncResult<std::string, harness::FileError> joinPath(std::vector<std::string>, std::stop_token) override { return ready_file(std::string{}); }
    support::AsyncResult<std::string, harness::FileError> readTextFile(
            std::string, std::stop_token stop_token) override {
        last_stop_token = stop_token;
        return ready_file(std::string{});
    }
    support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(std::string, std::optional<int>, std::stop_token stop_token) override { last_stop_token = stop_token; return ready_file(std::vector<std::string>{}); }
    support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(std::string, std::stop_token) override { return ready_file(harness::BinaryData{}); }
    support::AsyncResult<void, harness::FileError> writeFile(std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        last_stop_token = stop_token;
        last_write_path = std::move(path);
        if (const auto* text = std::get_if<std::string>(&content)) last_write_content = *text;
        return ready_file();
    }
    support::AsyncResult<void, harness::FileError> appendFile(std::string, harness::WriteContent, std::stop_token) override { return ready_file(); }
    support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(std::string, std::stop_token) override { return ready_file(harness::FileInfo{}); }
    support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(std::string, std::stop_token) override { return ready_file(std::vector<harness::FileInfo>{}); }
    support::AsyncResult<std::string, harness::FileError> canonicalPath(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
    support::AsyncResult<bool, harness::FileError> exists(std::string, std::stop_token) override { return ready_file(true); }
    support::AsyncResult<void, harness::FileError> createDir(std::string, bool, std::stop_token) override { return ready_file(); }
    support::AsyncResult<void, harness::FileError> remove(std::string, bool, std::stop_token) override { return ready_file(); }
    support::AsyncResult<std::string, harness::FileError> createTempDir(std::optional<std::string>, std::stop_token) override { return ready_file(std::string{}); }
    support::AsyncResult<std::string, harness::FileError> createTempFile(std::optional<std::string>, std::optional<std::string>, std::stop_token) override { return ready_file(std::string{}); }
    support::AsyncResult<void, harness::FileError> cleanup() override { return ready_file(); }

    std::stop_token last_stop_token;
    std::string last_write_path;
    std::string last_write_content;

private:
    std::filesystem::path workspace_path_;
};

class CapturingShell final : public harness::AsyncShell {
public:
    support::AsyncResult<harness::ShellExecResult, harness::ExecutionError> exec(std::string command, harness::ExecOptions options) override {
        last_command = std::move(command);
        last_timeout = options.timeout.value_or(std::chrono::milliseconds{0});
        last_stop_token = options.stop_token;
        last_env = options.env;
        if (options.onStdout && !streamed_stdout.empty()) {
            if (auto delivered = (*options.onStdout)(streamed_stdout); !delivered) {
                return support::AsyncResult<harness::ShellExecResult, harness::ExecutionError>{
                    std::unexpected(harness::ExecutionError{
                        .code = harness::ExecutionErrorCode::CallbackError,
                        .message = delivered.error().message,
                    })};
            }
        }
        if (options.onStderr && !streamed_stderr.empty()) {
            if (auto delivered = (*options.onStderr)(streamed_stderr); !delivered) {
                return support::AsyncResult<harness::ShellExecResult, harness::ExecutionError>{
                    std::unexpected(harness::ExecutionError{
                        .code = harness::ExecutionErrorCode::CallbackError,
                        .message = delivered.error().message,
                    })};
            }
        }
        if (next_exec_error) {
            return support::AsyncResult<harness::ShellExecResult, harness::ExecutionError>{
                    std::unexpected(*next_exec_error)};
        }
        return support::AsyncResult<harness::ShellExecResult, harness::ExecutionError>{
            std::expected<harness::ShellExecResult, harness::ExecutionError>{next_shell_result}};
    }

    std::string last_command;
    std::chrono::milliseconds last_timeout{0};
    std::stop_token last_stop_token;
    std::optional<std::map<std::string, std::string>> last_env;
    std::string streamed_stdout;
    std::string streamed_stderr;
    std::optional<harness::ExecutionError> next_exec_error;
    harness::ShellExecResult next_shell_result{.stdout_output = "ok", .stderr_output = "", .exitCode = 0};
};

class TestRuntime final {
public:
    TestRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()),
          work_guard_(boost::asio::make_work_guard(*loop_)),
          root_(loop_, harness::RuntimeLimits{}) {}

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() {
        return root_.make_target();
    }

    boost::asio::io_context& loop() noexcept { return *loop_; }

private:
    std::shared_ptr<boost::asio::io_context> loop_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    harness::RuntimeRoot root_;
};

TestRuntime& test_runtime() {
    static TestRuntime runtime;
    return runtime;
}

[[nodiscard]] std::shared_ptr<harness::RuntimeTarget> test_runtime_target() {
    return test_runtime().make_target();
}

template <typename Start>
support::Expected<agent::AsyncToolExecutionResult> run_tool(Start start) {
    auto& loop = test_runtime().loop();
    loop.restart();
    std::optional<support::Expected<agent::AsyncToolExecutionResult>> result;
    boost::asio::co_spawn(
            loop,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(start());
                co_return;
            },
            boost::asio::detached);
    while (!result) {
        loop.run_one();
    }
    // Drain posted completion teardown (mailbox requeues, producer frame
    // cleanup) before returning: work still queued here would linger in the
    // shared static runtime past the test's end (ASan leak reports, #473).
    while (loop.poll_one() != 0) {
    }
    return std::move(*result);
}

agent::ToolInvocation invocation(std::string name, std::string json) {
    auto args = support::read_json(json);
    REQUIRE(args);
    return agent::ToolInvocation{"call-1", std::move(name), std::move(*args), std::move(json)};
}

/// The whole `details.truncation` object, asserting its presence. A helper that
/// returns the sub-object keeps every content assertion below a full-text one
/// instead of a substring that an empty result would satisfy.
const support::JsonValue& truncation_details(const agent::AsyncToolExecutionResult& result) {
    REQUIRE(result.details);
    const auto& object = result.details->get_object();
    REQUIRE(object.count("truncation") == 1);
    return object.at("truncation");
}

std::string truncated_by(const support::JsonValue& truncation) { return truncation.at("truncatedBy").get_string(); }

[[nodiscard]] std::string read_file_at(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// `n` numbered lines joined by "\n" with a trailing newline, the shape both
/// fixtures below need.
[[nodiscard]] std::string numbered_lines(int count) {
    std::string text;
    for (int line = 1; line <= count; ++line) {
        text += "line" + std::to_string(line) + "\n";
    }
    return text;
}

} // namespace

TEST_CASE("built-in tools default to exclusive execution", "[tools][async][spec]") {
    tests::TempWorkspace workspace;
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    auto shell = std::make_shared<CapturingShell>();

    auto read = tools::make_async_read_file_tool(filesystem);
    auto write = tools::make_async_write_file_tool(filesystem);
    auto edit = tools::make_async_edit_tool(filesystem);
    auto bash = tools::make_async_bash_tool(shell, filesystem);

    CHECK(read.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(write.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(edit.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(bash.concurrency == agent::ToolConcurrency::Exclusive);
}

TEST_CASE("async read_file tool uses Glaze typed args", "[tools][async][u6][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "line1\nline2\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("read_file", R"({"path":"note.txt","offset":2,"limit":1})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "line2");
}

TEST_CASE("async edit tool applies disjoint edits and returns pi-shaped diff details",
        "[tools][async][issue354][compat-pi]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "alpha\nbeta\ngamma\ndelta\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("edit",
                R"({"path":"edit.txt","edits":[{"oldText":"alpha\n","newText":"ALPHA\n"},)"
                R"({"oldText":"gamma\n","newText":"GAMMA\n"}]})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // pi edit.ts content line and details shape: diff, patch, firstChangedLine.
    CHECK(ai::text_from_content(result->content) ==
          "Successfully replaced 2 block(s) in edit.txt.");
    REQUIRE(result->details);
    const auto& details = result->details->get_object();
    const auto diff = details.at("diff").get_string();
    const auto patch = details.at("patch").get_string();
    // The display diff pins the exact pi generateDiffString output: removed
    // and added lines with line numbers, unchanged lines as context.
    CHECK(diff ==
          "-1 alpha\n"
          "+1 ALPHA\n"
          " 2 beta\n"
          "-3 gamma\n"
          "+3 GAMMA\n"
          " 4 delta\n");
    CHECK(details.at("firstChangedLine").get_number() == 1);
    // The unified patch is a standard two-file patch that applies.
    CHECK(patch ==
          "--- edit.txt\n"
          "+++ edit.txt\n"
          "@@ -1,4 +1,4 @@\n"
          "-alpha\n"
          "+ALPHA\n"
          " beta\n"
          "-gamma\n"
          "+GAMMA\n"
          " delta\n");
    CHECK(workspace.read("edit.txt") == "ALPHA\nbeta\nGAMMA\ndelta\n");
}

TEST_CASE("async edit tool matches every edit against the original and rejects overlaps",
        "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "one\ntwo\nthree\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("edit",
                R"({"path":"edit.txt","edits":[{"oldText":"one\ntwo\n","newText":"ONE\nTWO\n"},)"
                R"({"oldText":"two\nthree\n","newText":"TWO\nTHREE\n"}]})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("overlap") != std::string::npos);
    // The failed call never touches the file.
    CHECK(workspace.read("edit.txt") == "one\ntwo\nthree\n");
}

TEST_CASE("async edit tool rejects missing and duplicate target text with pi messages",
        "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "foo foo foo");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto missing = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"edit.txt","edits":[{"oldText":"bar","newText":"baz"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(missing);
    CHECK(missing->is_error);
    CHECK(ai::text_from_content(missing->content).find(
        "Could not find the exact text in edit.txt.") != std::string::npos);

    auto duplicate = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"edit.txt","edits":[{"oldText":"foo","newText":"bar"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(duplicate);
    CHECK(duplicate->is_error);
    CHECK(ai::text_from_content(duplicate->content).find(
        "Found 3 occurrences of the text in edit.txt.") != std::string::npos);
    CHECK(workspace.read("edit.txt") == "foo foo foo");
}

TEST_CASE("async edit tool preserves BOM and CRLF line endings", "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "\xef\xbb\xbf" "one\r\ntwo\r\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"edit.txt","edits":[{"oldText":"two","newText":"TWO"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(workspace.read("edit.txt") == "\xef\xbb\xbf" "one\r\nTWO\r\n");
}

TEST_CASE("async edit tool fuzzy-matches smart-quote and dash variants", "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    // The file carries smart quotes, an em dash, and trailing line
    // whitespace; the edit uses ASCII forms without the trailing space,
    // matching pi's fuzzy normalization.
    workspace.write("note.txt", "say \xe2\x80\x9chello\xe2\x80\x9d world\xe2\x80\x94today   \n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"note.txt","edits":[{"oldText":"say \"hello\" world-today","newText":"fixed"}]})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The fuzzy replacement rewrites only the matched line and keeps the
    // unchanged parts of the file byte-identical.
    CHECK(workspace.read("note.txt") == "fixed\n");
}

TEST_CASE("async edit tool rejects empty oldText with pi's message", "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "content\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"note.txt","edits":[{"oldText":"","newText":"x"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find(
        "oldText must not be empty in note.txt.") != std::string::npos);
    CHECK(workspace.read("note.txt") == "content\n");
}

TEST_CASE("async edit tool rejects no-change edits with pi's message", "[tools][async][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"note.txt","edits":[{"oldText":"same","newText":"same"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find(
        "No changes made to note.txt.") != std::string::npos);
    CHECK(workspace.read("note.txt") == "same\n");
}

TEST_CASE(
        "edit declared contract validation and execution acceptance agree", "[tools][async][issue77][issue354][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    // The agent loop validates every call with prepare_tool_arguments before
    // execution (ADR 0007); execution must accept exactly what it accepts.
    auto contract_accepts = [&](const std::string& json) {
        ai::ToolCallContent call{
            .id = "call-1",
            .name = "edit",
            .arguments = std::nullopt,
            .raw_arguments = json,
            .thought_signature = std::nullopt,
            .argument_error = std::nullopt,
        };
        return agent::prepare_tool_arguments(tool.definition, call).has_value();
    };
    auto execution_accepts = [&](const std::string& json) {
        auto result = run_tool([&]() {
            return tool.execute(invocation("edit", json), std::stop_token{}, agent::ToolUpdateSink{});
        });
        REQUIRE(result);
        return !result->is_error;
    };

    // The declared edits[] form is accepted by both layers.
    const std::string valid = R"({"path":"note.txt","edits":[{"oldText":"hello","newText":"hi"}]})";
    CHECK(contract_accepts(valid));
    CHECK(execution_accepts(valid));
    CHECK(workspace.read("note.txt") == "hi world\n");

    // The deleted legacy fallback is rejected by both layers.
    const std::string legacy = R"({"path":"note.txt","old_text":"hi","new_text":"hey"})";
    CHECK_FALSE(contract_accepts(legacy));
    CHECK_FALSE(execution_accepts(legacy));

    // Legacy fields stay rejected even next to a valid edits[] array.
    const std::string mixed =
        R"({"path":"note.txt","edits":[{"oldText":"hi","newText":"hey"}],"old_text":"a","new_text":"b"})";
    CHECK_FALSE(contract_accepts(mixed));
    CHECK_FALSE(execution_accepts(mixed));

    // An empty edits[] array satisfies neither the contract nor execution.
    const std::string empty_edits = R"({"path":"note.txt","edits":[]})";
    CHECK_FALSE(contract_accepts(empty_edits));
    CHECK_FALSE(execution_accepts(empty_edits));

    // A missing path is rejected by both layers.
    const std::string missing_path = R"({"edits":[{"oldText":"hi","newText":"hey"}]})";
    CHECK_FALSE(contract_accepts(missing_path));
    CHECK_FALSE(execution_accepts(missing_path));
}

TEST_CASE("async tools prefer structured arguments over raw provider text", "[tools][async][u6][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("structured.txt", "from-structured");
    workspace.write("raw.txt", "from-raw");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto structured = support::read_json(R"({"path":"structured.txt"})");
    REQUIRE(structured);
    agent::ToolInvocation call{"call-1", "read_file", *structured, R"({"path":"raw.txt"})"};

    auto result = run_tool([&]() {
        return tool.execute(std::move(call), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "from-structured");
}

TEST_CASE("async bash tool preserves its visible command and carries execution options",
        "[tools][async][issue40][issue84][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    auto tool = tools::make_async_bash_tool(shell, filesystem);
    std::stop_source stop_source;

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi","timeout":5})"),
            stop_source.get_token(), agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(shell->last_command == "echo hi");
    CHECK(shell->last_timeout == std::chrono::milliseconds(5000));
    CHECK(shell->last_stop_token == stop_source.get_token());
}

TEST_CASE("async bash tool exposes live PI_* session facts when a session environment is provided",
        "[tools][async][issue414][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    auto session_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    session_environment->session_id = "session-42";
    session_environment->session_file = "/tmp/session-42.jsonl";
    session_environment->provider = "openai-codex";
    session_environment->model = "gpt-5.2-codex";
    session_environment->reasoning_level = "high";
    auto tool = tools::make_async_bash_tool(shell, filesystem, session_environment);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"env | grep PI_"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(shell->last_env.has_value());
    CHECK(shell->last_env->at("PI_SESSION_ID") == "session-42");
    CHECK(shell->last_env->at("PI_SESSION_FILE") == "/tmp/session-42.jsonl");
    CHECK(shell->last_env->at("PI_PROVIDER") == "openai-codex");
    CHECK(shell->last_env->at("PI_MODEL") == "gpt-5.2-codex");
    CHECK(shell->last_env->at("PI_REASONING_LEVEL") == "high");
}

TEST_CASE("async bash tool shadows absent PI_* facts with empty values and injects nothing without a holder",
        "[tools][async][issue414][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    auto session_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    session_environment->session_id = "session-7";
    auto tool = tools::make_async_bash_tool(shell, filesystem, session_environment);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    REQUIRE(shell->last_env.has_value());
    CHECK(shell->last_env->at("PI_SESSION_ID") == "session-7");
    CHECK(shell->last_env->at("PI_SESSION_FILE") == "");
    CHECK(shell->last_env->at("PI_PROVIDER") == "");
    CHECK(shell->last_env->at("PI_MODEL") == "");
    CHECK(shell->last_env->at("PI_REASONING_LEVEL") == "");

    // Without a session environment the tool injects no environment at all
    // (pi `exposeSessionEnvironment: false`).
    auto plain_tool = tools::make_async_bash_tool(shell, filesystem);
    auto plain_result = run_tool([&]() {
        return plain_tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(plain_result);
    CHECK_FALSE(shell->last_env.has_value());
}

TEST_CASE("async bash tool returns clean output with no exit-code prefix and no details",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->streamed_stdout = "first\nsecond\n";
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"emit-two-lines"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The whole content, so the deleted `exit_code=0` prefix cannot reappear
    // unnoticed, and `truncated=true` cannot ride along either.
    CHECK(ai::text_from_content(result->content) == "first\nsecond\n");
    // pi: a clean exit with no truncation carries no details at all.
    CHECK_FALSE(result->details);
    // No spill: nothing was truncated.
    CHECK(filesystem->last_write_path.empty());
}

TEST_CASE("async bash tool renders empty clean output as pi's literal (no output)", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->next_shell_result.stdout_output = "";
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(invocation("bash", R"({"command":"true"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The literal pi `formatOutput` default, not an empty string: a test that
    // only asserted "no exit_code= in the text" would pass on either.
    CHECK(ai::text_from_content(result->content) == "(no output)");
    CHECK_FALSE(result->details);
}

TEST_CASE("async bash tool truncates from the tail with pi's line-limit summary and a spill file",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    // A real filesystem: the spill path's existence and contents are part of
    // the model contract, so a capture double that records the write proves
    // nothing about the file.
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    const harness::OutputLimit limit;
    shell->streamed_stdout = numbered_lines(static_cast<int>(limit.max_lines) + 100);
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"emit-large-output"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(result->details);
    const auto& object = result->details->get_object();
    REQUIRE(object.count("fullOutputPath") == 1);
    const auto path = object.at("fullOutputPath").get_string();
    // pi `output-accumulator.ts:19-22`: the spill lives in the OS temp
    // directory as `<prefix>-<id>.log`; the working directory is not involved.
    CHECK(path.starts_with(std::filesystem::temp_directory_path().string() + "/"));
    CHECK(path.ends_with(".log"));
    CHECK_FALSE(path.find("bash-output-") != std::string::npos);
    // The path the model reads is the same string details carries.
    const auto visible = ai::text_from_content(result->content);
    const int total_lines = static_cast<int>(limit.max_lines) + 100;
    const int start_line = total_lines - static_cast<int>(limit.max_lines) + 1;
    std::string expected;
    for (int line = start_line; line <= total_lines; ++line) {
        expected += "line" + std::to_string(line);
        if (line != total_lines) {
            expected += '\n';
        }
    }
    expected += "\n\n[Showing lines " + std::to_string(start_line) + "-" + std::to_string(total_lines) + " of " +
                std::to_string(total_lines) + ". Full output: " + path + "]";
    CHECK(visible == expected);
    // The deleted markers are gone, asserted on the full text above and here as
    // the property they used to carry.
    CHECK(visible.find("exit_code=") == std::string::npos);
    CHECK(visible.find("truncated=true") == std::string::npos);
    CHECK(visible.find("[output truncated") == std::string::npos);

    const auto& truncation = object.at("truncation");
    CHECK(truncation.at("truncated").get_boolean());
    CHECK(truncated_by(truncation) == "lines");
    CHECK(truncation.at("totalLines").get_number() == static_cast<double>(total_lines));
    CHECK(truncation.at("outputLines").get_number() == static_cast<double>(limit.max_lines));
    CHECK(truncation.at("maxLines").get_number() == static_cast<double>(limit.max_lines));
    CHECK(truncation.at("maxBytes").get_number() == static_cast<double>(limit.max_bytes));
    CHECK_FALSE(truncation.at("lastLinePartial").get_boolean());
    CHECK_FALSE(truncation.at("firstLineExceedsLimit").get_boolean());
    // `content` must not travel into details: it is the model-facing text.
    CHECK(object.count("content") == 0);
    CHECK(truncation.get_object().count("content") == 0);

    // The file exists and holds the complete output, not the visible tail.
    std::error_code exists_error;
    REQUIRE(std::filesystem::exists(path, exists_error));
    CHECK_FALSE(exists_error);
    const auto spilled = read_file_at(path);
    CHECK(spilled == shell->streamed_stdout);
    // The spill is the complete output while the visible text is only the last
    // 2000 lines, so the file is strictly the longer one here.
    CHECK(spilled.size() > visible.size());
    CHECK(visible.size() <= limit.max_bytes);
}

TEST_CASE("async bash tool truncates on the byte limit and names the limit in the summary",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    const harness::OutputLimit limit;
    // Ten 8 KB lines: under the line limit, far over the byte limit, and the
    // last line is whole, so the byte-limit variant of the summary is the one
    // pi emits.
    shell->streamed_stdout.clear();
    for (int line = 0; line < 10; ++line) {
        shell->streamed_stdout += std::string(8 * 1024, static_cast<char>('a' + line)) + "\n";
    }
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"emit-wide-output"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(result->details);
    const auto& object = result->details->get_object();
    const auto path = object.at("fullOutputPath").get_string();
    const auto& truncation = object.at("truncation");
    CHECK(truncated_by(truncation) == "bytes");
    // 6 * 8193 = 49158 fits; the 7th line crosses 50 KB.
    const auto output_lines = 6u;
    const auto total_lines = 10u;
    const auto start_line = total_lines - output_lines + 1;
    std::string expected;
    for (unsigned index = 0; index < output_lines; ++index) {
        expected += std::string(8 * 1024, static_cast<char>('a' + total_lines - output_lines + index)) + "\n";
    }
    expected.pop_back();
    expected += "\n\n[Showing lines " + std::to_string(start_line) + "-" + std::to_string(total_lines) + " of " +
                std::to_string(total_lines) + " (50.0KB limit). Full output: " + path + "]";
    CHECK(ai::text_from_content(result->content) == expected);
    CHECK(read_file_at(path) == shell->streamed_stdout);
}

TEST_CASE("async bash tool reports a cut final line with pi's last-line summary", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    const harness::OutputLimit limit;
    // Two complete lines then one 80 KB line: the tail of that final line is
    // all that survives, which is the third of pi's three summary forms.
    shell->streamed_stdout = "first\nsecond\n" + std::string(80 * 1024, 'z');
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"emit-one-wide-line"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(result->details);
    const auto& object = result->details->get_object();
    const auto path = object.at("fullOutputPath").get_string();
    const auto& truncation = object.at("truncation");
    CHECK(truncation.at("lastLinePartial").get_boolean());
    CHECK(truncated_by(truncation) == "bytes");
    // 50.0KB of the 80.0KB final line, whose full size is named alongside.
    CHECK(ai::text_from_content(result->content) ==
            std::string(limit.max_bytes, 'z') +
                    "\n\n[Showing last 50.0KB of line 3 (line is 80.0KB). Full output: " + path + "]");
    CHECK(read_file_at(path) == shell->streamed_stdout);
}

TEST_CASE("async bash tool spill file holds the complete redacted output", "[tools][async][issue73][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    const harness::OutputLimit limit;
    shell->streamed_stdout = std::string(limit.max_bytes + 100, 'x') + "\napi_key=super-secret\ncomplete-tail\xc3\xa9";
    shell->next_shell_result.stdout_output = shell->streamed_stdout.substr(0, limit.max_bytes);
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const auto visible = ai::text_from_content(result->content);
    // Redaction is unchanged by the contract change, and it holds on the
    // visible text as well as on the spill.
    CHECK(visible.find("super-secret") == std::string::npos);
    CHECK(visible.find("[REDACTED]") != std::string::npos);
    CHECK(visible.size() <= limit.max_bytes + 200);
    REQUIRE(result->details);
    const auto path = result->details->get_object().at("fullOutputPath").get_string();
    CHECK(visible.find(path) != std::string::npos);
    CHECK(filesystem->last_write_path == path);
    CHECK(filesystem->last_write_content.find("super-secret") == std::string::npos);
    CHECK(filesystem->last_write_content.find("[REDACTED]") != std::string::npos);
    CHECK(filesystem->last_write_content.size() > limit.max_bytes);
    CHECK(filesystem->last_write_content.ends_with("complete-tail\xc3\xa9"));
}

TEST_CASE("async bash tool without streamed output uses the execution-layer result and still spills",
        "[tools][async][issue73][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    const harness::OutputLimit limit;
    // streamed_stdout/streamed_stderr stay empty, so the fake Shell never
    // fires the streaming callbacks and only the result fields exist. pi's
    // accumulator has the same complete output in this case, so the tool
    // formats and spills exactly as the streamed path does; what changed is
    // that the "capped at execution layer" marker no longer exists.
    shell->next_shell_result.stdout_output =
            std::string(limit.max_bytes + 100, 'x') + "\napi_key=super-secret\ncomplete-tail";
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(result->details);
    const auto& object = result->details->get_object();
    const auto path = object.at("fullOutputPath").get_string();
    const auto visible = ai::text_from_content(result->content);
    CHECK(visible.find("capped at execution layer") == std::string::npos);
    CHECK(visible.find("super-secret") == std::string::npos);
    CHECK(visible.find("complete-tail") != std::string::npos);
    CHECK(visible.ends_with(". Full output: " + path + "]"));
    // The spill is the redacted complete output, so the secret is gone from it
    // too even though the raw result field carried it.
    const auto spilled = read_file_at(path);
    CHECK(spilled.find("super-secret") == std::string::npos);
    CHECK(spilled.find("[REDACTED]") != std::string::npos);
    CHECK(spilled == std::string(limit.max_bytes + 100, 'x') + "\napi_key=[REDACTED]\ncomplete-tail");
    std::error_code exists_error;
    REQUIRE(std::filesystem::exists(path, exists_error));
    CHECK_FALSE(exists_error);
}

TEST_CASE("async bash tool returns a non-zero exit as pi's error text", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->streamed_stdout = "partial work\n";
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    shell->next_shell_result.exitCode = 3;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(invocation("bash", R"({"command":"fail"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    // pi `appendStatus`: the output, a blank line, then the status. The
    // streamed output already ends in a newline, so the separator is the
    // blank line pi adds on top of it.
    CHECK(ai::text_from_content(result->content) == "partial work\n\n\nCommand exited with code 3");
    CHECK_FALSE(result->details);
}

TEST_CASE("async bash tool reports a silent non-zero exit with pi's (no output) default",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->next_shell_result.stdout_output = "";
    shell->next_shell_result.exitCode = 127;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"not-found"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    // The default applies on the exit-code paths, so the model sees both parts.
    CHECK(ai::text_from_content(result->content) == "(no output)\n\nCommand exited with code 127");
}

TEST_CASE("async bash tool reports a missing exit code with pi's own status", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->streamed_stdout = "before the signal\n";
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    // `harness::process_exit_code` reports an unknown child status as a
    // negative code; that is this tool's null exit code, and pi decides it
    // before the non-zero check.
    shell->next_shell_result.exitCode = -1;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(invocation("bash", R"({"command":"stopped"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content) == "before the signal\n\n\nCommand terminated without an exit code");
}

TEST_CASE("async bash tool reports an abort as pi's bare status with no (no output) default",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->next_exec_error = harness::ExecutionError{
            .code = harness::ExecutionErrorCode::Aborted,
            .message = "Operation aborted",
    };
    shell->next_shell_result.stdout_output = "";
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"sleep 100"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    // Exactly the status, with the `(no output)` default deliberately
    // suppressed: the full-text assertion is the point of this case.
    CHECK(ai::text_from_content(result->content) == "Command aborted");
}

TEST_CASE(
        "async bash tool reports an abort after output as pi's output plus status", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->streamed_stdout = "so far\n";
    shell->next_exec_error = harness::ExecutionError{
            .code = harness::ExecutionErrorCode::Aborted,
            .message = "Operation aborted",
    };
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"sleep 100"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content) == "so far\n\n\nCommand aborted");
}

TEST_CASE("async bash tool reports a timeout as pi's timed-out status", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->next_exec_error = harness::ExecutionError{
            .code = harness::ExecutionErrorCode::Timeout,
            .message = "shell command timed out",
    };
    shell->next_shell_result.stdout_output = "";
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(invocation("bash", R"({"command":"sleep 100","timeout":5})"),
                std::stop_token{},
                agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    // Exactly the status: a timeout carries no `(no output)` prefix either.
    CHECK(ai::text_from_content(result->content) == "Command timed out after 5 seconds");
}

TEST_CASE("async bash tool keeps a non-abort execution error's own text", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    // pi rethrows every other exec error unchanged; only the abort and timeout
    // sentinels become pi status strings.
    shell->next_exec_error = harness::ExecutionError{
            .code = harness::ExecutionErrorCode::SpawnError,
            .message = "fork failed",
    };
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("bash", R"({"command":"anything"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content) == "fork failed");
}

TEST_CASE("async bash tool strips ANSI escape sequences", "[tools][async][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<CapturingShell>();
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    shell->streamed_stdout = "\x1b[31mred\x1b[0m";
    shell->next_shell_result.stdout_output = shell->streamed_stdout;
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The whole text, so a reintroduced escape or prefix cannot hide behind a
    // substring match.
    CHECK(ai::text_from_content(result->content) == "red");
}

TEST_CASE("async bash tool is disabled unless the Shell explicitly enables it", "[tools][async][spec]") {
    tests::TempWorkspace workspace;
    auto shell = std::make_shared<harness::AsyncLocalShell>(test_runtime_target(), workspace.path(), false);
    auto filesystem = std::make_shared<CapturingFileSystem>(workspace.path());
    auto tool = tools::make_async_bash_tool(shell, filesystem);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo blocked"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
}

TEST_CASE("async read tool returns a whole untruncated file with no hint and no details",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "line1\nline2\nline3\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"note.txt"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The whole file text and nothing else. A test asserting only that
    // "[Showing" is absent passes on an empty result, so the full content is
    // the assertion.
    CHECK(ai::text_from_content(result->content) == "line1\nline2\nline3\n");
    // pi sets no details in the untruncated branch.
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool without a trailing newline returns the file verbatim", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "no trailing newline");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"note.txt"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "no trailing newline");
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool reports a limit-stopped read with pi's more-lines hint and no details",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "l1\nl2\nl3\nl4\nl5\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("read", R"({"path":"note.txt","limit":2})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // 6 lines counted (5 plus the trailing empty one), 2 read, 4 left.
    CHECK(ai::text_from_content(result->content) == "l1\nl2\n\n[4 more lines in file. Use offset=3 to continue.]");
    // pi deliberately reports no details here even though the text carries a
    // continuation hint; a "helpful" details is a contract change.
    CHECK_FALSE(result->details);
}

TEST_CASE(
        "async read tool reports a limit that consumed the whole file with no hint", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    // No trailing newline, so pi's split has no extra empty line to report.
    workspace.write("note.txt", "l1\nl2\nl3");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("read", R"({"path":"note.txt","limit":3})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The negative twin of the more-lines case: nothing follows the text.
    CHECK(ai::text_from_content(result->content) == "l1\nl2\nl3");
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool reports the line-limit truncation with pi's continuation hint",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    const harness::OutputLimit limit;
    workspace.write("big.txt", numbered_lines(static_cast<int>(limit.max_lines) + 31));
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"big.txt"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const int total_lines = static_cast<int>(limit.max_lines) + 32;
    std::string expected;
    for (int line = 1; line <= static_cast<int>(limit.max_lines); ++line) {
        expected += "line" + std::to_string(line) + "\n";
    }
    expected.pop_back();
    expected += "\n\n[Showing lines 1-" + std::to_string(limit.max_lines) + " of " + std::to_string(total_lines) +
                ". Use offset=" + std::to_string(limit.max_lines + 1) + " to continue.]";
    CHECK(ai::text_from_content(result->content) == expected);
    // The count is the whole file, not the kept slice: a cheap "does it end
    // with a hint" check would not separate those two numbers.
    CHECK(ai::text_from_content(result->content).find("of 2032.") != std::string::npos);

    const auto& truncation = truncation_details(*result);
    CHECK(truncation.at("truncated").get_boolean());
    CHECK(truncated_by(truncation) == "lines");
    CHECK(truncation.at("outputLines").get_number() == static_cast<double>(limit.max_lines));
    // The hint counts the whole file (2032, pi's `read.ts` split), while
    // `truncation.totalLines` counts the selected content pi hands to
    // `truncateHead` (2031, after the trailing empty line is dropped). Pinning
    // both is what keeps them from being conflated.
    CHECK(truncation.at("totalLines").get_number() == 2031);
    CHECK(truncation.at("maxLines").get_number() == static_cast<double>(limit.max_lines));
    CHECK(truncation.at("maxBytes").get_number() == static_cast<double>(limit.max_bytes));
    CHECK_FALSE(truncation.at("lastLinePartial").get_boolean());
    CHECK_FALSE(truncation.at("firstLineExceedsLimit").get_boolean());
}

TEST_CASE("async read tool reports the byte-limit truncation with pi's limit-named hint",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    const harness::OutputLimit limit;
    // 100 lines of 1 KB each: under the 2000-line limit, over the 50 KB byte
    // limit, and no single line is oversized.
    std::string body;
    for (int line = 0; line < 100; ++line) {
        body += std::string(1024, static_cast<char>('a' + line / 10)) + "\n";
    }
    workspace.write("wide.txt", body);
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"wide.txt"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // 49 lines of 1024 bytes plus their 48 joining newlines = 50224, which
    // fits; the 50th crosses 51200.
    const int kept_lines = 49;
    std::string expected;
    for (int line = 0; line < kept_lines; ++line) {
        expected += std::string(1024, static_cast<char>('a' + line / 10));
        if (line != kept_lines - 1) {
            expected += '\n';
        }
    }
    const auto summary = "\n\n[Showing lines 1-" + std::to_string(kept_lines) +
                         " of 101 (50.0KB limit). Use offset=" + std::to_string(kept_lines + 1) + " to continue.]";
    CHECK(ai::text_from_content(result->content) == expected + summary);

    const auto& truncation = truncation_details(*result);
    CHECK(truncated_by(truncation) == "bytes");
    CHECK(truncation.at("outputLines").get_number() == static_cast<double>(kept_lines));
    // `outputBytes` counts the retained text only, which the summary is not
    // part of; asserting the exact figure keeps the two from being conflated.
    CHECK(truncation.at("outputBytes").get_number() == static_cast<double>(expected.size()));
    CHECK(truncation.at("totalBytes").get_number() == 102400 + 100);
    CHECK_FALSE(truncation.at("firstLineExceedsLimit").get_boolean());
}

TEST_CASE("async read tool replaces an oversized first line with pi's sed fallback notice",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    const harness::OutputLimit limit;
    workspace.write("huge.txt", std::string(60 * 1024, 'x'));
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"huge.txt"})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The notice is the whole content, and it carries the model-supplied path
    // and the numeric byte cap, not a formatted size.
    CHECK(ai::text_from_content(result->content) ==
            "[Line 1 is 60.0KB, exceeds 50.0KB limit. Use bash: sed -n '1p' huge.txt | head -c 51200]");

    const auto& truncation = truncation_details(*result);
    CHECK(truncation.at("firstLineExceedsLimit").get_boolean());
    CHECK(truncated_by(truncation) == "bytes");
    CHECK(truncation.at("outputLines").get_number() == 0);
    CHECK(truncation.at("totalBytes").get_number() == static_cast<double>(60 * 1024));
}

TEST_CASE("async read tool names the absolute path argument in the oversized-first-line notice",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    const auto target = workspace.path() / "huge.log";
    workspace.write("huge.log", std::string(60 * 1024, 'x'));
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", "{\"path\":\"" + target.string() + "\"}"),
                std::stop_token{},
                agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // The argument the model supplied, so the suggested sed command is one the
    // model can run as written.
    CHECK(ai::text_from_content(result->content) ==
            "[Line 1 is 60.0KB, exceeds 50.0KB limit. Use bash: sed -n '1p' " + target.string() + " | head -c 51200]");
}

TEST_CASE("async read tool rejects an out-of-range offset with pi's error text", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "l1\nl2\nl3\nl4\nl5\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
                invocation("read", R"({"path":"note.txt","offset":900})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    // The whole error text, with the file's own line count.
    CHECK(ai::text_from_content(result->content) == "Offset 900 is beyond end of file (6 lines total)");
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool accepts the last in-range offset", "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "l1\nl2\nl3\nl4\nl5\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    // The negative twin of the out-of-range case: offset 6 is the trailing empty
    // line pi's split produces, so it is in range and reads as empty.
    auto result = run_tool([&]() {
        return tool.execute(
                invocation("read", R"({"path":"note.txt","offset":6})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content).empty());
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool applies offset and limit together with pi's line arithmetic",
        "[tools][async][issue823][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "l1\nl2\nl3\nl4\nl5\nl6\n");
    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("read", R"({"path":"note.txt","offset":2,"limit":2})"),
                std::stop_token{},
                agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    // Read l2 and l3 out of 7 counted lines, so 4 remain and the next offset
    // is 4.
    CHECK(ai::text_from_content(result->content) == "l2\nl3\n\n[4 more lines in file. Use offset=4 to continue.]");
    CHECK_FALSE(result->details);
}

TEST_CASE("async read tool serves absolute paths inside and outside the workspace",
        "[tools][async][issue618][issue696][issue698][spec]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    workspace.write("local.txt", "local body");
    outside.write("my-skill/SKILL.md", "skill body");

    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    const auto inside = (workspace.path() / "local.txt").string();
    auto local = run_tool([&]() {
        return tool.execute(
                invocation("read", "{\"path\":\"" + inside + "\"}"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(local);
    CHECK_FALSE(local->is_error);
    CHECK(ai::text_from_content(local->content).find("local body") != std::string::npos);

    // Skills and prompt mentions carrying absolute paths resolve without an
    // authorization list (ADR 0057 retires the #629 skill-root allowlist).
    const auto skill_file = (outside.path() / "my-skill" / "SKILL.md").string();
    auto skill = run_tool([&]() {
        return tool.execute(
                invocation("read", "{\"path\":\"" + skill_file + "\"}"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(skill);
    CHECK_FALSE(skill->is_error);
    CHECK(ai::text_from_content(skill->content).find("skill body") != std::string::npos);

    // A missing outside file surfaces the OS-level error, not a containment
    // rejection.
    const auto missing_file = (outside.path() / "missing.md").string();
    auto missing = run_tool([&]() {
        return tool.execute(
                invocation("read", "{\"path\":\"" + missing_file + "\"}"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(missing);
    CHECK(missing->is_error);
}

TEST_CASE("async read tool strips '@' prefixes and expands '~' on external paths",
        "[tools][async][issue695][issue699][spec]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("docs/file.md", "mention body");

    auto env = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    // No workspace or skill-root allowlist remains in the tool contract (ADR
    // 0057); the path schema carries pi's read.ts wording (verbatim).
    CHECK(tool.definition.description == "Read a text file at a relative or absolute path");
    CHECK(tool.definition.parameters.at("properties").at("path").at("description").get<std::string>() ==
            "Path to the file to read (relative or absolute)");
    CHECK(tool.prompt_snippet == "Read file contents");
    REQUIRE(tool.prompt_guidelines.size() == 1);
    CHECK(tool.prompt_guidelines.front() == "Use read to examine files instead of cat or sed.");

    // Leading "@" prompt mentions strip before resolution (ADR 0057).
    const auto mentioned = "@" + (outside.path() / "docs" / "file.md").string();
    auto at_result = run_tool([&]() {
        return tool.execute(
                invocation("read", "{\"path\":\"" + mentioned + "\"}"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(at_result);
    CHECK_FALSE(at_result->is_error);
    CHECK(ai::text_from_content(at_result->content).find("mention body") != std::string::npos);

    // "~" expands against $HOME before resolution.
    tests::TempWorkspace fake_home;
    fake_home.write("notes/home.md", "home body");
    const tests::EnvVarGuard home{"HOME", fake_home.path().string()};
    auto tilde_result = run_tool([&]() {
        return tool.execute(
                invocation("read", "{\"path\":\"~/notes/home.md\"}"), std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(tilde_result);
    CHECK_FALSE(tilde_result->is_error);
    CHECK(ai::text_from_content(tilde_result->content).find("home body") != std::string::npos);
}

TEST_CASE("async write tool writes an absolute path outside the workspace with pi wording",
        "[tools][async][issue619][compat-pi][spec]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_write_file_tool(filesystem);

    // pi `core/tools/write.ts` description and schema wording (verbatim).
    CHECK(tool.definition.description == "Write content to a file. Creates the file if it doesn't exist, "
                                         "overwrites if it does. Automatically creates parent directories.");
    CHECK(tool.definition.parameters.at("properties").at("path").at("description").get<std::string>() ==
            "Path to the file to write (relative or absolute)");
    CHECK(tool.definition.parameters.at("properties").at("content").at("description").get<std::string>() ==
            "Content to write to the file");

    const auto target = (outside.path() / "nested" / "created.txt").string();
    auto result = run_tool([&]() {
        return tool.execute(invocation("write", "{\"path\":\"" + target + "\",\"content\":\"outside body\"}"),
                std::stop_token{},
                agent::ToolUpdateSink{});
    });
    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(outside.read("nested/created.txt") == "outside body");
}

TEST_CASE("async edit tool edits an absolute path outside the workspace with pi wording",
        "[tools][async][issue619][compat-pi][spec]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace outside;
    outside.write("scratch/note.txt", "before body");
    auto filesystem = std::make_shared<harness::AsyncLocalFileSystem>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(filesystem);

    // pi `core/tools/edit.ts` path schema wording (verbatim); the tool
    // description already matches pi and stays unchanged.
    CHECK(tool.definition.parameters.at("properties").at("path").at("description").get<std::string>() ==
            "Path to the file to edit (relative or absolute)");

    const auto target = (outside.path() / "scratch" / "note.txt").string();
    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
                                    "{\"path\":\"" + target +
                                            "\",\"edits\":[{\"oldText\":\"before body\",\"newText\":\"after body\"}]}"),
                std::stop_token{},
                agent::ToolUpdateSink{});
    });
    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(outside.read("scratch/note.txt") == "after body");
}
