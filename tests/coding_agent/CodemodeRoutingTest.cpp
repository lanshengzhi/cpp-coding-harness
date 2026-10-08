// Spec #882, ticket #885: codemode script -> session-tool routing. The
// separation case the cheap "tools.* exists" check lets through: a script whose
// call silently resolves to undefined instead of running the session tool. The
// tests therefore assert the session tool actually ran, that both the jsName
// and the raw-name binding reach it, and that the run records pi's nested-call
// shape (`{callerId}/{n}`, `nested_calls`) on the model-issued result.

#include "agent/ToolCallExecutor.hpp"
#include "coding_agent/extensions/ExtensionToolRegistry.hpp"
#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "support/FakeTool.hpp"
#include "support/Json.hpp"

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace extensions = cch::coding_agent::extensions;

struct Executed {
    support::Expected<agent::ToolCallBatchResult> result;
    std::vector<std::string> echo_calls;
};

[[nodiscard]] agent::Tool echo_tool(std::string name, std::vector<std::string>* calls) {
    ai::Tool definition;
    definition.name = name;
    definition.description = "Echo the argument.";
    definition.parameters = support::JsonValue::object_t{
            {"type", "object"},
            {"properties", support::JsonValue::object_t{{"x", support::JsonValue::object_t{{"type", "number"}}}}},
            {"additionalProperties", false},
    };
    return tests::make_fake_tool(std::move(definition),
            agent::ToolConcurrency::ParallelSafe,
            [calls](agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                    -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                calls->push_back(invocation.name);
                co_return agent::AsyncToolExecutionResult{
                        .content = std::vector<ai::Content>{ai::text_content("echoed")},
                        .is_error = false,
                };
            });
}

/// One callable tool standing in for a pi MCP conversion
/// (`convertMcpResult`): declares the MCP result output schema and returns a
/// scripted result, recording its calls.
[[nodiscard]] agent::Tool schema_tool(std::string name,
        std::vector<ai::Content> content,
        std::optional<support::JsonValue> details,
        bool is_error,
        std::vector<std::string>* calls) {
    ai::Tool definition;
    definition.name = name;
    definition.description = "Structured echo.";
    definition.parameters = support::JsonValue::object_t{
            {"type", "object"},
            {"properties", support::JsonValue::object_t{{"x", support::JsonValue::object_t{{"type", "number"}}}}},
            {"additionalProperties", false},
    };
    definition.output_schema = support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"content", support::JsonValue::object_t{{"type", "array"}}},
                            {"isError", support::JsonValue::object_t{{"type", "boolean"}}},
                    }},
            {"required", support::JsonValue::array_t{"content"}},
    };
    return tests::make_fake_tool(std::move(definition),
            agent::ToolConcurrency::ParallelSafe,
            [content = std::move(content), details = std::move(details), is_error, calls](
                    agent::ToolInvocation invocation, std::stop_token, agent::ToolUpdateSink)
                    -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                calls->push_back(invocation.name);
                agent::AsyncToolExecutionResult result;
                result.content = content;
                result.details = details;
                result.is_error = is_error;
                co_return result;
            });
}

