#pragma once

#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/harness/session/SessionEntry.hpp>
#include <cch/harness/session/SessionTree.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace cch::harness::session {

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

/// Estimate token count for one message using pi's conservative character
/// heuristic (chars/4, `estimateTokens` in harness/compaction/compaction.ts).
/// Images count as a fixed 4800 chars; unknown roles estimate 0.
[[nodiscard]] std::size_t estimate_tokens(const ai::MessageVariant& message);

/// Return the usage of the last valid assistant message in a message list:
/// pi skips `aborted`/`error` stop reasons and all-zero usage
/// (`getLastAssistantUsage`).
[[nodiscard]] std::optional<ai::Usage> get_last_assistant_usage(
    const std::vector<ai::MessageVariant>& messages);

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

/// Cut point selected for compaction (pi `CutPointResult`).
struct CutPointResult {
    /// Index of the first entry retained after compaction.
    std::size_t first_kept_entry_index{0};
    /// Index of the turn-start entry when the cut splits a turn, otherwise -1.
    std::ptrdiff_t turn_start_index{-1};
    /// Whether the selected cut point splits an in-progress turn.
    bool is_split_turn{false};
};

/// Find the compaction cut point that keeps approximately the requested
/// recent-token budget (pi `findCutPoint` on a root-to-leaf entry path).
[[nodiscard]] CutPointResult find_cut_point(
    const std::vector<const SessionEntry*>& path,
    std::size_t start_index,
    std::size_t end_index,
    std::size_t keep_recent_tokens);

/// Find the user-visible message that starts the turn containing an entry
/// (pi `findTurnStartIndex`).
[[nodiscard]] std::ptrdiff_t find_turn_start_index(
    const std::vector<const SessionEntry*>& path,
    std::size_t entry_index,
    std::size_t start_index);

/// File paths touched by a session branch or compaction range (pi
/// `FileOperations`).
struct FileOperations {
    /// Files read but not necessarily modified.
    std::set<std::string> read;
    /// Files written by full-file write operations.
    std::set<std::string> written;
    /// Files modified by edit operations.
    std::set<std::string> edited;
};

/// Add file operations from assistant tool calls to an accumulator (pi
/// `extractFileOpsFromMessage`).
void extract_file_ops_from_message(
    const ai::MessageVariant& message,
    FileOperations& file_ops);

/// Compute sorted read-only and modified file lists from accumulated
/// operations (pi `computeFileLists`).
struct FileLists {
    std::vector<std::string> read_files;
    std::vector<std::string> modified_files;
};

[[nodiscard]] FileLists compute_file_lists(const FileOperations& file_ops);

/// Format file lists as summary metadata tags (pi `formatFileOperations`).
[[nodiscard]] std::string format_file_operations(
    const std::vector<std::string>& read_files,
    const std::vector<std::string>& modified_files);

/// Serialize LLM messages to plain text for summarization prompts (pi
/// `serializeConversation` in harness/compaction/utils.ts: `[User]:`,
/// `[Assistant thinking]:`, `[Assistant]:`, `[Assistant tool calls]:`,
/// `[Tool result]:` with 2000-char truncation).
[[nodiscard]] std::string serialize_conversation(
    const std::vector<ai::MessageVariant>& messages);

/// Prepared inputs for a compaction run (pi `CompactionPreparation`).
struct CompactionPreparation {
    /// Entry id where retained history starts.
    std::string first_kept_entry_id;
    /// Messages summarized into the history summary.
    std::vector<ai::MessageVariant> messages_to_summarize;
    /// Prefix messages summarized separately when compaction splits a turn.
    std::vector<ai::MessageVariant> turn_prefix_messages;
    /// Recent messages retained after compaction and stored on the compaction
    /// entry.
    std::vector<ai::MessageVariant> retained_tail;
    /// Whether compaction splits a turn.
    bool is_split_turn{false};
    /// Estimated context tokens before compaction.
    std::size_t tokens_before{0};
    /// Previous compaction summary used for iterative updates.
    std::optional<std::string> previous_summary;
    /// File operations extracted from summarized history.
    FileOperations file_ops;
    /// Settings used to prepare compaction.
    CompactionSettings settings;
};

/// Prepare session entries for compaction, or return nullopt when compaction
/// is not applicable (pi `prepareCompaction`: an empty path or a path whose
/// last entry is already a compaction). The path is root-to-leaf, matching pi
/// `SessionManager.getBranch()`.
[[nodiscard]] util::Expected<std::optional<CompactionPreparation>>
prepare_compaction(
    const std::vector<const SessionEntry*>& path,
    CompactionSettings settings);

/// The seam through which summarization streams run: the machinery builds the
/// request context and options (including `cacheRetention: "none"` and a
/// fresh session id) and the caller executes them through the session's
/// `ModelRuntime::streamSimple`, mirroring pi's harness passing `models` into
/// `compact`. The model used for summarization is the caller's captured model.
using SummarizationStreamFn = std::move_only_function<
    boost::asio::awaitable<util::Expected<ai::AssistantMessage>>(
        ai::AiContext context,
        ai::SimpleStreamOptions options)>;

