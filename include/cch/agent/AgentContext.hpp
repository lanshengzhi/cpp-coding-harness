#pragma once

#include "AgentTool.hpp"
#include "../ai/Context.hpp"
#include "../ai/Tool.hpp"
#include "../util/Error.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cch::agent {

using TransformContextHook = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>(const std::vector<ai::MessageVariant>&)>;

using ConvertToLlmHook = std::move_only_function<
    util::Expected<std::vector<ai::MessageVariant>>(const std::vector<ai::MessageVariant>&)>;

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
    std::optional<std::string> model;
    std::optional<std::string> thinking_level;
};

using PrepareNextTurnHook = std::move_only_function<
    util::Expected<std::optional<AgentLoopTurnUpdate>>(const PrepareNextTurnContext&)>;

using ShouldStopAfterTurnHook = std::move_only_function<
    util::Expected<bool>(const PrepareNextTurnContext&)>;

// Validates high-privilege turn updates before they are applied. Model changes
// require this hook until a provider/model registry validator is wired directly
// into the agent loop.
using ValidateTurnUpdateHook = std::move_only_function<
    util::ExpectedVoid(const AgentLoopTurnUpdate&)>;

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
    std::optional<int> max_turns;
    /// Admission bounds applied to each steering/follow-up queue validation
    /// pass (ADR 0022). Defaults: 256 messages and 16 MiB of approximate
    /// message content.
    std::size_t max_queued_messages{256};
    std::size_t max_queued_bytes{16 * 1024 * 1024};
    std::string model;
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

    AsyncAgentOptions() = default;
    AsyncAgentOptions(AsyncAgentOptions&&) = default;
    AsyncAgentOptions& operator=(AsyncAgentOptions&&) = default;
    AsyncAgentOptions(const AsyncAgentOptions&) = delete;
    AsyncAgentOptions& operator=(const AsyncAgentOptions&) = delete;

    AsyncAgentOptions(int max_turns_, std::string model_)
        : max_turns(max_turns_), model(std::move(model_)) {}
};

struct AgentState {
    std::vector<ai::MessageVariant> messages;
    bool is_running{false};
    std::optional<ai::AssistantMessage> streaming_message;
    std::vector<std::string> active_tool_names;
    std::vector<std::string> pending_tool_call_ids;
    std::string model;
    std::string thinking_level;
    // Bounded observations reported without vetoing run progress: redacted
    // weak-subscriber failures and rejected steering/follow-up queue
    // admissions (ADR 0022).
    std::vector<util::Error> diagnostics;
};

} // namespace cch::agent
