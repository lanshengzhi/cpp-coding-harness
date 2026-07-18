#pragma once

#include <filesystem>

namespace cch::coding_agent {

/// User-level state root for the harness, mirroring pi's agent config directory
/// (`~/.pi/agent/`). All user-level state files (auth, settings, trust) live
/// under this single root, keeping them clearly separated from project-level
/// `.cpp-harness/` resources.
///
/// Resolution order:
///   1. `CCH_CODING_AGENT_DIR` environment override (pi: `PI_CODING_AGENT_DIR`),
///   2. `$HOME/.cpp-harness/agent` (`%USERPROFILE%\.cpp-harness\agent` on Windows).
///
/// Returns an empty path when no home directory can be determined; derived
/// file paths are then empty as well, and loaders treat them as missing.
[[nodiscard]] std::filesystem::path agent_config_dir();

/// `agent_config_dir()/auth.json` — user authentication entries (pi: `auth.json`).
[[nodiscard]] std::filesystem::path auth_file_path();

/// `agent_config_dir()/settings.json` — user settings (pi: `settings.json`).
[[nodiscard]] std::filesystem::path settings_file_path();

/// `agent_config_dir()/trust.json` — persisted project trust decisions
/// (pi: `ProjectTrustStore` rooted at the agent config directory).
[[nodiscard]] std::filesystem::path trust_store_file_path();

} // namespace cch::coding_agent
