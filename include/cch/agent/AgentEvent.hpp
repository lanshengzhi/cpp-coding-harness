#pragma once

#include <cch/ai/Message.hpp>
#include <cch/util/Error.hpp>

#include <functional>
#include <string>
#include <variant>

namespace cch::agent {

struct AgentStartEvent {
    std::string prompt;
};

struct TurnStartEvent {
    int turn{};
};

struct MessageStartEvent {
    int turn{};
};

struct MessageUpdateEvent {
    int turn{};
    std::string delta;
};

struct MessageEndEvent {
    int turn{};
    ai::AssistantMessage message;
};

struct ToolExecutionStartEvent {
    int turn{};
    std::string tool_call_id;
    std::string tool_name;
};

struct ToolExecutionEndEvent {
    int turn{};
    std::string tool_call_id;
    std::string tool_name;
    bool is_error{false};
    std::string content;
};

struct TurnEndEvent {
    int turn{};
    ai::AssistantStopReason stop_reason{ai::AssistantStopReason::Unknown};
};

struct AgentEndEvent {
    bool success{false};
    std::string reason;
};

using AgentLifecycleEvent = std::variant<
    AgentStartEvent,
    TurnStartEvent,
    MessageStartEvent,
    MessageUpdateEvent,
    MessageEndEvent,
    ToolExecutionStartEvent,
    ToolExecutionEndEvent,
    TurnEndEvent,
    AgentEndEvent>;

using AgentEventSink = std::function<util::ExpectedVoid(const AgentLifecycleEvent&)>;

} // namespace cch::agent
