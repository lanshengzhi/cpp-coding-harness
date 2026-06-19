#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../include/cch/tools/ToolFactories.hpp"
#include "../../include/cch/util/Json.hpp"
#include "../support/TempWorkspace.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

using namespace cch;

namespace {

class CapturingEnv final : public harness::AsyncExecutionEnv {
public:
    explicit CapturingEnv(std::filesystem::path workspace_path) : workspace_path_(std::move(workspace_path)) {}

    const std::filesystem::path& workspace() const override { return workspace_path_; }
    bool bash_enabled() const override { return true; }

    boost::asio::awaitable<util::Expected<harness::AsyncFileReadResult>> read_file(
        std::string,
        int,
        int) override {
        co_return harness::AsyncFileReadResult{""};
    }

    boost::asio::awaitable<util::Expected<harness::AsyncFileWriteResult>> write_file(
        std::string,
        std::string,
        bool) override {
        co_return harness::AsyncFileWriteResult{};
    }

    boost::asio::awaitable<util::Expected<harness::AsyncFileEditResult>> edit_file(
        std::string,
        std::string,
        std::string) override {
        co_return harness::AsyncFileEditResult{};
    }

    boost::asio::awaitable<util::Expected<harness::AsyncShellResult>> run_shell(
        std::string command,
        std::chrono::milliseconds timeout) override {
        last_command = std::move(command);
        last_timeout = timeout;
        co_return harness::AsyncShellResult{0, "ok", false};
    }

    std::string last_command;
    std::chrono::milliseconds last_timeout{0};

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

TEST_CASE("async read_file tool uses Glaze typed args and workspace guard", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "line1\nline2\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_read_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("read_file", R"({"path":"note.txt","offset":2,"limit":1})"));
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

    // Legacy single-arg format: old_text/new_text
    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file", R"({"path":"note.txt","old_text":"same","new_text":"new"})"));
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
        return tool->execute(invocation("edit_file",
            R"({"path":"note.txt","edits":[{"old_text":"hello","new_text":"hi"},{"old_text":"baz","new_text":"zip"}]})"));
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content).find("replaced 2 block") != std::string::npos);
    // Note: read_file strips trailing newline (std::getline behavior),
    // so write_file writes content without trailing newline.
    CHECK(workspace.read("note.txt") == "hi world\nfoo bar\nzip qux");
}

TEST_CASE("async edit_file tool rejects empty edits array", "[tools][async]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "content\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file",
            R"({"path":"note.txt","edits":[]})"));
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
            R"({"path":"note.txt","edits":[{"old_text":"nonexistent","new_text":"x"}]})"));
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("not found") != std::string::npos);
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
        return tool->execute(std::move(call));
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(ai::text_from_content(result->content) == "from-structured");
}

TEST_CASE("async bash tool rejects non-positive timeout", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path(), true);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("bash", R"({"command":"echo hi","timeout_ms":0})"));
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("timeout_ms") != std::string::npos);
}

TEST_CASE("async bash tool clamps oversized timeout", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<CapturingEnv>(workspace.path());
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("bash", R"({"command":"echo hi","timeout_ms":999999})"));
    });

    REQUIRE(result);
    CHECK_FALSE(result->is_error);
    CHECK(env->last_command == "echo hi");
    CHECK(env->last_timeout == std::chrono::milliseconds(120000));
}

TEST_CASE("async bash tool is disabled unless env explicitly enables it", "[tools][async]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path(), false);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("bash", R"({"command":"echo blocked","timeout_ms":1000})"));
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(ai::text_from_content(result->content).find("disabled") != std::string::npos);
}
