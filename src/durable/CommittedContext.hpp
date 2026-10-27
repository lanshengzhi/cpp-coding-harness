#pragma once

#include <cch/chord/Json.hpp>
#include <cch/durable/Records.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace cch::durable {

/// The committed active context of one conversation (frozen pi `ContextView`
/// over durable records): the newest head marker within the read bounds, the
/// entries of its kept range oldest first, and their contributed model
/// messages with the excluded stop reasons filtered out. The C++ slice has
/// no per-target entry edits, so contributions are the entries' own model
/// messages (frozen pi `contribute` without `ContextEdit`).
struct CommittedContext {
    /// The newest entry carrying a head within the bounds, if any (frozen pi
    /// `ContextView.head`).
    std::optional<EntryRecord> head{};
    /// The head marker followed by the kept range's non-head entries, oldest
    /// first (frozen pi `ContextView.entries`).
    std::vector<EntryRecord> entries{};
    /// The contributed model messages of every entry in `entries` (frozen pi
    /// `ContextView.contributions`).
    std::vector<std::vector<chord::Json>> contributions{};
    /// The flat contributions with tool results ordered after their calls
    /// (frozen pi `ContextView.messages`, `orderToolResults`).
    std::vector<chord::Json> messages{};
    /// The newest entry id within the bounds (frozen pi `ContextBounds.tail`).
    std::optional<EntryId> tail{};

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
};

/// Derive the committed context of one conversation within bounds: the tail
/// is the newest entry at or before `at` (absent: the newest entry), and the
/// range runs from the newest head marker's head (or the transcript start)
/// through the tail (frozen pi `captureContextBounds` + `deriveContext`).
[[nodiscard]] support::Expected<CommittedContext> derive_committed_context(
        Storage& storage, ConversationId conversation_id, std::optional<EntryId> at = std::nullopt);

/// Estimated tokens of one model message: characters/3.5 per text, thinking,
/// tool call name and JSON arguments, a fixed charge per image (frozen pi
/// `estimateMessageTokens`).
[[nodiscard]] std::int64_t estimate_message_tokens(const chord::Json& message);

/// Size of a request over `context` followed by `extra` messages: the usage
/// of the newest assistant appended after the head marker, whose request
/// included the marker, plus estimates of the messages after it; without
/// one, estimates of every message (frozen pi `estimateContext`).
[[nodiscard]] std::int64_t estimate_context_tokens(
        const CommittedContext& context, const std::vector<chord::Json>& extra = {});

/// Index in `context.entries` of the first entry a summary keeps, or absent
/// when there is nothing to compact (frozen pi `selectCut`, spec §8.7).
[[nodiscard]] std::optional<std::size_t> select_cut(const CommittedContext& context, std::int64_t keep_recent_tokens);

/// The messages a summary summarizes: the entries before `cut`, ordered like
/// model context (frozen pi `summarizedMessages`).
[[nodiscard]] std::vector<chord::Json> summarized_messages(const CommittedContext& context, std::size_t cut);

/// Messages as plain text for the summarizer prompt: system and thinking
/// omitted, tool calls and results recorded, tool result text truncated
/// (frozen pi `serializeConversation`).
[[nodiscard]] std::string serialize_conversation(const std::vector<chord::Json>& messages);

} // namespace cch::durable
