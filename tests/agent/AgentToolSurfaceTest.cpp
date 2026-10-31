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
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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

[[nodiscard]] agent::Tool make_counting_tool(std::string name, std::map<std::string, int>& executes) {
    return tests::make_fake_tool(ai::Tool{.name = name,
                                         .description = "A tool",
                                         .parameters = test::permissive_object_tool_argument_contract()},
            agent::ToolConcurrency::Exclusive,
            [counter = &executes, tool_name = name](agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
                    -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                ++(*counter)[tool_name];
                co_return agent::AsyncToolExecutionResult{.content = std::vector<ai::Content>{ai::text_content("ok")},
                        .details = std::nullopt,
                        .is_error = false};
            });
}

[[nodiscard]] std::vector<std::string> names_of(const std::vector<ai::Tool>& tools) {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& tool : tools) {
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] std::vector<std::string> removed_names(const std::vector<ai::ToolReference>& removed) {
    std::vector<std::string> names;
    names.reserve(removed.size());
    for (const auto& reference : removed) {
        names.push_back(reference.name);
    }
    return names;
}

/// Every system message in live history as (added, removed) declaration pairs.
[[nodiscard]] std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> system_declarations(
        const agent::Agent& subject) {
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> declarations;
    for (const auto& message : subject.state().messages) {
        const auto* system = std::get_if<ai::SystemMessage>(&message);
        if (system == nullptr) {
            continue;
        }
        declarations.emplace_back(names_of(system->tools_added), removed_names(system->tools_removed));
    }
    return declarations;
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

// #1110 / audit-1047: the active set is the executable set, not only the
// prompt declaration. A removed tool's model-issued call must never execute,
// and the request must declare the ordered delta between the transcript's
// tool state and the executable set (pi `declareToolChanges`).
TEST_CASE("a removed active tool never executes and the request declares the ordered delta",
        "[agent][async][tool-loadout][issue1110][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    std::map<std::string, int> executes;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_counting_tool("alpha", executes)));
    REQUIRE(registry.add(make_counting_tool("beta", executes)));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    options.system_prompt = "sys";
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    // The transcript leads with the declared loadout (pi
    // `createInitialSystemMessage`), so the delta has a declared state to
    // compare against.
    REQUIRE(subject.state().messages.size() == 1);
    REQUIRE(system_declarations(subject) ==
            std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>{{{"alpha", "beta"}, {}}});

    REQUIRE(subject.set_active_tools({"beta"}));
    // The model issues a call to the removed name. Restricting the prompt
    // declaration alone would let this call reach `execute()`.
    ai::AssistantMessage removed_call;
    removed_call.stop_reason = ai::AssistantStopReason::ToolUse;
    removed_call.content.emplace_back(ai::tool_call_content("removed-1", "alpha", "{}"));
    stream->responses.push_back(std::move(removed_call));
    stream->responses.push_back(ai::assistant_text_message("done"));

    auto run = run_prompt(subject, "go");
    REQUIRE(run);

    CHECK(executes["alpha"] == 0);
    CHECK(executes["beta"] == 0);
    CHECK(request_tool_names(*stream) == std::vector<std::string>{"beta"});
    // The delta is the second system message, before the user message.
    CHECK(system_declarations(subject) == std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>>{
                                                  {{"alpha", "beta"}, {}},
                                                  {{}, {"alpha"}},
                                          });

    const auto messages = subject.state().messages;
    REQUIRE(messages.size() == 6);
    REQUIRE(std::holds_alternative<ai::SystemMessage>(messages[1]));
    REQUIRE(std::holds_alternative<ai::UserMessage>(messages[2]));
    const auto* result = std::get_if<ai::ToolResultMessage>(&messages[4]);
    REQUIRE(result != nullptr);
    CHECK(result->tool_call_id == "removed-1");
    CHECK(result->tool_name == "alpha");
    CHECK(result->is_error);
    // A call that never executed has no duration (pi `durationMs` absence).
    CHECK_FALSE(result->duration_ms.has_value());
}

TEST_CASE("the tool delta declares ordered additions and removals", "[agent][async][tool-loadout][issue1110][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    std::map<std::string, int> executes;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_counting_tool("alpha", executes)));
    REQUIRE(registry.add(make_counting_tool("beta", executes)));
    REQUIRE(registry.add(make_counting_tool("gamma", executes)));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    REQUIRE(subject.set_active_tools({"gamma", "alpha"}));
    REQUIRE(run_prompt(subject, "first"));
    REQUIRE(system_declarations(subject).size() == 2);
    // Removals follow the previous declaration's order.
    CHECK(system_declarations(subject)[1] ==
            std::pair<std::vector<std::string>, std::vector<std::string>>{{}, {"beta"}});

    // Registering a tool does not declare it; the caller names it.
    REQUIRE(subject.register_tool(make_counting_tool("delta", executes)));
    REQUIRE(subject.set_active_tools({"alpha", "delta"}));
    REQUIRE(run_prompt(subject, "second"));

    const auto declarations = system_declarations(subject);
    REQUIRE(declarations.size() == 3);
    // Additions follow the executable set's order; removals keep the
    // previous declaration's order.
    CHECK(declarations[2] == std::pair<std::vector<std::string>, std::vector<std::string>>{{"delta"}, {"gamma"}});
}

TEST_CASE(
        "the active set keeps its own copy of the replacement array", "[agent][async][tool-loadout][issue1110][spec]") {
    auto stream = std::make_shared<tests::FakeModelStream>();
    std::map<std::string, int> executes;
    agent::ToolRegistry registry;
    REQUIRE(registry.add(make_counting_tool("alpha", executes)));
    REQUIRE(registry.add(make_counting_tool("beta", executes)));
    agent::AsyncAgentOptions options;
    options.model = tests::make_model("gpt-test");
    agent::Agent subject(stream->factory(), std::move(registry), std::move(options));

    std::vector<std::string> replacement{"beta"};
    REQUIRE(subject.set_active_tools(replacement));
    // pi `AgentState.tools` setter copies the replacement array; mutating the
    // caller's array afterwards must not narrow the active snapshot.
    replacement.pop_back();
    CHECK(subject.active_tools() == std::vector<std::string>{"beta"});
    CHECK(subject.state().active_tool_names == std::vector<std::string>{"beta"});

    REQUIRE(run_prompt(subject, "hello"));
    CHECK(request_tool_names(*stream) == std::vector<std::string>{"beta"});

    ai::AssistantMessage call;
    call.stop_reason = ai::AssistantStopReason::ToolUse;
    call.content.emplace_back(ai::tool_call_content("active-1", "beta", "{}"));
    stream->responses.push_back(std::move(call));
    stream->responses.push_back(ai::assistant_text_message("done"));
    REQUIRE(run_prompt(subject, "call beta"));
    CHECK(executes["beta"] == 1);
}
