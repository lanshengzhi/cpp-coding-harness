#pragma once

#include <filesystem>

namespace cch::coding_agent {

/// Product-owned user-level state root (`~/.pike/agent/`). All user-level
/// state files (auth, settings, trust) live under this single root, separate
/// from project-level `.pi/` resources.
///
/// Resolution order:
///   1. `PIKE_CODING_AGENT_DIR` environment override,
///   2. `$HOME/.pike/agent`.
///
/// The former pi environment variable and directory are never consulted. Use
/// the explicit `pike import` command to copy state from a pi installation.
///
/// Returns an empty path when no home directory can be determined; derived
/// file paths are then empty as well, and loaders treat them as missing.
[[nodiscard]] std::filesystem::path agent_config_dir();

/// Home directory backing the user-level root (pi: `os.homedir()`), also used
/// to expand a leading home marker in CLI session-directory overrides.
/// Returns an empty path when no home can be determined.
[[nodiscard]] std::filesystem::path home_directory();

/// `agent_config_dir()/sessions` — default persisted Agent Session histories.
/// Returns an empty path when no user-level root can be resolved and never
/// creates the directory.
[[nodiscard]] std::filesystem::path sessions_root_path();

/// `agent_config_dir()/themes` — global Native TUI theme resources.
/// Returns an empty path when no user-level root can be resolved and never
/// creates or scans any pi state directory.
[[nodiscard]] std::filesystem::path themes_root_path();

/// `agent_config_dir()/auth.json` — user authentication entries (pi: `auth.json`).
[[nodiscard]] std::filesystem::path auth_file_path();

/// `agent_config_dir()/models.json` — user model configuration (pi: `models.json`).
/// Never written by the runtime; `ModelRuntime` reads and composes it.
[[nodiscard]] std::filesystem::path models_file_path();

/// `agent_config_dir()/settings.json` — user settings (pi: `settings.json`).
[[nodiscard]] std::filesystem::path settings_file_path();

/// `agent_config_dir()/trust.json` — persisted project trust decisions
/// (pi: `ProjectTrustStore` rooted at the agent config directory).
[[nodiscard]] std::filesystem::path trust_store_file_path();

} // namespace cch::coding_agent
