#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::agent::detail {

enum class InputQueueKind { Steering, FollowUp };

/// Owned facts needed to start one serialized Agent turn. The live AgentState
/// remains in Agent::Impl; this value is the coroutine's per-turn snapshot.
struct AgentExecutionSnapshot {
    ai::Model model;
    std::string thinking_level;
    std::string system_prompt;
    std::vector<ai::MessageVariant> messages;
    std::vector<ai::Tool> tools;
};

/// Run configuration that is not live Agent state. It is owned by Agent::Impl
/// and remains alive for every execution coroutine started by that Agent.
struct RunPolicy {
    ai::ModelStreamFactory stream_factory;
    ToolRegistry registry;
    std::string session_id;
    std::optional<int> max_turns{std::nullopt};
    std::optional<ai::CacheRetention> cache_retention{std::nullopt};
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    std::uint32_t max_retries{0};
    std::optional<std::uint64_t> max_retry_delay_ms{std::nullopt};
    ai::RequestHeaders headers{};
    std::optional<BeforeToolCallHook> before_tool_call{std::nullopt};
    std::optional<AfterToolCallHook> after_tool_call{std::nullopt};
    std::optional<TransformContextHook> transform_context{std::nullopt};
    std::optional<ConvertToLlmHook> convert_to_llm{std::nullopt};
    std::optional<PrepareNextTurnHook> prepare_next_turn{std::nullopt};
    std::optional<ShouldStopAfterTurnHook> should_stop_after_turn{std::nullopt};
    std::optional<ValidateTurnUpdateHook> validate_turn_update{std::nullopt};
    ToolExecutionPolicy tool_execution{BoundedParallelToolExecution{}};
};

/// The only Agent-to-loop callback surface: snapshots are owned values,
/// emission reduces immediately through Agent::Impl, updates are validated and
/// applied by Agent::Impl, and queue draining is synchronous.
struct AgentExecutionCallbacks {
    std::move_only_function<AgentExecutionSnapshot()> snapshot;
    AgentEventSink emit;
    std::move_only_function<support::Expected<AgentExecutionSnapshot>(AgentLoopTurnUpdate)> apply_update;
    std::move_only_function<std::vector<ai::MessageVariant>(InputQueueKind)> drain;
};

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_agent_execution(RunPolicy& policy,
        AgentExecutionCallbacks callbacks,
        std::optional<ai::UserMessage> user_message,
        std::stop_token stop_token);

} // namespace cch::agent::detail
