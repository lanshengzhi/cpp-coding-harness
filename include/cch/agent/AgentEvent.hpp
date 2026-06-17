#pragma once

#include "../ai/Message.hpp"
#include "../util/Error.hpp"
#include <cstddef>
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

struct ThinkingUpdateEvent {
    int turn{};
    std::size_t content_index{};
    std::string delta;
};

struct ToolCallStreamStartEvent {
    int turn{};
    std::size_t content_index{};
};

struct ToolCallStreamUpdateEvent {
    int turn{};
    std::size_t content_index{};
    std::string delta;
};

struct ToolCallStreamEndEvent {
    int turn{};
    std::size_t content_index{};
    ai::ToolCallContent tool_call;
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
    ThinkingUpdateEvent,
    ToolCallStreamStartEvent,
    ToolCallStreamUpdateEvent,
    ToolCallStreamEndEvent,
    ToolExecutionStartEvent,
    ToolExecutionEndEvent,
    TurnEndEvent,
    AgentEndEvent>;

using AgentEventSink = std::move_only_function<util::ExpectedVoid(const AgentLifecycleEvent&)>;

} // namespace cch::agent
