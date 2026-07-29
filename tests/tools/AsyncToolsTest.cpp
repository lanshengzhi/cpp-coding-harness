#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../support/TempWorkspace.hpp"

#include <cch/harness/LocalExecutionEnv.hpp>
#include <cch/tools/ToolFactories.hpp>
#include "agent/ToolArgumentPreparation.hpp"
#include "util/Json.hpp"
#include "util/OutputLimiter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

using namespace cch;

namespace {

class CapturingEnv final : public harness::AsyncExecutionEnv {
public:
    explicit CapturingEnv(std::filesystem::path workspace_path) : workspace_path_(std::move(workspace_path)) {}

    const std::filesystem::path& workspace() const override { return workspace_path_; }

    boost::asio::awaitable<std::expected<std::string, harness::FileError>> absolutePath(
        std::string path,
        std::stop_token) override {
        co_return path;
    }
    boost::asio::awaitable<std::expected<std::string, harness::FileError>> joinPath(
        std::vector<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }
    boost::asio::awaitable<std::expected<std::string, harness::FileError>> readTextFile(
        std::string,
        std::stop_token stop_token) override {
        last_stop_token = stop_token;
        co_return std::string{};
    }
    boost::asio::awaitable<std::expected<std::vector<std::string>, harness::FileError>> readTextLines(
        std::string,
        std::optional<int>,
        std::stop_token stop_token) override {
        last_stop_token = stop_token;
        co_return std::vector<std::string>{};
    }
    boost::asio::awaitable<std::expected<harness::BinaryData, harness::FileError>> readBinaryFile(
        std::string,
        std::stop_token) override {
        co_return harness::BinaryData{};
    }
    boost::asio::awaitable<std::expected<void, harness::FileError>> writeFile(
        std::string path,
        harness::WriteContent content,
        std::stop_token stop_token) override {
        last_stop_token = stop_token;
        last_write_path = std::move(path);
        if (const auto* text = std::get_if<std::string>(&content)) {
            last_write_content = *text;
        }
        co_return std::expected<void, harness::FileError>{};
    }
    boost::asio::awaitable<std::expected<void, harness::FileError>> appendFile(
        std::string,
        harness::WriteContent,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    boost::asio::awaitable<std::expected<harness::FileInfo, harness::FileError>> fileInfo(
        std::string,
        std::stop_token) override {
        co_return harness::FileInfo{};
    }
    boost::asio::awaitable<std::expected<std::vector<harness::FileInfo>, harness::FileError>> listDir(
        std::string,
        std::stop_token) override {
        co_return std::vector<harness::FileInfo>{};
    }
    boost::asio::awaitable<std::expected<std::string, harness::FileError>> canonicalPath(
        std::string path,
        std::stop_token) override {
        co_return path;
    }
    boost::asio::awaitable<std::expected<bool, harness::FileError>> exists(
        std::string,
        std::stop_token) override {
        co_return true;
    }
    boost::asio::awaitable<std::expected<void, harness::FileError>> createDir(
        std::string,
        bool,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    boost::asio::awaitable<std::expected<void, harness::FileError>> remove(
        std::string,
        bool,
        std::stop_token) override {
        co_return std::expected<void, harness::FileError>{};
    }
    boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempDir(
        std::optional<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }
    boost::asio::awaitable<std::expected<std::string, harness::FileError>> createTempFile(
        std::optional<std::string>,
        std::optional<std::string>,
        std::stop_token) override {
        co_return std::string{};
    }

    boost::asio::awaitable<std::expected<harness::ShellExecResult, harness::ExecutionError>> exec(
        std::string command,
        harness::ExecOptions options) override {
        last_command = std::move(command);
        last_timeout = options.timeout.value_or(std::chrono::milliseconds{0});
        last_stop_token = options.stop_token;
        if (options.onStdout && !streamed_stdout.empty()) {
            (*options.onStdout)(streamed_stdout);
        }
        if (options.onStderr && !streamed_stderr.empty()) {
            (*options.onStderr)(streamed_stderr);
        }
        co_return next_shell_result;
    }

    std::string last_command;
    std::chrono::milliseconds last_timeout{0};
    std::stop_token last_stop_token;
    std::string streamed_stdout;
    std::string streamed_stderr;
    harness::ShellExecResult next_shell_result{
        .stdout_output = "ok",
        .stderr_output = "",
        .exitCode = 0,
    };
    std::string last_write_path;
    std::string last_write_content;

private:
    std::filesystem::path workspace_path_;
};

template <typename Start>
util::Expected<agent::AsyncToolExecutionResult> run_tool(Start start) {
    boost::asio::io_context io;
    std::optional<util::Expected<agent::AsyncToolExecutionResult>> result;
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

agent::ToolInvocation invocation(std::string name, std::string json) {
    auto args = util::read_json<util::JsonValue>(json);
    REQUIRE(args);
    return agent::ToolInvocation{"call-1", std::move(name), std::move(*args), std::move(json)};
}

} // namespace

TEST_CASE("built-in tools default to exclusive execution", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());

    auto read = tools::make_async_read_file_tool(env);
    auto write = tools::make_async_write_file_tool(env);
    auto edit = tools::make_async_edit_file_tool(env);
    auto bash = tools::make_async_bash_tool(env);

    CHECK(read->concurrency() == agent::ToolConcurrency::Exclusive);
    CHECK(write->concurrency() == agent::ToolConcurrency::Exclusive);
    CHECK(edit->concurrency() == agent::ToolConcurrency::Exclusive);
    CHECK(bash->concurrency() == agent::ToolConcurrency::Exclusive);
}

TEST_CASE("async read_file tool uses Glaze typed args and workspace guard", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "line1\nline2\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(
            invocation("read_file", R"({"path":"note.txt","offset":2,"limit":1})"),
            std::stop_token{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "line2");
}

TEST_CASE("async edit_file tool returns error for duplicate oldText matches", "[tools][async]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\nsame\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file",
            R"({"path":"note.txt","edits":[{"oldText":"same","newText":"new"}]})"), std::stop_token{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("2 occurrences") != std::string::npos);
    CHECK(workspace.read("note.txt") == "same\nsame\n");
}

TEST_CASE("async edit_file tool supports edits[] array with multiple replacements", "[tools][async]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world\nfoo bar\nbaz qux\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(
            invocation("edit_file",
                R"({"path":"note.txt","edits":[{"oldText":"hello","newText":"hi"},)"
                R"({"oldText":"baz","newText":"zip"}]})"),
            std::stop_token{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content).find("replaced 2 block") != std::string::npos);
    // The edit tool reads and writes the exact file content, so the
    // trailing newline is preserved.
    CHECK(workspace.read("note.txt") == "hi world\nfoo bar\nzip qux\n");
}

