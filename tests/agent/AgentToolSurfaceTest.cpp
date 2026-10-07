// Spec #882 / ticket #884: the Agent's live tool-surface seam. The MCP manager
// re-registers tools while the session runs (pi `extensions/mcp/index.ts`
// `registerTools`/`hideTools`), and a withdrawn tool becomes hidden rather than
// unregistered. The Agent therefore exposes `register_tool`, `remove_tool`,
// `set_active_tools`, and `active_tools`, and declares to the model only the
// active subset (pi `setActiveTools` -> declared tools). The separation cases
// name what an existence-only check would let through: a registry that gains a
// tool but never declares it, and one that drops a tool from the registry
// while it stays in the declared set.

#include <cch/agent/Agent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/FakeModelStream.hpp"
#include "support/FakeTool.hpp"
#include "support/ModelFixture.hpp"
#include "support/ToolArgumentContracts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] agent::Tool make_tool(std::string name) {
    return tests::make_fake_tool(ai::Tool{std::move(name), "A tool", test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::Exclusive,
            [](agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
                    -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                co_return agent::AsyncToolExecutionResult{.content = std::vector<ai::Content>{ai::text_content("ok")},
                        .details = std::nullopt,
                        .is_error = false};
            });
}

[[nodiscard]] support::ExpectedVoid run_prompt(agent::Agent& subject, std::string prompt) {
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await support::detail::await_async_result(subject.prompt(std::move(prompt)));
                co_return;
            },
            boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

[[nodiscard]] std::vector<std::string> request_tool_names(const tests::FakeModelStream& stream) {
    REQUIRE(!stream.calls.empty());
    std::vector<std::string> names;
    names.reserve(stream.calls.back().context.tools.size());
    for (const auto& tool : stream.calls.back().context.tools) {
        names.push_back(tool.name);
    }
    return names;
}

} // namespace

TEST_CASE("Agent declares only the active tool subset to the model", "[agent][async][issue884][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_tool("alpha")));
    REQUIRE(registry.add(make_tool("zed")));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    // All registered tools are active at construction (pi's assembled loadout).
    CHECK(subject.active_tools() == std::vector<std::string>{"alpha", "zed"});

    REQUIRE(subject.set_active_tools({"alpha"}));
    auto run = run_prompt(subject, "hello");
    REQUIRE(run);
    CHECK(request_tool_names(*stream) == std::vector<std::string>{"alpha"});
    CHECK(subject.state().active_tool_names == std::vector<std::string>{"alpha"});
}

TEST_CASE("Agent registers a live tool and declares it only once activated", "[agent][async][issue884][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_tool("alpha")));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    REQUIRE(subject.register_tool(make_tool("zed")));
    // Registration alone does not declare the tool (pi `defaultActive: false`,
    // and the MCP surface echoes activation through `set_active_tools`).
    CHECK(subject.active_tools() == std::vector<std::string>{"alpha"});
    CHECK(subject.set_active_tools({"alpha", "zed"}));

    auto run = run_prompt(subject, "hello");
    REQUIRE(run);
    CHECK(request_tool_names(*stream) == std::vector<std::string>{"alpha", "zed"});
}

TEST_CASE("Agent removes a tool from the live registry and the declared set", "[agent][async][issue884][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_tool("alpha")));
    REQUIRE(registry.add(make_tool("zed")));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    REQUIRE(subject.remove_tool("alpha"));
    CHECK(subject.active_tools() == std::vector<std::string>{"zed"});
    auto run = run_prompt(subject, "hello");
    REQUIRE(run);
    CHECK(request_tool_names(*stream) == std::vector<std::string>{"zed"});
}

TEST_CASE("Agent set_active_tools ignores names that are not registered", "[agent][async][issue884][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_tool("alpha")));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    REQUIRE(subject.set_active_tools({"alpha", "ghost"}));
    CHECK(subject.active_tools() == std::vector<std::string>{"alpha"});
    CHECK_FALSE(subject.remove_tool("ghost"));
}

TEST_CASE("Agent register_tool rejects a tool without an execute operation or name", "[agent][async][issue884][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    agent::ToolRegistry registry;
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    CHECK_FALSE(subject.register_tool(agent::Tool{}));
    CHECK_FALSE(subject.remove_tool("missing"));
}
