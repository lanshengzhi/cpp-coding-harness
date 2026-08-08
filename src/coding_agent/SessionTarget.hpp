#pragma once

#include <filesystem>
#include <variant>

namespace cch::coding_agent {

/// Create a persisted Agent Session beneath the workspace-keyed Agent Config
/// Directory sessions root.
struct DefaultPersistedSessionTarget {};

/// Create a new persisted Agent Session at an exact caller-supplied path.
struct ExplicitNewSessionTarget {
    std::filesystem::path path;
};

/// Resume the persisted Agent Session at an exact caller-supplied path.
struct ExplicitResumeSessionTarget {
    std::filesystem::path path;
};

/// Create an Agent Session without a session directory or transcript file.
struct InMemorySessionTarget {};

/// Mutually exclusive session target. Default construction selects
/// workspace-keyed persistence under the Agent Config Directory.
using SessionTarget = std::variant<
    DefaultPersistedSessionTarget,
    ExplicitNewSessionTarget,
    ExplicitResumeSessionTarget,
    InMemorySessionTarget>;

} // namespace cch::coding_agent
