#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/AgentLoop.hpp"
#include "../../src/tools/Tools.hpp"
#include "../support/FakeChatClient.hpp"
#include "../support/TempWorkspace.hpp"

using namespace cch;

TEST_CASE("one-shot prompt records user and assistant messages", "[agent][u1]") {
    tests::FakeChatClient client;
    client.push_response(tests::text_response("hello from fake"));

    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("say hello");

    REQUIRE(result.ok());
    CHECK(result.value().final_text == "hello from fake");
    REQUIRE(result.value().messages.size() == 2);
    CHECK(result.value().messages[0].role == agent::Role::User);
    CHECK(result.value().messages[0].content == "say hello");
    CHECK(result.value().messages[1].role == agent::Role::Assistant);
    CHECK(client.requests.size() == 1);
    CHECK(client.requests[0].messages[0].content == "say hello");
}

TEST_CASE("empty assistant text completes without formatting crash", "[agent][u1]") {
    tests::FakeChatClient client;
    client.push_response(tests::text_response(""));

    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("empty is ok");

    REQUIRE(result.ok());
    CHECK(result.value().final_text.empty());
    CHECK(result.value().stop_reason == "stop");
}

TEST_CASE("chat client error becomes structured loop failure", "[agent][u1]") {
    tests::FakeChatClient client;
    client.fail_next("provider unavailable");

    std::vector<agent::LoopEvent> events;
    agent::ToolRegistry registry;
    agent::LoopOptions options;
    options.on_event = [&](const agent::LoopEvent& event) { events.push_back(event); };
    agent::AgentLoop loop(client, registry, options);

    auto result = loop.run("will fail");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == "provider unavailable");
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == "model_request");
    CHECK(events[1].type == "provider_error");
    CHECK(events[1].detail == "provider unavailable");
}

TEST_CASE("tool call result is appended with matching call id and fed into next model request", "[agent][u4][ae1]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "agent context");
    tests::FakeChatClient client;
    boost::json::object args;
    args["path"] = "note.txt";
    client.push_response(tests::tool_response("call-1", "read_file", args));
    client.push_response(tests::text_response("I saw the note"));

    agent::ToolRegistry registry;
    registry.add(tools::make_read_file_tool());
    agent::LoopOptions options;
    options.workspace = workspace.path();
    agent::AgentLoop loop(client, registry, options);

    auto result = loop.run("inspect note.txt");

    REQUIRE(result.ok());
    REQUIRE(client.requests.size() == 2);
    REQUIRE(client.requests[1].messages.size() == 3);
    const auto& tool_message = client.requests[1].messages[2];
    CHECK(tool_message.role == agent::Role::Tool);
    CHECK(tool_message.tool_call_id == "call-1");
    CHECK(tool_message.content.find("agent context") != std::string::npos);
}

TEST_CASE("tool call loop emits stable MVP event transcript", "[agent][u1][ae1]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "event context");
    tests::FakeChatClient client;
    boost::json::object args;
    args["path"] = "note.txt";
    client.push_response(tests::tool_response("call-1", "read_file", args, "reading"));
    client.push_response(tests::text_response("done"));
    std::vector<agent::LoopEvent> events;

    agent::ToolRegistry registry;
    registry.add(tools::make_read_file_tool());
    agent::LoopOptions options;
    options.workspace = workspace.path();
    options.on_event = [&](const agent::LoopEvent& event) { events.push_back(event); };
    agent::AgentLoop loop(client, registry, options);

    auto result = loop.run("inspect note.txt");

    REQUIRE(result.ok());
    REQUIRE(events.size() == 7);
    CHECK(events[0].type == "model_request");
    CHECK(events[1].type == "assistant");
    CHECK(events[1].detail == "reading");
    CHECK(events[2].type == "tool_call");
    CHECK(events[2].detail == "read_file#call-1");
    CHECK(events[3].type == "tool_success");
    CHECK(events[3].detail == "call-1");
    CHECK(events[4].type == "model_request");
    CHECK(events[5].type == "assistant");
    CHECK(events[5].detail == "done");
    CHECK(events[6].type == "completed");
    CHECK(events[6].detail == "stop");
}

TEST_CASE("unknown tool and malformed arguments continue as error tool results", "[agent][u4]") {
    tests::FakeChatClient client;
    boost::json::object args;
    client.push_response(tests::tool_response("missing", "does_not_exist", args));
    client.push_response(tests::text_response("handled missing tool"));
    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("call a tool");

    REQUIRE(result.ok());
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].messages[2].is_error);
    CHECK(client.requests[1].messages[2].content.find("Unknown tool") != std::string::npos);

    tests::FakeChatClient malformed_client;
    llm::ChatResponse malformed;
    malformed.assistant_message.role = agent::Role::Assistant;
    agent::ToolCall call;
    call.id = "bad-args";
    call.name = "read_file";
    call.arguments_valid = false;
    call.argument_error = "invalid JSON arguments";
    malformed.assistant_message.tool_calls.push_back(call);
    malformed_client.push_response(malformed);
    malformed_client.push_response(tests::text_response("handled malformed args"));
    agent::ToolRegistry read_registry;
    read_registry.add(tools::make_read_file_tool());
    agent::AgentLoop malformed_loop(malformed_client, read_registry);

    auto malformed_result = malformed_loop.run("bad args");

    REQUIRE(malformed_result.ok());
    REQUIRE(malformed_client.requests.size() == 2);
    CHECK(malformed_client.requests[1].messages[2].is_error);
    CHECK(malformed_client.requests[1].messages[2].content == "invalid JSON arguments");
}

