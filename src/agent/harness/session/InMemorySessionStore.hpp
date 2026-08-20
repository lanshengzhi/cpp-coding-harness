#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

/// The in-memory closed alternative behind the SessionStore facade: a
/// no-persistence store for an in-memory Agent Session. Appends build the
/// same SessionEntry values the JSONL alternative persists — without
/// disk I/O and without append-time redaction, which is persistence policy
/// — so the owning SessionStore can mirror them into its live SessionTree
/// exactly like a persisted session (pi's non-persisting SessionManager
/// keeps the same in-memory entries). Live Session State remains owned
/// exclusively by AgentSessionRuntime.
class InMemorySessionStore final {
public:
    /// pi `appendMessage`: the message hangs under the current leaf (the
    /// caller — SessionStore under its lock — resolves the live tree's
    /// leaf, nullopt at the root position).
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append(
        const ai::MessageVariant& message,
        std::optional<std::string> parent_id);

    // --- v3 tree entry append methods ---
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_model_change(
        std::optional<std::string> parent_id,
        std::string provider,
        std::string model_id);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_thinking_level_change(
        std::optional<std::string> parent_id,
        std::string thinking_level);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_label_change(
        std::optional<std::string> parent_id,
        std::string target_id,
        std::optional<std::string> label);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_compaction(
        std::optional<std::string> parent_id,
        CompactionEntryValue value);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_branch_summary(
        std::optional<std::string> parent_id,
        std::string from_id,
        std::string summary,
        std::optional<support::JsonValue> details,
        std::optional<bool> from_hook);
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_session_info(
        std::optional<std::string> parent_id,
        std::string name);
    /// Mirror a Leaf marker into the live tree state (pi `setLeafId`;
    /// nullopt target is the root position).
    [[nodiscard]] support::Expected<std::vector<SessionEntry>> append_leaf(
        std::optional<std::string> parent_id,
        std::optional<std::string> target_id);

    [[nodiscard]] std::optional<std::filesystem::path> path() const;
};

} // namespace cch::harness::session
