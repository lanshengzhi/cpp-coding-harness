#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Models.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::agent {

/// Private result carrier for one coroutine-loop invocation.
struct AsyncAgentRunResult {
    ai::AiContext context;
    ai::AssistantStopReason stop_reason{ai::AssistantStopReason::Stop};
    int turns{0};
    AgentState state;
};

/// Private execution machinery owned by the stateful Agent. Issues every turn
/// through the AI-owned `ModelStream` seam (ADR 0040 / #453).
class AsyncAgentLoop {
public:
    AsyncAgentLoop(
        ai::ModelStreamFactory stream_factory,
        AsyncToolRegistry registry,
        AsyncAgentOptions options = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> run(
        std::string user_prompt,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        std::string user_prompt,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        ai::UserMessage user_message,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

    /// Continue the loop without a new user message (pi `agent.continue()` /
    /// `runAgentLoopContinue`), used by the session assembly's overflow
    /// compact-and-retry-once: the context already ends in a user or tool
    /// result message. Rejects an empty history and a history whose last
    /// message is an assistant message (pi's "Cannot continue from message
    /// role: assistant").
    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

private:
    friend class Agent;

    enum class InputQueueKind { Steering, FollowUp };
    using InputQueueDrainer = std::move_only_function<
        std::vector<ai::MessageVariant>(InputQueueKind queue_kind)>;

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        std::optional<ai::UserMessage> user_message,
        AgentEventSink sink,
        std::stop_token stop_token,
        InputQueueDrainer drain_queued_messages);

    [[nodiscard]] const ai::Model& current_model() const noexcept {
        return options_.model;
    }
    [[nodiscard]] const std::string& current_thinking_level() const noexcept {
        return options_.thinking_level;
    }
    /// The session System Prompt seeded into every per-run request context
    /// (pi `state.systemPrompt`).
    [[nodiscard]] const std::string& current_system_prompt() const noexcept {
        return options_.system_prompt;
    }
    /// Apply a clamped thinking level for subsequent turns (pi
    /// `agent-session.ts` setThinkingLevel). The caller has already clamped
    /// the request against the active model; this keeps the loop's option and
    /// the live Agent state in agreement for the next stream request.
    void set_thinking_level(std::string level) noexcept {
        options_.thinking_level = std::move(level);
    }
    /// Swap the turn Model for subsequent runs (pi `agent.state.model = model`
    /// in `setModel`). The caller has validated the Model and re-clamps the
    /// thinking level right after; this keeps the loop's option and the live
    /// Agent state in agreement for the next stream request.
    void set_model(ai::Model model) noexcept {
        options_.model = std::move(model);
    }
    /// Replace the session System Prompt for subsequent runs (pi
    /// `agent.state.systemPrompt = ...` on `/reload` rebuild). The caller
    /// keeps the loop's option and the live Agent state in agreement.
    void set_system_prompt(std::string system_prompt) noexcept {
        options_.system_prompt = std::move(system_prompt);
    }
    [[nodiscard]] const std::string& session_id() const noexcept {
        return options_.session_id;
    }
    [[nodiscard]] std::size_t max_queued_messages() const noexcept {
        return options_.max_queued_messages;
    }
    [[nodiscard]] std::size_t max_queued_bytes() const noexcept {
        return options_.max_queued_bytes;
    }
    [[nodiscard]] InputQueueMode steering_mode() const noexcept {
        return options_.steering_mode;
    }
    [[nodiscard]] InputQueueMode follow_up_mode() const noexcept {
        return options_.follow_up_mode;
    }

    ai::ModelStreamFactory stream_factory_; // move-only; produces one ModelStream per turn
    AsyncToolRegistry registry_;
    AsyncAgentOptions options_;
};

} // namespace cch::agent