TEST_CASE("multiple tool calls execute sequentially in model order", "[agent][u4]") {
    tests::TempWorkspace workspace;
    tests::FakeChatClient client;
    llm::ChatResponse response;
    response.assistant_message.role = agent::Role::Assistant;
    agent::ToolCall first;
    first.id = "first";
    first.name = "write_file";
    first.arguments = {{"path", "a.txt"}, {"content", "A"}};
    agent::ToolCall second;
    second.id = "second";
    second.name = "write_file";
    second.arguments = {{"path", "b.txt"}, {"content", "B"}};
    response.assistant_message.tool_calls = {first, second};
    client.push_response(response);
    client.push_response(tests::text_response("done"));

    agent::ToolRegistry registry;
    registry.add(tools::make_write_file_tool());
    agent::LoopOptions options;
    options.workspace = workspace.path();
    agent::AgentLoop loop(client, registry, options);

    auto result = loop.run("write files");

    REQUIRE(result.ok());
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].messages[2].tool_call_id == "first");
    CHECK(client.requests[1].messages[3].tool_call_id == "second");
    CHECK(workspace.read("a.txt") == "A");
    CHECK(workspace.read("b.txt") == "B");
}

TEST_CASE("tool call arguments are redacted in canonical message history", "[agent][u4]") {
    tests::FakeChatClient client;
    llm::ChatResponse response;
    response.assistant_message.role = agent::Role::Assistant;
    agent::ToolCall call;
    call.id = "secret-args";
    call.name = "unknown_tool";
    call.arguments = {{"apiKey", "plain-secret-value"}, {"access_token", "second-secret-value"}, {"path", "note.txt"}};
    call.raw_arguments = R"({"apiKey":"plain-secret-value","access_token":"second-secret-value","path":"note.txt"})";
    response.assistant_message.tool_calls.push_back(call);
    client.push_response(response);
    client.push_response(tests::text_response("handled"));
    agent::AgentLoop loop(client, agent::ToolRegistry{});

    auto result = loop.run("call with secret args");

    REQUIRE(result.ok());
    REQUIRE(result.value().messages.size() >= 2);
    const auto& stored_call = result.value().messages[1].tool_calls[0];
    CHECK(std::string(stored_call.arguments.at("apiKey").as_string()) == "[REDACTED]");
    CHECK(std::string(stored_call.arguments.at("access_token").as_string()) == "[REDACTED]");
    CHECK(stored_call.raw_arguments.find("plain-secret-value") == std::string::npos);
    CHECK(stored_call.raw_arguments.find("second-secret-value") == std::string::npos);
    CHECK(stored_call.raw_arguments.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("max turn limit stops infinite tool call loop", "[agent][u4]") {
    tests::FakeChatClient client;
    boost::json::object args;
    args["path"] = "missing.txt";
    client.push_response(tests::tool_response("loop-1", "read_file", args));
    client.push_response(tests::tool_response("loop-2", "read_file", args));
    std::vector<agent::LoopEvent> events;
    agent::ToolRegistry registry;
    registry.add(tools::make_read_file_tool());
    agent::LoopOptions options;
    options.max_turns = 2;
    options.on_event = [&](const agent::LoopEvent& event) { events.push_back(event); };
    agent::AgentLoop loop(client, registry, options);

    auto result = loop.run("loop forever");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == "max_turns_exceeded");
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().type == "max_turns");
}

namespace {
class SecretTool final : public agent::Tool {
public:
    agent::ToolDefinition definition() const override {
        return {"secret_tool", "return a fake secret", {}};
    }
    agent::ToolExecutionResult execute(const boost::json::object&, const agent::ToolContext&) override {
        return {"token=sk-123456789SECRET", false};
    }
};
}

TEST_CASE("secret-looking tool output is redacted before next provider request", "[agent][u4]") {
    tests::FakeChatClient client;
    boost::json::object args;
    client.push_response(tests::tool_response("secret-call", "secret_tool", args));
    client.push_response(tests::text_response("redacted"));
    agent::ToolRegistry registry;
    registry.add(std::make_shared<SecretTool>());
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("use secret tool");

    REQUIRE(result.ok());
    REQUIRE(client.requests.size() == 2);
    CHECK(client.requests[1].messages[2].content.find("sk-123456789SECRET") == std::string::npos);
    CHECK(client.requests[1].messages[2].content.find("[REDACTED]") != std::string::npos);
}
