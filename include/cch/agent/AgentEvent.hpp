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

struct ToolExecutionUpdateEvent {
    std::string tool_call_id{};
    std::string tool_name{};
    util::JsonValue args{};
    AsyncToolExecutionResult partial_result{};
};

struct ToolExecutionEndEvent {
    std::string tool_call_id;
    std::string tool_name;
    AsyncToolExecutionResult result;
    bool is_error{false};
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
    ToolExecutionUpdateEvent,
    ToolExecutionEndEvent>;

/// Weak lifecycle observer used by Agent subscriptions. Reported failures and
/// exceptions are diagnostic observations and cannot veto Agent progress.
using AgentEventSink = std::move_only_function<util::ExpectedVoid(const AgentLifecycleEvent&)>;

/// Strong per-run lifecycle participant. Unlike an AgentEventSink subscription,
/// a failure vetoes further execution after live state and weak observers have
/// already observed the event. This seam is intended for named commitment
/// capabilities such as durable persistence, not ordinary presentation.
using AgentEventCommitter = std::move_only_function<util::ExpectedVoid(const AgentLifecycleEvent&)>;

} // namespace cch::agent