/// Run one codemode script through the real Agent Tool executor with a real
/// session registry, so the routing path (not a stand-in for it) is exercised.
[[nodiscard]] Executed run_script_with_registry(std::string script, agent::ToolRegistry registry) {
    {
        extensions::CodemodeToolSource source{};
        auto loaded = source.load_tools();
        REQUIRE(loaded.has_value());
        extensions::ExtensionToolRegistry extension_tools;
        for (auto& tool : *loaded) {
            REQUIRE(extension_tools.add(std::move(tool)).has_value());
        }
        REQUIRE(extensions::register_extension_tools(registry, std::move(extension_tools)).has_value());
    }

    ai::AssistantMessage assistant;
    ai::ToolCallContent call;
    call.id = "call_1";
    call.name = "codemode";
    call.raw_arguments =
            support::write_json(support::JsonValue{support::JsonValue::object_t{{"code", script}}}).value();
    assistant.content.emplace_back(std::move(call));
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;

    ai::AiContext context;
    agent::ToolCallExecutorOptions options;
    agent::ToolCallExecutor executor{registry, std::move(options)};

    std::optional<support::Expected<agent::ToolCallBatchResult>> result;
    std::mutex mutex;
    boost::asio::thread_pool pool{2};
    agent::AgentEventSink sink{
            [&](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid { return support::ExpectedVoid{}; }};
    boost::asio::co_spawn(
            pool,
            [&]() -> boost::asio::awaitable<void> {
                auto outcome = co_await executor.execute(agent::ToolCallBatchRequest{assistant, context}, sink);
                std::lock_guard lock(mutex);
                result = std::move(outcome);
                co_return;
            },
            boost::asio::detached);
    pool.join();
    REQUIRE(result.has_value());
    return Executed{std::move(*result), {}};
}

[[nodiscard]] Executed run_script_with_tools(std::string script, std::vector<std::string> extra_tool_names) {
    std::vector<std::string> echo_calls;
    agent::ToolRegistry registry;
    for (auto& name : extra_tool_names) {
        REQUIRE(registry.add(echo_tool(name, &echo_calls)).has_value());
    }
    auto executed = run_script_with_registry(std::move(script), std::move(registry));
    executed.echo_calls = std::move(echo_calls);
    return executed;
}

[[nodiscard]] std::vector<agent::Tool> make_tools(agent::Tool tool) {
    std::vector<agent::Tool> tools;
    tools.push_back(std::move(tool));
    return tools;
}

[[nodiscard]] Executed run_script_with_agent_tools(std::string script, std::vector<agent::Tool> extra_tools) {
    agent::ToolRegistry registry;
    for (auto& tool : extra_tools) {
        REQUIRE(registry.add(std::move(tool)).has_value());
    }
    return run_script_with_registry(std::move(script), std::move(registry));
}

[[nodiscard]] std::string joined_text(const ai::ToolResultMessage& message) {
    return ai::text_from_content(message.content);
}

} // namespace

TEST_CASE("a codemode script's tools.* call runs the session tool and records the nested call",
        "[coding_agent][codemode][issue885][spec]") {
    auto executed =
            run_script_with_tools("const r = await tools.echo({ x: 1 }); text('got:' + r); return 'done';", {"echo"});

    REQUIRE(executed.result.has_value());
    REQUIRE(executed.result->results.size() == 1);
    const auto& result = executed.result->results.front();
    CHECK_FALSE(result.is_error);

    // The session tool ran, and the script saw its value.
    REQUIRE(executed.echo_calls.size() == 1);
    CHECK(executed.echo_calls.front() == "echo");
    CHECK(joined_text(result).find("got:echoed") != std::string::npos);

    // The model-issued result carries pi's nested-call record with the
    // `{callerId}/{n}` id.
    REQUIRE(result.nested_calls.has_value());
    auto parsed = support::read_json(support::write_json(*result.nested_calls).value());
    REQUIRE(parsed.has_value());
    const auto& object = parsed->get_object();
    CHECK(object.at("complete").get_boolean());
    const auto& calls = object.at("calls").get_array();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().get_object().at("id").get_string() == "call_1/1");
    CHECK(calls.front().get_object().at("name").get_string() == "echo");
    CHECK(calls.front().get_object().at("status").get_string() == "ok");
}

TEST_CASE(
        "both the jsName and the raw-name binding reach the session tool", "[coding_agent][codemode][issue885][spec]") {
    // `list-issues` normalizes to the identifier `list_issues`.
    auto executed = run_script_with_tools("text(await tools.list_issues({ x: 1 })); "
                                          "text(await tools['list-issues']({ x: 2 })); "
                                          "return 'ok';",
            {"list-issues"});

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    CHECK_FALSE(result.is_error);
    REQUIRE(executed.echo_calls.size() == 2);
    CHECK(executed.echo_calls[0] == "list-issues");
    CHECK(executed.echo_calls[1] == "list-issues");
    REQUIRE(result.nested_calls.has_value());
    const auto parsed = support::read_json(support::write_json(*result.nested_calls).value());
    REQUIRE(parsed.has_value());
    CHECK(parsed->get_object().at("calls").get_array().size() == 2);
}