/// Generates the fresh session id each summarization request carries so
/// compaction never pollutes the session's cache affinity (pi `uuidv7()`).
using SummarizationSessionIdFactory = std::move_only_function<std::string()>;

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
    std::optional<util::JsonValue> details;
};

/// Options for one `compact` run (pi harness `compact`'s `customInstructions`,
/// `signal`, `thinkingLevel`, plus the injected stream seam).
struct CompactionRunOptions {
    std::optional<std::string> custom_instructions;
    /// Effective thinking level; `off`/empty forwards no `reasoning`.
    std::string thinking_level;
    std::stop_token stop_token;
    SummarizationStreamFn summarization_stream;
    /// Defaults to a fresh uuid-style session id generator.
    SummarizationSessionIdFactory session_id_factory;
};

/// Generate compaction summary data from prepared session history (pi harness
/// `compact`). Summarization requests are issued through the injected stream
/// seam with `cacheRetention: "none"` and a fresh session id per request.
[[nodiscard]] boost::asio::awaitable<util::Expected<CompactionResult>> compact(
    const CompactionPreparation& preparation,
    const ai::Model& model,
    CompactionRunOptions run_options);

// ── Verbatim pi summarization prompts (harness/compaction/compaction.ts) ────

inline constexpr std::string_view kSummarizationSystemPrompt =
    "You are a context summarization assistant. Your task is to read a "
    "conversation between a user and an AI assistant, then produce a "
    "structured summary following the exact format specified.\n\n"
    "Do NOT continue the conversation. Do NOT respond to any questions in the "
    "conversation. ONLY output the structured summary.";

inline constexpr std::string_view kSummarizationPrompt =
    "The messages above are a conversation to summarize. Create a structured "
    "context checkpoint summary that another LLM will use to continue the "
    "work.\n\n"
    "Use this EXACT format:\n\n"
    "## Goal\n"
    "[What is the user trying to accomplish? Can be multiple items if the "
    "session covers different tasks.]\n\n"
    "## Constraints & Preferences\n"
    "- [Any constraints, preferences, or requirements mentioned by user]\n"
    "- [Or \"(none)\" if none were mentioned]\n\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Completed tasks/changes]\n\n"
    "### In Progress\n"
    "- [ ] [Current work]\n\n"
    "### Blocked\n"
    "- [Issues preventing progress, if any]\n\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale]\n\n"
    "## Next Steps\n"
    "1. [Ordered list of what should happen next]\n\n"
    "## Critical Context\n"
    "- [Any data, examples, or references needed to continue]\n"
    "- [Or \"(none)\" if not applicable]\n\n"
    "Keep each section concise. Preserve exact file paths, function names, "
    "and error messages.";

inline constexpr std::string_view kUpdateSummarizationPrompt =
    "The messages above are NEW conversation messages to incorporate into the "
    "existing summary provided in <previous-summary> tags.\n\n"
    "Update the existing structured summary with new information. RULES:\n"
    "- PRESERVE all existing information from the previous summary\n"
    "- ADD new progress, decisions, and context from the new messages\n"
    "- UPDATE the Progress section: move items from \"In Progress\" to "
    "\"Done\" when completed\n"
    "- UPDATE \"Next Steps\" based on what was accomplished\n"
    "- PRESERVE exact file paths, function names, and error messages\n"
    "- If something is no longer relevant, you may remove it\n\n"
    "Use this EXACT format:\n\n"
    "## Goal\n"
    "[Preserve existing goals, add new ones if the task expanded]\n\n"
    "## Constraints & Preferences\n"
    "- [Preserve existing, add new ones discovered]\n\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Include previously done items AND newly completed items]\n\n"
    "### In Progress\n"
    "- [ ] [Current work - update based on progress]\n\n"
    "### Blocked\n"
    "- [Current blockers - remove if resolved]\n\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale] (preserve all previous, add new)\n\n"
    "## Next Steps\n"
    "1. [Update based on current state]\n\n"
    "## Critical Context\n"
    "- [Preserve important context, add new if needed]\n\n"
    "Keep each section concise. Preserve exact file paths, function names, "
    "and error messages.";

inline constexpr std::string_view kTurnPrefixSummarizationPrompt =
    "This is the PREFIX of a turn that was too large to keep. The SUFFIX "
    "(recent work) is retained.\n\n"
    "Summarize the prefix to provide context for the retained suffix:\n\n"
    "## Original Request\n"
    "[What did the user ask for in this turn?]\n\n"
    "## Early Progress\n"
    "- [Key decisions and work done in the prefix]\n\n"
    "## Context for Suffix\n"
    "- [Information needed to understand the retained recent work]\n\n"
    "Be concise. Focus on what's needed to understand the kept suffix.";

} // namespace cch::harness::session
