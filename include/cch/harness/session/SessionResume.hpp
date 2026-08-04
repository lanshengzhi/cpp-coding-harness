#pragma once

#include "SessionEntry.hpp"

#include "../../ai/Message.hpp"
#include "../../util/Error.hpp"

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
    std::optional<std::string> thinking_level;
    SessionTopology topology{SessionTopology::Linear};
};

/// Reopen a persisted session into agent-ready context for the active path.
[[nodiscard]] util::Expected<SessionResumeResult> resume_session(const std::filesystem::path& path);

} // namespace cch::harness::session
