#pragma once

#include <filesystem>

namespace cch::tests {

/// The Agent Config Directory the product resolves under `xdg_config_home`:
/// the fixed `$XDG_CONFIG_HOME/pike/agent` (ADR 0058).
[[nodiscard]] inline std::filesystem::path agent_root_under_xdg(const std::filesystem::path& xdg_config_home) {
    return xdg_config_home / "pike" / "agent";
}

/// The Agent Config Directory the product resolves under `home`: the fixed
/// `$HOME/.config/pike/agent` (ADR 0058).
[[nodiscard]] inline std::filesystem::path agent_root_under_home(const std::filesystem::path& home) {
    return home / ".config" / "pike" / "agent";
}

/// The HOME whose default Agent Config Directory is exactly `agent_root`, which
/// must be shaped `<home>/.config/pike/agent`. Tests that name a root keep
/// naming it; only the environment slice changes.
[[nodiscard]] inline std::filesystem::path home_for_agent_root(const std::filesystem::path& agent_root) {
    return agent_root.parent_path().parent_path().parent_path();
}

} // namespace cch::tests
