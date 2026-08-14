#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

enum class SessionTopology {
    Linear,
    Branched,
    Compacted,
};

struct SessionResumeResult {
    SessionMetadata metadata;
    std::vector<ai::MessageVariant> history;
    /// Stored `model_change` provider (pi `{provider, modelId}`); re-resolved
    /// against the live runtime catalog on resume.
    std::optional<std::string> provider;
    /// Stored `model_change` model id.
    std::optional<std::string> model;
    /// Derived `thinkingLevel` from the active path (pi "off" default; the
    /// last `thinking_level_change` entry wins).
    std::string thinking_level{"off"};
    /// True when the active path carries a `thinking_level_change` entry (pi
    /// sdk.ts `hasThinkingEntry` gates resumed-level restoration against the
    /// settings `defaultThinkingLevel`).
    bool has_thinking_level_entry{false};
    SessionTopology topology{SessionTopology::Linear};
};

/// Reopen a persisted session into agent-ready context for the active path.
[[nodiscard]] support::Expected<SessionResumeResult> resume_session(const std::filesystem::path& path);

} // namespace cch::harness::session
