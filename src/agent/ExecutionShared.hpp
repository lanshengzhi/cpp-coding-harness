#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/util/Error.hpp>

#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {

/// Invoke one weak agent event sink with exception containment (ADR 0017).
/// The single event-emit path shared by the agent loop and the tool-call
/// executor.
[[nodiscard]] inline util::ExpectedVoid emit_agent_event(
    AgentEventSink& sink,
    const AgentLifecycleEvent& event) {
    if (!sink) {
        return {};
    }
    try {
        return sink(event);
    } catch (const std::exception& e) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "agent event sink failed",
            e.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            "agent event sink failed",
            "unknown exception"));
    }
}

/// Emit the message lifecycle pair for one tool result message.
[[nodiscard]] inline util::ExpectedVoid emit_tool_result_message(
    AgentEventSink& sink,
    const ai::ToolResultMessage& message) {
    if (auto r = emit_agent_event(sink, MessageStartEvent{ai::MessageVariant{message}}); !r) {
        return r;
    }
    return emit_agent_event(sink, MessageEndEvent{ai::MessageVariant{message}});
}

/// Extract the tool calls carried by one assistant message.
[[nodiscard]] inline std::vector<ai::ToolCallContent> tool_calls_from(
    const ai::AssistantMessage& message) {
    std::vector<ai::ToolCallContent> calls;
    for (const auto& block : message.content) {
        if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
            calls.push_back(*call);
        }
    }
    return calls;
}

/// Invoke one agent policy hook with exception containment. hook_name is the
/// pi wire vocabulary name reported in the failure diagnostic.
template <typename Hook, typename... Args>
[[nodiscard]] std::invoke_result_t<Hook&, Args&&...> invoke_agent_hook(
    std::string_view hook_name,
    Hook& hook,
    Args&&... args) {
    using Result = std::invoke_result_t<Hook&, Args&&...>;
    try {
        return hook(std::forward<Args>(args)...);
    } catch (const std::exception& e) {
        return Result(std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            std::string(hook_name) + " hook failed",
            e.what())));
    } catch (...) {
        return Result(std::unexpected(util::make_error(
            util::ErrorCode::Tool,
            std::string(hook_name) + " hook failed",
            "unknown exception")));
    }
}

} // namespace cch::agent