TEST_CASE("async edit_file tool rejects empty edits array", "[tools][async]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "content\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file",
            R"({"path":"note.txt","edits":[]})"), std::stop_token{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("at least one") != std::string::npos);
}

TEST_CASE("async edit_file tool rejects oldText not found", "[tools][async]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file",
            R"({"path":"note.txt","edits":[{"oldText":"nonexistent","newText":"x"}]})"), std::stop_token{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("not found") != std::string::npos);
}

TEST_CASE("edit_file declared contract validation and execution acceptance agree", "[tools][async][issue77]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "hello world\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    // The agent loop validates every call with prepare_tool_arguments before
    // execution (ADR 0007); execution must accept exactly what it accepts.
    auto contract_accepts = [&](const std::string& json) {
        ai::ToolCallContent call{
            .id = "call-1",
            .name = "edit_file",
            .arguments = std::nullopt,
            .raw_arguments = json,
            .thought_signature = std::nullopt,
            .argument_error = std::nullopt,
        };
        return agent::prepare_tool_arguments(tool->definition(), call).has_value();
    };
    auto execution_accepts = [&](const std::string& json) {
        auto result = run_tool([&]() {
            return tool->execute(invocation("edit_file", json), std::stop_token{});
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
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto structured = util::read_json<util::JsonValue>(R"({"path":"structured.txt"})");
    REQUIRE(structured);
    agent::ToolInvocation call{"call-1", "read_file", *structured, R"({"path":"raw.txt"})"};

    auto result = run_tool([&]() {
        return tool->execute(std::move(call), std::stop_token{});
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
        return tool->execute(
            invocation("bash", R"({"command":"echo hi","timeout":5})"),
            stop_source.get_token());
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(env->last_command == "echo hi");
    CHECK(env->last_timeout == std::chrono::milliseconds(5000));
    CHECK(env->last_stop_token == stop_source.get_token());
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
        return tool->execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{});
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
        return tool->execute(
            invocation("bash", R"({"command":"emit-large-output"})"),
            std::stop_token{});
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
        return tool->execute(
            invocation("bash", R"({"command":"echo hi"})"),
            std::stop_token{});
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    const auto visible = ai::text_from_content(result->content);
    CHECK(visible.find("red") != std::string::npos);
    CHECK(visible.find('\x1b') == std::string::npos);
}

TEST_CASE("async bash tool is disabled unless env explicitly enables it", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path(), false);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(
            invocation("bash", R"({"command":"echo blocked"})"),
            std::stop_token{});
    });

    REQUIRE(result);
    CHECK(result->is_error);
}
