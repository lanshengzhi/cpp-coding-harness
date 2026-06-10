#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/AgentEvent.hpp"

using namespace cch;

TEST_CASE("agent event kinds expose stable semantic names", "[agent][u4]") {
    CHECK(agent::to_string(agent::AgentEventKind::AgentStart) == "agent_start");
    CHECK(agent::to_string(agent::AgentEventKind::TurnStart) == "turn_start");
    CHECK(agent::to_string(agent::AgentEventKind::ModelRequest) == "model_request");
    CHECK(agent::to_string(agent::AgentEventKind::UserMessage) == "user_message");
    CHECK(agent::to_string(agent::AgentEventKind::AssistantMessage) == "assistant_message");
    CHECK(agent::to_string(agent::AgentEventKind::ToolExecutionStart) == "tool_execution_start");
    CHECK(agent::to_string(agent::AgentEventKind::ToolExecutionEnd) == "tool_execution_end");
    CHECK(agent::to_string(agent::AgentEventKind::TurnEnd) == "turn_end");
    CHECK(agent::to_string(agent::AgentEventKind::AgentEnd) == "agent_end");
}

TEST_CASE("tool lifecycle events carry call metadata", "[agent][u4]") {
    auto start = agent::AgentEvent::tool_start("call-1", "read_file", 2);
    CHECK(start.kind == agent::AgentEventKind::ToolExecutionStart);
    CHECK(start.turn == 2);
    CHECK(start.tool_call_id == "call-1");
    CHECK(start.tool_name == "read_file");
    CHECK(start.detail == "read_file#call-1");

    auto end = agent::AgentEvent::tool_end("call-1", "read_file", true, 2);
    CHECK(end.kind == agent::AgentEventKind::ToolExecutionEnd);
    CHECK(end.turn == 2);
    CHECK(end.tool_call_id == "call-1");
    CHECK(end.tool_name == "read_file");
    CHECK(end.is_error);
}
