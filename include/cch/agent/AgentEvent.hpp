#pragma once

#include "../ai/Message.hpp"
#include "../ai/StreamEvent.hpp"
#include "AgentTool.hpp"
#include "../util/Error.hpp"
#include "../util/JsonValue.hpp"

#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace cch::agent {

struct AgentStartEvent {};

struct AgentEndEvent {
    std::vector<ai::MessageVariant> messages;
};

struct TurnStartEvent {};

struct TurnEndEvent {
    ai::MessageVariant message;
    std::vector<ai::ToolResultMessage> tool_results;
};

struct MessageStartEvent {
    ai::MessageVariant message;
};

struct MessageUpdateEvent {
    ai::MessageVariant message;
    ai::AssistantStreamEvent assistant_event;
};

struct MessageEndEvent {
    ai::MessageVariant message;
};

struct ToolExecutionStartEvent {
    std::string tool_call_id;
    std::string tool_name;
    util::JsonValue args;
};

struct ToolExecutionEndEvent {
    std::string tool_call_id;
    std::string tool_name;
    AsyncToolExecutionResult result;
    bool is_error;
};

using AgentLifecycleEvent = std::variant<
    AgentStartEvent,
    AgentEndEvent,
    TurnStartEvent,
    TurnEndEvent,
    MessageStartEvent,
    MessageUpdateEvent,
    MessageEndEvent,
    ToolExecutionStartEvent,
    ToolExecutionEndEvent>;

using AgentEventSink = std::move_only_function<util::ExpectedVoid(const AgentLifecycleEvent&)>;

} // namespace cch::agent
