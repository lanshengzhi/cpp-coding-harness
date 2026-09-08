#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <optional>
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
    /// Structured Provider/transport outcome for an assistant error. This is
    /// event metadata, not part of the persisted/model-facing message.
    std::optional<ai::InferenceFailure> inference_failure{std::nullopt};
};

struct ToolExecutionStartEvent {
    std::string tool_call_id;
    std::string tool_name;
    support::JsonValue args;
};

struct ToolExecutionUpdateEvent {
    std::string tool_call_id{};
    std::string tool_name{};
    support::JsonValue args{};
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

/// Weak lifecycle observer used by Agent subscriptions. Reported failures are
/// diagnostic observations and cannot veto Agent progress.
using AgentEventSink = std::move_only_function<support::ExpectedVoid(const AgentLifecycleEvent&)>;

/// Strong per-run lifecycle participant. Unlike an AgentEventSink subscription,
/// a failure vetoes further execution after live state and weak observers have
/// already observed the event. This seam is intended for named commitment
/// capabilities such as durable persistence, not ordinary presentation.
using AgentEventCommitter = std::move_only_function<support::ExpectedVoid(const AgentLifecycleEvent&)>;

} // namespace cch::agent
