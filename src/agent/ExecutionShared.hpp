#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/awaitable.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::agent {

/// Invoke one Agent event sink and return its explicit failure outcome.
/// The single event-emit path shared by the agent loop and the tool-call
/// executor.
[[nodiscard]] inline support::ExpectedVoid emit_agent_event(
    AgentEventSink& sink,
    const AgentLifecycleEvent& event) {
    if (!sink) {
        return {};
    }
    return sink(event);
}

/// Emit the message lifecycle pair for one tool result message.
[[nodiscard]] inline support::ExpectedVoid emit_tool_result_message(
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

template <typename T>
struct AsyncResultTerminal;

template <typename T, typename E>
struct AsyncResultTerminal<support::AsyncResult<T, E>> {
    using type = std::expected<T, E>;
};

/// Invoke one asynchronous Agent policy hook and preserve its explicit
/// failure outcome. The consuming loop owns the private Asio bridge; Owner
/// Interfaces expose only `AsyncResult` operations.
template <typename Hook, typename... Args>
[[nodiscard]] boost::asio::awaitable<
    typename AsyncResultTerminal<std::invoke_result_t<Hook&, Args...>>::type>
invoke_agent_hook(
    std::string_view hook_name,
    Hook& hook,
    Args&&... args) {
    using Result = typename AsyncResultTerminal<std::invoke_result_t<Hook&, Args...>>::type;
    auto result = co_await support::detail::await_async_result(hook(std::forward<Args>(args)...));
    if (!result && result.error().message == "async operation failed") {
        co_return Result(std::unexpected(support::make_error(
            support::ErrorCode::Tool,
            std::string(hook_name) + " hook failed",
            result.error().detail)));
    }
    co_return result;
}

} // namespace cch::agent
