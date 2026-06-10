#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/harness/LocalExecutionEnv.hpp>
#include <cch/tools/ToolFactories.hpp>
#include "../support/TempWorkspace.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>

using namespace cch;

namespace {

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
    auto args = util::read_json<glz::generic>(json);
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
    CHECK(result->content == "line2");
}

TEST_CASE("async edit_file tool returns error result for ambiguous replacements", "[tools][async][u6][ae4]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "same\nsame\n");
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path());
    auto tool = tools::make_async_edit_file_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("edit_file", R"({"path":"note.txt","old_text":"same","new_text":"new"})"));
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->content.find("multiple") != std::string::npos);
    CHECK(workspace.read("note.txt") == "same\nsame\n");
}

TEST_CASE("async bash tool is disabled unless env explicitly enables it", "[tools][async][u6]") {
    tests::TempWorkspace workspace;
    auto env = std::make_shared<harness::AsyncLocalExecutionEnv>(workspace.path(), false);
    auto tool = tools::make_async_bash_tool(env);

    auto result = run_tool([&]() {
        return tool->execute(invocation("bash", R"({"command":"echo blocked","timeout_ms":1000})"));
    });

    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->content.find("disabled") != std::string::npos);
}
