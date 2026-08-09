#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>
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

namespace detail {

/// Mirrors pi Agent's internal DEFAULT_MODEL at parity baseline 83114817
/// (`packages/agent/src/agent.ts`). Concrete Agent Model when no real model has
/// been resolved; streaming against it fails through normal provider lookup
/// until a real model is selected (ADR 0034 / #326).
inline const ai::Model kDefaultModel{
    .id = "unknown",
    .name = "unknown",
    .api = "unknown",
    .provider = "unknown",
    .base_url = "",
    .reasoning = false,
    .thinking_level_map = std::nullopt,
    .input = {},
    .cost = {},
    .context_window = 0,
    .max_tokens = 0,
    .headers = std::nullopt,
    .compat = std::nullopt,
};

} // namespace detail

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

/// pi-compatible policy for draining one pending input queue.
enum class InputQueueMode { OneAtATime, All };

inline constexpr std::size_t kDefaultMaxQueuedMessages = 256;
inline constexpr std::size_t kDefaultMaxQueuedBytes = 16 * 1024 * 1024;

/// Passive configuration and pending contents for one Agent-owned input queue.
struct AgentInputQueue {
    InputQueueMode mode{InputQueueMode::OneAtATime};
    std::vector<ai::MessageVariant> messages;
};

/// Passive observation of both Agent-owned input queues and their shared
/// per-queue admission limits.
struct AgentInputQueues {
    std::size_t max_messages{kDefaultMaxQueuedMessages};
    std::size_t max_bytes{kDefaultMaxQueuedBytes};
    AgentInputQueue steering;
    AgentInputQueue follow_up;
};

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
    std::optional<AgentLoopContextReplacement> context{std::nullopt};
    std::optional<ai::Model> model{std::nullopt};
    std::optional<std::string> thinking_level{std::nullopt};
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
    /// Maximum concurrently executing prepared tool calls. 0 (the default)
    /// means no explicit cap: every prepared call in the batch is dispatched
    /// concurrently, matching pi's unbounded parallel default (ADR 0034),
    /// subject to the per-tool parallel-safety bound (ADR 0016). An explicit
    /// value of 1 executes the batch through the sequential path.
    std::size_t max_in_flight{0};
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
    /// Per-queue admission limits (ADR 0022). Defaults: 256 messages and
    /// 16 MiB of approximate message content.
    std::size_t max_queued_messages{kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{kDefaultMaxQueuedBytes};
    InputQueueMode steering_mode{InputQueueMode::OneAtATime};
    InputQueueMode follow_up_mode{InputQueueMode::OneAtATime};
    /// Concrete Model for every turn, forwarded to `streamSimple` exactly as
    /// configured. Defaults to the pi-aligned unknown `kDefaultModel`; session
    /// assembly replaces it with the first real model (ADR 0034). The Agent
    /// applies no silent per-request placeholder substitution.
    ai::Model model{detail::kDefaultModel};
    /// Session identifier forwarded as the per-turn `sessionId` streamSimple
    /// option (pi AgentOptions.sessionId / harness `sessionMetadata.id`). Empty
    /// means the host provided none.
    std::string session_id;
    /// The session System Prompt (pi `AgentState.systemPrompt`, built by the
    /// harness at session construction): seeded into every per-run request
    /// `AiContext.system_prompt`, exactly like pi's `createContextSnapshot`.
    /// Empty (the default) forwards no system prompt, mirroring pi's default
    /// `""` and the adapters' empty-string guard.
    std::string system_prompt{};
    /// Thinking level for the run. Empty means the pi `DEFAULT_THINKING_LEVEL`
    /// ("medium") is requested; the loop normalizes it and clamps the request
    /// against the active model's supported set at construction and on model
    /// switch (ADR 0034 / #352). Per turn the effective level becomes the
    /// stream `reasoning` option exactly like pi's harness consumer: `off`
    /// forwards no reasoning, any other of the seven levels is forwarded as
    /// the stream reasoning (pi `createLoopConfig` / `agent-harness.ts`
    /// `thinkingLevel === "off" ? undefined : thinkingLevel`).
    std::string thinking_level;
    /// Per-turn `cacheRetention` streamSimple option. Unset (the default)
    /// resolves to the pi-aligned `"short"` retention; compaction is the only
    /// agent-core consumer that overrides it, with `"none"` and a fresh
    /// session id (ADR 0033 / ADR 0034).
    std::optional<ai::CacheRetention> cache_retention{std::nullopt};
    /// Per-turn `timeoutMs` streamSimple option. Unset forwards the stream
    /// default (30 s on the scoped adapters, matching pi's SDK defaults).
    std::optional<std::uint64_t> timeout_ms{std::nullopt};
    /// Per-turn `maxRetries` streamSimple option. 0 (the default) forwards the
    /// stream default, which is no client-side retry on the scoped adapters;
    /// transient-failure recovery lives in the agent-level turn auto-retry
    /// capability (ADR 0034), not the stream layer.
    std::uint32_t max_retries{0};
    /// Per-turn `maxRetryDelayMs` streamSimple option. Unset resolves to the
    /// pi default of 60000 ms at the Models layer.
    std::optional<std::uint64_t> max_retry_delay_ms{std::nullopt};
    /// Per-turn `headers` streamSimple option, merged with resolved auth and
    /// lifecycle headers by Models before provider dispatch. Empty means the
    /// host configured none.
    ai::RequestHeaders headers{};
    std::optional<BeforeToolCallHook> before_tool_call;
    std::optional<AfterToolCallHook> after_tool_call;
    std::optional<TransformContextHook> transform_context;
    std::optional<ConvertToLlmHook> convert_to_llm;
    std::optional<PrepareNextTurnHook> prepare_next_turn;
    std::optional<ShouldStopAfterTurnHook> should_stop_after_turn;
    std::optional<ValidateTurnUpdateHook> validate_turn_update;
    /// Tool scheduling policy for each assistant tool-call batch. Defaults to
    /// bounded parallel with no explicit cap (pi's parallel default): calls in
    /// one assistant message execute concurrently, while a batch containing a
    /// call to a tool whose adapter declares `ToolConcurrency::Exclusive` runs
    /// through the sequential path exactly like pi's per-tool
    /// `executionMode: "sequential"` override (ADR 0034 / #355).
    ToolExecutionPolicy tool_execution{BoundedParallelToolExecution{}};
};

struct AgentState {
    /// The session System Prompt (pi `AgentState.systemPrompt`): the value
    /// seeded into per-run request contexts from `AsyncAgentOptions`, kept in
    /// sync with the loop options like the model and thinking level.
    std::string system_prompt{};
    std::vector<ai::MessageVariant> messages;
    bool is_running{false};
    std::optional<ai::AssistantMessage> streaming_message;
    std::vector<std::string> active_tool_names;
    std::vector<std::string> pending_tool_call_ids;
    AgentInputQueues input_queues;
    ai::Model model{};
    std::string thinking_level;
    // Bounded observations reported without vetoing run progress: redacted
    // weak-subscriber failures (ADR 0017).
    std::vector<util::Error> diagnostics;
};

} // namespace cch::agent
