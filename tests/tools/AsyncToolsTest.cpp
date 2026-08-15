#include "../support/TempWorkspace.hpp"

#include <cch/agent/harness/LocalExecutionEnv.hpp>
#include <cch/agent/tools/ToolFactories.hpp>
#include "agent/ToolArgumentPreparation.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "harness/RuntimeRoot.hpp"
#include "util/Json.hpp"
#include "util/OutputLimiter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
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

class CapturingEnv final : public harness::AsyncExecutionEnv {
public:
    explicit CapturingEnv(std::filesystem::path workspace_path) : workspace_path_(std::move(workspace_path)) {}
    const std::filesystem::path& workspace() const override { return workspace_path_; }
    support::AsyncResult<std::string, harness::FileError> absolutePath(std::string path, std::stop_token) override { return ready_file(std::move(path)); }
    support::AsyncResult<std::string, harness::FileError> joinPath(std::vector<std::string>, std::stop_token) override { return ready_file(std::string{}); }
    support::AsyncResult<std::string, harness::FileError> readTextFile(std::string, std::stop_token stop_token) override { last_stop_token = stop_token; return ready_file(std::string{}); }
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
    support::AsyncResult<harness::ShellExecResult, harness::ExecutionError> exec(std::string command, harness::ExecOptions options) override {
        last_command = std::move(command);
        last_timeout = options.timeout.value_or(std::chrono::milliseconds{0});
        last_stop_token = options.stop_token;
        last_env = options.env;
        if (options.onStdout && !streamed_stdout.empty()) (*options.onStdout)(streamed_stdout);
        if (options.onStderr && !streamed_stderr.empty()) (*options.onStderr)(streamed_stderr);
        return support::AsyncResult<harness::ShellExecResult, harness::ExecutionError>{
            std::expected<harness::ShellExecResult, harness::ExecutionError>{next_shell_result}};
    }

    std::string last_command;
    std::chrono::milliseconds last_timeout{0};
    std::stop_token last_stop_token;
    std::optional<std::map<std::string, std::string>> last_env;
    std::string streamed_stdout;
    std::string streamed_stderr;
    harness::ShellExecResult next_shell_result{.stdout_output = "ok", .stderr_output = "", .exitCode = 0};
    std::string last_write_path;
    std::string last_write_content;
private:
    std::filesystem::path workspace_path_;
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
util::Expected<agent::AsyncToolExecutionResult> run_tool(Start start) {
    auto& loop = test_runtime().loop();
    loop.restart();
    std::optional<util::Expected<agent::AsyncToolExecutionResult>> result;
    boost::asio::co_spawn(
        loop,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await ai::detail::await_async_result(start());
            co_return;
        },
        boost::asio::detached);
    while (!result) {
        loop.run_one();
    }
    return std::move(*result);
}

agent::ToolInvocation invocation(std::string name, std::string json) {
    auto args = util::read_json(json);
    REQUIRE(args);
    return agent::ToolInvocation{"call-1", std::move(name), std::move(*args), std::move(json)};
}

} // namespace

