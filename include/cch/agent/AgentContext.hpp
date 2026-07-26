#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace cch::agent {

using TransformContextHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>>(
        std::vector<ai::MessageVariant>,
        std::stop_token)>;

/// pi's conversion policy does not receive an abort signal, but it still uses
/// the same awaitable move-only execution contract as every Agent policy.
using ConvertToLlmHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>>(
        std::vector<ai::MessageVariant>)>;

using SyncTransformContextPolicy = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>(
        std::vector<ai::MessageVariant>)>;
using CancellableSyncTransformContextPolicy = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>(
        std::vector<ai::MessageVariant>,
        std::stop_token)>;
using SyncConvertToLlmPolicy = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>(
        std::vector<ai::MessageVariant>)>;

[[nodiscard]] TransformContextHook adapt_sync_transform_context(
    SyncTransformContextPolicy policy);
[[nodiscard]] TransformContextHook adapt_sync_transform_context(
    CancellableSyncTransformContextPolicy policy);
[[nodiscard]] ConvertToLlmHook adapt_sync_convert_to_llm(
    SyncConvertToLlmPolicy policy);

using GetSteeringMessagesHook = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>()>;

using GetFollowUpMessagesHook = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>()>;

struct PrepareNextTurnContext {
    ai::AssistantMessage assistant_message;
    std::vector<ai::ToolResultMessage> tool_results;
    ai::AiContext context;
    std::vector<ai::MessageVariant> new_messages;
};

/// Replacement model-facing context for the next request in the current run.
/// Executable tool capabilities remain owned by the loop's tool registry.
struct AgentLoopContextReplacement {
    std::optional<std::string> system_prompt;
    std::vector<ai::MessageVariant> messages;
};

struct AgentLoopTurnUpdate {
    std::optional<AgentLoopContextReplacement> context;
    std::optional<ai::Model> model;
    std::optional<std::string> thinking_level;
};

using PrepareNextTurnHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<std::optional<AgentLoopTurnUpdate>>>(
        PrepareNextTurnContext)>;

using ShouldStopAfterTurnHook = std::move_only_function<
    boost::asio::awaitable<util::Expected<bool>>(PrepareNextTurnContext)>;

// Validates high-privilege turn updates before they are applied. Model changes
// require this hook until a provider/model registry validator is wired directly
// into the agent loop.
using ValidateTurnUpdateHook = std::move_only_function<
    boost::asio::awaitable<util::ExpectedVoid>(AgentLoopTurnUpdate)>;

using SyncPrepareNextTurnPolicy = std::move_only_function<
    util::Expected<std::optional<AgentLoopTurnUpdate>>(PrepareNextTurnContext)>;
using SyncShouldStopAfterTurnPolicy = std::move_only_function<
    util::Expected<bool>(PrepareNextTurnContext)>;
using SyncValidateTurnUpdatePolicy = std::move_only_function<
    util::ExpectedVoid(AgentLoopTurnUpdate)>;

[[nodiscard]] PrepareNextTurnHook adapt_sync_prepare_next_turn(
    SyncPrepareNextTurnPolicy policy);
[[nodiscard]] ShouldStopAfterTurnHook adapt_sync_should_stop_after_turn(
    SyncShouldStopAfterTurnPolicy policy);
[[nodiscard]] ValidateTurnUpdateHook adapt_sync_validate_turn_update(
    SyncValidateTurnUpdatePolicy policy);

struct SequentialToolExecution {};

struct BoundedParallelToolExecution {
    std::size_t max_in_flight{1};
};

using ToolExecutionPolicy = std::variant<
    SequentialToolExecution,
    BoundedParallelToolExecution>;

struct AsyncAgentOptions {
    /// Maximum model turns for one run. std::nullopt (the default) imposes no
    /// turn cap: a run stops through pi-aligned terminal conditions, the
    /// stop-after-turn policy, termination decisions, or cancellation
    /// (ADR 0015). An explicitly set cap ends the run with a validation error
    /// once the budget is exhausted; exhaustion is never reported as a
    /// provider error.
    std::optional<int> max_turns{std::nullopt};
    /// Admission bounds applied to each steering/follow-up queue validation
    /// pass (ADR 0022). Defaults: 256 messages and 16 MiB of approximate
    /// message content.
    std::size_t max_queued_messages{256};
    std::size_t max_queued_bytes{16 * 1024 * 1024};
    ai::Model model;
    std::string thinking_level;
    std::optional<BeforeToolCallHook> before_tool_call;
    std::optional<AfterToolCallHook> after_tool_call;
    std::optional<TransformContextHook> transform_context;
    std::optional<ConvertToLlmHook> convert_to_llm;
    std::optional<GetSteeringMessagesHook> get_steering_messages;
    std::optional<GetFollowUpMessagesHook> get_follow_up_messages;
    std::optional<PrepareNextTurnHook> prepare_next_turn;
    std::optional<ShouldStopAfterTurnHook> should_stop_after_turn;
    std::optional<ValidateTurnUpdateHook> validate_turn_update;
    ToolExecutionPolicy tool_execution{SequentialToolExecution{}};
};

struct AgentState {
    std::vector<ai::MessageVariant> messages;
    bool is_running{false};
    std::optional<ai::AssistantMessage> streaming_message;
    std::vector<std::string> active_tool_names;
    std::vector<std::string> pending_tool_call_ids;
    ai::Model model;
    std::string thinking_level;
    // Bounded observations reported without vetoing run progress: redacted
    // weak-subscriber failures and rejected steering/follow-up queue
    // admissions (ADR 0022).
    std::vector<util::Error> diagnostics;
};

} // namespace cch::agent
