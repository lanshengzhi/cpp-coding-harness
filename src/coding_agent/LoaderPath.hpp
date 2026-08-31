#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// Converts an absolute path beneath workspace_root to the corresponding
/// workspace-relative path. Relative paths pass through unchanged and
/// absolute or traversal-bearing paths outside the workspace return no value.
/// This is a representation adapter only; the filesystem capability remains
/// the authoritative containment seam for every filesystem operation.
[[nodiscard]] inline std::optional<std::string> strip_workspace_root(
    const std::filesystem::path& workspace_root,
    std::string_view candidate_text) {
    const std::filesystem::path candidate{candidate_text};
    if (!candidate.is_absolute()) {
        return std::string{candidate_text};
    }

    for (const auto& component : candidate) {
        if (component == "..") {
            return std::nullopt;
        }
    }

    auto root_it = workspace_root.begin();
    auto candidate_it = candidate.begin();
    while (root_it != workspace_root.end() &&
           candidate_it != candidate.end() &&
           *root_it == *candidate_it) {
        ++root_it;
        ++candidate_it;
    }
    if (root_it != workspace_root.end()) {
        return std::nullopt;
    }

    std::filesystem::path relative;
    for (; candidate_it != candidate.end(); ++candidate_it) {
        relative /= *candidate_it;
    }
    return relative.string();
}

} // namespace cch::coding_agent
