#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// Converts an absolute path beneath workspace_root to the corresponding
/// workspace-relative path. Both sides are lexically normalized first — the
/// same contract the filesystem capability's uniform pi `resolveToCwd`
/// resolution enforces (ADR 0057) — so `..` and `.` components resolve by
/// normalization rather than rejection. Relative paths pass through
/// unchanged; absolute paths that normalize outside the workspace root
/// return no value. This is a representation adapter only; addressing and
/// resolution live in the filesystem capability's uniform pi `resolveToCwd`
/// operation (ADR 0057).
[[nodiscard]] inline std::optional<std::string> strip_workspace_root(
    const std::filesystem::path& workspace_root,
    std::string_view candidate_text) {
    const std::filesystem::path candidate{candidate_text};
    if (!candidate.is_absolute()) {
        return std::string{candidate_text};
    }

    // Mirror `resolveToCwd`: lexical normalization resolves `..` and `.`,
    // then a non-root trailing empty filename component is dropped.
    const auto normalize = [](std::filesystem::path path) {
        path = path.lexically_normal();
        if (path != path.root_path() && path.filename().empty()) {
            path = path.parent_path();
        }
        return path;
    };
    const auto root = normalize(workspace_root);
    const auto normalized = normalize(candidate);

    auto root_it = root.begin();
    auto candidate_it = normalized.begin();
    while (root_it != root.end() && candidate_it != normalized.end() && *root_it == *candidate_it) {
        ++root_it;
        ++candidate_it;
    }
    if (root_it != root.end()) {
        return std::nullopt;
    }

    std::filesystem::path relative;
    for (; candidate_it != normalized.end(); ++candidate_it) {
        relative /= *candidate_it;
    }
    return relative.string();
}

} // namespace cch::coding_agent
