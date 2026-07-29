#pragma once

#include <cch/harness/ExecutionEnv.hpp>

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace cch::harness {

/// Resolve one Unix Shell executable without consulting the ambient process
/// working directory. Relative configured paths and relative PATH entries are
/// interpreted from the canonical Session workspace.
[[nodiscard]] std::expected<std::filesystem::path, ExecutionError> resolve_shell_executable(
    const std::optional<std::string>& configured_path,
    const std::filesystem::path& workspace,
    const std::map<std::string, std::string>& environment,
    const std::filesystem::path& system_bash_candidate = "/bin/bash");

} // namespace cch::harness
