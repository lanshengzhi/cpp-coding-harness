#pragma once

#include <string>
#include <utility>

namespace cch::agent {

enum class AgentEventKind {
    AgentStart,
    TurnStart,
    ModelRequest,
    UserMessage,
    AssistantMessage,
    ToolExecutionStart,
    ToolExecutionEnd,
    TurnEnd,
    AgentEnd,
    ProviderError,
    MaxTurns,
};

inline std::string to_string(AgentEventKind kind) {
    switch (kind) {
    case AgentEventKind::AgentStart:
        return "agent_start";
    case AgentEventKind::TurnStart:
        return "turn_start";
    case AgentEventKind::ModelRequest:
        return "model_request";
    case AgentEventKind::UserMessage:
        return "user_message";
    case AgentEventKind::AssistantMessage:
        return "assistant_message";
    case AgentEventKind::ToolExecutionStart:
        return "tool_execution_start";
    case AgentEventKind::ToolExecutionEnd:
        return "tool_execution_end";
    case AgentEventKind::TurnEnd:
        return "turn_end";
    case AgentEventKind::AgentEnd:
        return "agent_end";
    case AgentEventKind::ProviderError:
        return "provider_error";
    case AgentEventKind::MaxTurns:
        return "max_turns";
    }
    return "unknown";
}

struct AgentEvent {
    AgentEventKind kind{AgentEventKind::AgentStart};
    int turn{0};
    std::string detail;
    std::string tool_call_id;
    std::string tool_name;
    bool is_error{false};

    static AgentEvent make(AgentEventKind kind, std::string detail = {}, int turn = 0) {
        AgentEvent event;
        event.kind = kind;
        event.detail = std::move(detail);
        event.turn = turn;
        return event;
    }

    static AgentEvent tool_start(std::string id, std::string name, int turn) {
        AgentEvent event = make(AgentEventKind::ToolExecutionStart, name + "#" + id, turn);
        event.tool_call_id = std::move(id);
        event.tool_name = std::move(name);
        return event;
    }

    static AgentEvent tool_end(std::string id, std::string name, bool error, int turn) {
        AgentEvent event = make(AgentEventKind::ToolExecutionEnd, id, turn);
        event.tool_call_id = std::move(id);
        event.tool_name = std::move(name);
        event.is_error = error;
        return event;
    }
};

} // namespace cch::agent