TEST_CASE("an unlisted tool name is an explicit rejected call, never silent",
        "[coding_agent][codemode][issue885][spec]") {
    auto executed = run_script_with_tools("return await tools.missing({ x: 1 });", {"echo"});

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    // The script's rejection is terminal: the run fails with an explicit error.
    CHECK(result.is_error);
    CHECK(joined_text(result).find("missing") != std::string::npos);
    CHECK(executed.echo_calls.empty());
}

TEST_CASE("a codemode script call resolves to a structured tool's whole result object",
        "[coding_agent][codemode][issue886][spec]") {
    // pi `toScriptValue`: a tool declaring an output schema resolves to its
    // structured content — pi `convertMcpResult`'s CallToolResult object with
    // the content blocks and the `isError` flag, not the bare structured
    // content field.
    std::vector<std::string> calls;
    auto tool = schema_tool("mcp__echo__echo",
            {ai::text_content("side text")},
            support::JsonValue{support::JsonValue::object_t{
                    {"content",
                            support::JsonValue::array_t{support::JsonValue::object_t{
                                    {"type", "text"},
                                    {"text", "side text"},
                            }}},
                    {"isError", false},
            }},
            false,
            &calls);

    auto executed = run_script_with_agent_tools(
            "const r = await tools.mcp__echo__echo({ x: 1 });"
            " text('kind:' + (r && r.content ? 'calltoolresult' : 'other') + ' isError:' + r.isError);"
            " return 'done';",
            make_tools(std::move(tool)));

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    CHECK_FALSE(result.is_error);
    REQUIRE(calls.size() == 1);
    CHECK(calls.front() == "mcp__echo__echo");
    CHECK(joined_text(result).find("kind:calltoolresult isError:false") != std::string::npos);
}

TEST_CASE("an error result carrying structured content still resolves for the script",
        "[coding_agent][codemode][issue886][spec]") {
    // pi keeps an MCP CallToolResult's `isError` in the resolved object
    // instead of rejecting: the script inspects the flag itself.
    std::vector<std::string> calls;
    auto tool = schema_tool("mcp__echo__echo",
            {},
            support::JsonValue{support::JsonValue::object_t{
                    {"content", support::JsonValue::array_t{}},
                    {"isError", true},
                    {"structuredContent", support::JsonValue::object_t{{"total", 0}}},
            }},
            true,
            &calls);

    auto executed =
            run_script_with_agent_tools("const r = await tools.mcp__echo__echo({ x: 1 });"
                                        " text('saw isError:' + r.isError + ' total:' + r.structuredContent.total);"
                                        " return 'done';",
                    make_tools(std::move(tool)));

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    CHECK_FALSE(result.is_error);
    REQUIRE(calls.size() == 1);
    CHECK(joined_text(result).find("saw isError:true total:0") != std::string::npos);
}

TEST_CASE("a tool without an output schema resolves to its text even when details exist",
        "[coding_agent][codemode][issue886][spec]") {
    // pi `toScriptValue`'s guard requires the tool's output schema: details
    // alone do not switch a call to the structured path.
    std::vector<std::string> calls;
    auto tool = echo_tool("echo", &calls);

    auto executed = run_script_with_agent_tools(
            "const r = await tools.echo({ x: 1 }); text('saw:' + r); return 'done';", make_tools(std::move(tool)));

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    CHECK_FALSE(result.is_error);
    REQUIRE(calls.size() == 1);
    CHECK(joined_text(result).find("saw:echoed") != std::string::npos);
}

TEST_CASE("a failing call without structured content rejects with pi's tool-name fallback",
        "[coding_agent][codemode][issue886][spec]") {
    // pi: `throw new Error(text || `Tool "${tool.name}" failed")`.
    std::vector<std::string> calls;
    auto tool = schema_tool("mcp__echo__echo", {}, std::nullopt, true, &calls);

    auto executed =
            run_script_with_agent_tools("return await tools.mcp__echo__echo({ x: 1 });", make_tools(std::move(tool)));

    REQUIRE(executed.result.has_value());
    const auto& result = executed.result->results.front();
    CHECK(result.is_error);
    REQUIRE(calls.size() == 1);
    CHECK(joined_text(result).find("Tool \"mcp__echo__echo\" failed") != std::string::npos);
}
