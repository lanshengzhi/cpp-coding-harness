#pragma once

// pi `core/session-cwd.ts` subset: the boot missing-cwd recovery facts and
// the two verbatim text shapes — the startup-TUI Continue/Cancel prompt
// (`formatMissingSessionCwdPrompt`) and the non-interactive stderr error
// (`MissingSessionCwdError.message`). The issue detection mirrors pi
// `getMissingSessionCwdIssue` with the C++ assembly nuance: an empty header
// cwd keeps the launch cwd, so only a stored cwd that differs from the
// launch cwd can be missing (SessionFactory `run_assembly` applies the same
// condition).

#include <filesystem>
#include <format>
#include <optional>
#include <string>

namespace cch::coding_agent {

/// pi `SessionCwdIssue` (session-cwd.ts): the resumed session file, its
/// stored header cwd, and the launch (fallback) cwd.
struct MissingSessionCwdIssue {
    std::filesystem::path session_file;
    std::filesystem::path session_cwd;
    std::filesystem::path fallback_cwd;
};

/// pi `getMissingSessionCwdIssue` subset: an issue when the stored header
/// cwd is non-empty, differs from the launch cwd, and no longer exists.
[[nodiscard]] inline std::optional<MissingSessionCwdIssue>
get_missing_session_cwd_issue(
    const std::filesystem::path& session_file,
    const std::filesystem::path& header_cwd,
    const std::filesystem::path& fallback_cwd) {
    if (session_file.empty() || header_cwd.empty() ||
        header_cwd == fallback_cwd) {
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::exists(header_cwd, ec)) {
        return std::nullopt;
    }
    return MissingSessionCwdIssue{
        .session_file = session_file,
        .session_cwd = header_cwd,
        .fallback_cwd = fallback_cwd,
    };
}

/// pi `formatMissingSessionCwdPrompt`, verbatim: the startup-TUI
/// Continue/Cancel prompt title (session-cwd.ts).
[[nodiscard]] inline std::string format_missing_session_cwd_prompt(
    const MissingSessionCwdIssue& issue) {
    return std::format(
        "cwd from session file does not exist\n{}\n\n"
        "continue in current cwd\n{}",
        issue.session_cwd.string(),
        issue.fallback_cwd.string());
}

/// pi `formatMissingSessionCwdError`, verbatim: the non-interactive stderr
/// error (session-cwd.ts `MissingSessionCwdError.message`).
[[nodiscard]] inline std::string format_missing_session_cwd_error(
    const MissingSessionCwdIssue& issue) {
    if (issue.session_file.empty()) {
        return std::format(
            "Stored session working directory does not exist: {}\n"
            "Current working directory: {}",
            issue.session_cwd.string(),
            issue.fallback_cwd.string());
    }
    return std::format(
        "Stored session working directory does not exist: {}\n"
        "Session file: {}\n"
        "Current working directory: {}",
        issue.session_cwd.string(),
        issue.session_file.string(),
        issue.fallback_cwd.string());
}

} // namespace cch::coding_agent
