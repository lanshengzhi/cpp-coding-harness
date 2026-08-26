#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <stop_token>
#include <variant>
#include <vector>

namespace cch::harness::session {

class SessionStore;

/// Compaction thresholds and retention settings (pi `CompactionSettings`).
struct CompactionSettings {
    /// Enable automatic compaction decisions.
    bool enabled{true};
    /// Tokens reserved for the summary prompt and output (pi `reserveTokens`).
    std::size_t reserve_tokens{16384};
    /// Approximate recent-context tokens to keep after compaction
    /// (pi `keepRecentTokens`).
    std::size_t keep_recent_tokens{20000};
};

/// Default compaction settings used by the harness (pi
/// `DEFAULT_COMPACTION_SETTINGS`).
inline constexpr CompactionSettings kDefaultCompactionSettings{};

/// Calculate total context tokens from provider usage (pi
/// `calculateContextTokens`): the native `totalTokens` field wins, otherwise
/// the component sum.
[[nodiscard]] std::size_t calculate_context_tokens(const ai::Usage& usage);

/// Return whether context usage exceeds the configured compaction threshold
/// (pi `shouldCompact`): disabled settings never compact, otherwise
/// `contextTokens > contextWindow - reserveTokens`. The comparison is signed
/// so an unknown (zero) context window behaves exactly like pi's JS
/// arithmetic instead of underflowing.
[[nodiscard]] bool should_compact(
    std::size_t context_tokens,
    std::size_t context_window,
    const CompactionSettings& settings);

/// Detect a context-overflow assistant message (pi `isContextOverflow` in
/// `packages/ai/src/utils/overflow.ts`): an `error` terminal whose message
/// matches a provider overflow pattern (excluding rate-limit/throttling
/// patterns), a silent overflow whose `input + cacheRead` usage exceeds the
/// context window, or a `length` terminal whose zero-output usage fills the
/// window. `context_window` 0 disables the usage-based cases, mirroring pi's
/// truthiness gate.
[[nodiscard]] bool is_context_overflow(
    const ai::AssistantMessage& message,
    std::size_t context_window);

/// Estimate token count for one message using pi's conservative character
/// heuristic (chars/4, `estimateTokens` in harness/compaction/compaction.ts).
/// Images count as a fixed 4800 chars; unknown roles estimate 0.
[[nodiscard]] std::size_t estimate_tokens(const ai::MessageVariant& message);

/// Estimated context-token usage for a message list (pi
/// `ContextUsageEstimate` + `estimateContextTokens`).
struct ContextUsageEstimate {
    /// Estimated total context tokens.
    std::size_t tokens{0};
    /// Tokens reported by the most recent assistant usage block.
    std::size_t usage_tokens{0};
    /// Estimated tokens after the most recent assistant usage block.
    std::size_t trailing_tokens{0};
    /// Index of the message that provided usage, or nullopt when none exists.
    std::optional<std::size_t> last_usage_index;
};

/// Estimate context tokens for messages using provider usage when available.
[[nodiscard]] ContextUsageEstimate estimate_context_tokens(
    const std::vector<ai::MessageVariant>& messages);

/// The seam through which summarization streams run: the machinery builds the
/// request context and options (including `cacheRetention: "none"` and a
/// fresh session id) and the caller executes them through the session's
/// `ModelRuntime::streamSimple`, mirroring pi's harness passing `models` into
/// `compact`. The model used for summarization is the caller's captured model.
using SummarizationStreamFn = std::move_only_function<
    boost::asio::awaitable<support::Expected<ai::AssistantMessage>>(
        ai::AiContext context,
        ai::SimpleStreamOptions options)>;

/// Generates the fresh session id each summarization request carries so
/// compaction never pollutes the session's cache affinity (pi `uuidv7()`).
using SummarizationSessionIdFactory = std::move_only_function<std::string()>;

/// Invoked once after preparation succeeds and before the first
/// summarization request: the point where pi's auto trigger emits
/// `compaction_start`. The manual trigger emits `compaction_start` before
/// entering the door (it aborts the active run first) and leaves this unset;
/// a skipped run never invokes it, so the auto trigger emits no events for a
/// no-op check, matching pi `_runAutoCompaction`.
using CompactionStartHook = std::move_only_function<void()>;

/// Generated compaction data ready to be persisted as a compaction entry (pi
/// harness `CompactionResult`).
struct CompactionResult {
    /// Summary text that replaces compacted history in future context.
    std::string summary;
    /// Entry id where retained history starts.
    std::string first_kept_entry_id;
    /// Estimated context tokens before compaction.
    std::size_t tokens_before{0};
    /// Usage from the LLM call(s) that generated this summary, if available.
    std::optional<ai::Usage> usage;
    /// Retained recent messages stored directly on the compaction entry.
    std::vector<ai::MessageVariant> retained_tail;
    /// pi `CompactionDetails`: `{readFiles, modifiedFiles}`.
    std::optional<support::JsonValue> details;
};

/// Why one `compact` run did not apply, as a typed value: pi surfaces these
/// through `prepareCompaction` returning undefined and the session
/// re-inspecting the branch; the door returns the reason directly so callers
/// never re-read the module's knowledge.
struct CompactionSkipped {
    enum class Reason {
        /// The branch has no entries (pi's undefined-for-empty-path result).
        Empty,
        /// The branch's last entry is already a compaction (pi's
        /// "Already compacted" refusal).
        AlreadyCompacted,
        /// Nothing falls before the cut point (pi's
        /// "Nothing to compact (session too small)" refusal).
        NothingToSummarize,
    };
    Reason reason{Reason::NothingToSummarize};
};

/// The outcome of one `compact` door run.
using CompactionOutcomeVariant = std::variant<CompactionSkipped, CompactionResult>;

/// Options for one `compact` run (pi harness `compact`'s `customInstructions`,
/// `signal`, `thinkingLevel`, plus the injected seams).
struct CompactionRunOptions {
    /// Thresholds and retention budget for preparation and summarization.
    CompactionSettings settings;
    std::optional<std::string> custom_instructions;
    /// Effective thinking level; `off`/empty forwards no `reasoning`.
    std::string thinking_level;
    std::stop_token stop_token;
    SummarizationStreamFn summarization_stream;
    /// Defaults to a fresh uuid-style session id generator.
    SummarizationSessionIdFactory session_id_factory;
    /// See `CompactionStartHook`; unset on the manual path.
    CompactionStartHook on_compaction_start;
};

/// The one compaction door: pi's `prepareCompaction` + `compact` fused.
/// The store's live tree answers the branch query (pi `SessionManager
/// .getBranch`, root-to-leaf with the leaf last); preparation, cut-point
/// selection, file-operation extraction, and the summarization requests stay
/// inside the module. Summarization requests are issued through the injected
/// stream seam with `cacheRetention: "none"` and a fresh session id per
/// request. Persisting the compaction entry and rebuilding live context are
/// the caller's session-runtime jobs (pi `AgentSession.compact`). The `store`
/// and `model` arguments are borrowed and must outlive the returned operation,
/// which may suspend while summarization requests are running.
[[nodiscard]] boost::asio::awaitable<support::Expected<CompactionOutcomeVariant>> compact(
        SessionStore& store, const ai::Model& model, CompactionRunOptions run_options);

} // namespace cch::harness::session