TEST_CASE("built-in tools default to exclusive execution", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());

    auto read = tools::make_async_read_file_tool(env);
    auto write = tools::make_async_write_file_tool(env);
    auto edit = tools::make_async_edit_tool(env);
    auto bash = tools::make_async_bash_tool(env);

    CHECK(read.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(write.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(edit.concurrency == agent::ToolConcurrency::Exclusive);
    CHECK(bash.concurrency == agent::ToolConcurrency::Exclusive);
}

TEST_CASE("async read_file tool uses Glaze typed args and workspace guard", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "line1\nline2\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool applies disjoint edits and returns pi-shaped diff details", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "alpha\nbeta\ngamma\ndelta\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool matches every edit against the original and rejects overlaps", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "one\ntwo\nthree\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool rejects missing and duplicate target text with pi messages", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "foo foo foo");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool preserves BOM and CRLF line endings", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("edit.txt", "\xef\xbb\xbf" "one\r\ntwo\r\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_edit_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(invocation("edit",
            R"({"path":"edit.txt","edits":[{"oldText":"two","newText":"TWO"}]})"), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(workspace.read("edit.txt") == "\xef\xbb\xbf" "one\r\nTWO\r\n");
}

TEST_CASE("async edit tool fuzzy-matches smart-quote and dash variants", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    // The file carries smart quotes, an em dash, and trailing line
    // whitespace; the edit uses ASCII forms without the trailing space,
    // matching pi's fuzzy normalization.
    workspace.write("note.txt", "say \xe2\x80\x9chello\xe2\x80\x9d world\xe2\x80\x94today   \n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool rejects empty oldText with pi's message", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "content\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async edit tool rejects no-change edits with pi's message", "[tools][async][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("edit declared contract validation and execution acceptance agree", "[tools][async][issue77][issue354]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
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

TEST_CASE("async tools prefer structured arguments over raw provider text", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("structured.txt", "from-structured");
    workspace.write("raw.txt", "from-raw");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto structured = util::read_json(R"({"path":"structured.txt"})");
    REQUIRE(structured);
    agent::ToolInvocation call{"call-1", "read_file", *structured, R"({"path":"raw.txt"})"};

    auto result = run_tool([&]() {
        return tool.execute(std::move(call), std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "from-structured");
}

TEST_CASE(
    "async bash tool preserves its visible command and carries execution options",
    "[tools][async][issue40][issue84]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    auto tool = tools::make_async_bash_tool(env);
    std::stop_source stop_source;

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi","timeout":5})"),
            stop_source.get_token(), agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(env->last_command == "echo hi");
    CHECK(env->last_timeout == std::chrono::milliseconds(5000));
    CHECK(env->last_stop_token == stop_source.get_token());
}

TEST_CASE(
    "async bash tool exposes live PI_* session facts when a session environment is provided",
    "[tools][async][issue414]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    auto session_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    session_environment->session_id = "session-42";
    session_environment->session_file = "/tmp/session-42.jsonl";
    session_environment->provider = "openai-codex";
    session_environment->model = "gpt-5.2-codex";
    session_environment->reasoning_level = "high";
    auto tool = tools::make_async_bash_tool(env, session_environment);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"env | grep PI_"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    REQUIRE(env->last_env.has_value());
    CHECK(env->last_env->at("PI_SESSION_ID") == "session-42");
    CHECK(env->last_env->at("PI_SESSION_FILE") == "/tmp/session-42.jsonl");
    CHECK(env->last_env->at("PI_PROVIDER") == "openai-codex");
    CHECK(env->last_env->at("PI_MODEL") == "gpt-5.2-codex");
    CHECK(env->last_env->at("PI_REASONING_LEVEL") == "high");
}

TEST_CASE(
    "async bash tool shadows absent PI_* facts with empty values and injects nothing without a holder",
    "[tools][async][issue414]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    auto session_environment =
        std::make_shared<tools::BashSessionEnvironment>();
    session_environment->session_id = "session-7";
    auto tool = tools::make_async_bash_tool(env, session_environment);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    REQUIRE(env->last_env.has_value());
    CHECK(env->last_env->at("PI_SESSION_ID") == "session-7");
    CHECK(env->last_env->at("PI_SESSION_FILE") == "");
    CHECK(env->last_env->at("PI_PROVIDER") == "");
    CHECK(env->last_env->at("PI_MODEL") == "");
    CHECK(env->last_env->at("PI_REASONING_LEVEL") == "");

    // Without a session environment the tool injects no environment at all
    // (pi `exposeSessionEnvironment: false`).
    auto plain_tool = tools::make_async_bash_tool(env);
    auto plain_result = run_tool([&]() {
        return plain_tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });
    REQUIRE(plain_result);
    CHECK_FALSE(env->last_env.has_value());
}

TEST_CASE("async bash tool spill file contains complete output beyond the visible limit", "[tools][async][issue73]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    const util::OutputLimit limit;
    env->streamed_stdout = std::string(limit.max_bytes + 100, 'x') +
        "\napi_key=super-secret\ncomplete-tail\xc3\xa9";
    env->next_shell_result.stdout_output = env->streamed_stdout.substr(0, limit.max_bytes);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const auto visible = ai::text_from_content(result->content);
    CHECK(visible.find("truncated=true") != std::string::npos);
    CHECK(visible.find("super-secret") == std::string::npos);
    CHECK(visible.find("[REDACTED]") != std::string::npos);
    CHECK(visible.size() <= limit.max_bytes + 200);
    CHECK_FALSE(env->last_write_path.empty());
    CHECK(env->last_write_content.find("super-secret") == std::string::npos);
    CHECK(env->last_write_content.find("[REDACTED]") != std::string::npos);
    CHECK(env->last_write_content.size() > limit.max_bytes);
    CHECK(env->last_write_content.ends_with("complete-tail\xc3\xa9"));
}

TEST_CASE("async bash tool without streamed output reports capping at the execution layer without a spill file", "[tools][async][issue73]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    const util::OutputLimit limit;
    // streamed_stdout/streamed_stderr stay empty, so the fake env never fires
    // the streaming callbacks and only the runner-capped result fields exist.
    env->next_shell_result.stdout_output = std::string(limit.max_bytes + 100, 'x') +
        "\napi_key=super-secret\ncomplete-tail";
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const auto visible = ai::text_from_content(result->content);
    CHECK(visible.find("truncated=true") != std::string::npos);
    CHECK(visible.find("capped at execution layer") != std::string::npos);
    CHECK(visible.find("full output:") == std::string::npos);
    CHECK(visible.find("super-secret") == std::string::npos);
    CHECK(visible.find("complete-tail") != std::string::npos);
    CHECK(visible.size() <= limit.max_bytes + 200);
    // No spill file: the capped text is all that exists, so a "complete
    // output" promise would be untruthful.
    CHECK(env->last_write_path.empty());
}

TEST_CASE("async bash tool strips ANSI escape sequences", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    env->streamed_stdout = "\x1b[31mred\x1b[0m";
    env->next_shell_result.stdout_output = env->streamed_stdout;
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const auto visible = ai::text_from_content(result->content);
    CHECK(visible.find("red") != std::string::npos);
    CHECK(visible.find('\x1b') == std::string::npos);
}

TEST_CASE("async bash tool is disabled unless env explicitly enables it", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(test_runtime_target(), workspace.path(), false);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool.execute(
            invocation("bash", R"({"command":"echo blocked"})"),
            std::stop_token{}, agent::ToolUpdateSink{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
}
