#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/AgentTool.hpp"

using namespace cch;

TEST_CASE("agent tool call preserves provider call metadata", "[agent][u4]") {
    agent::ToolCall legacy;
    legacy.id = "call-1";
    legacy.name = "write_file";
    legacy.arguments = {{"path", "out.txt"}};
    legacy.raw_arguments = R"({"path":"out.txt"})";

    auto call = agent::to_agent_tool_call(legacy);

    CHECK(call.id == "call-1");
    CHECK(call.name == "write_file");
    CHECK(call.raw_arguments == R"({"path":"out.txt"})");
    CHECK(std::string(call.arguments.at("path").as_string()) == "out.txt");

    auto round_trip = agent::tool_call_from_agent(call);
    CHECK(round_trip.id == legacy.id);
    CHECK(round_trip.name == legacy.name);
    CHECK(round_trip.raw_arguments == legacy.raw_arguments);
}

TEST_CASE("agent tool result converts to tool-result message with name and error flag", "[agent][u4]") {
    agent::AgentToolResult result;
    result.tool_call_id = "call-1";
    result.tool_name = "read_file";
    result.content = "path does not exist";
    result.is_error = true;

    auto message = result.to_message();

    CHECK(message.role == agent::Role::Tool);
    CHECK(message.tool_call_id == "call-1");
    CHECK(message.tool_name == "read_file");
    CHECK(message.content == "path does not exist");
    CHECK(message.is_error);
}
