#pragma once

#include "cli/CliConfig.hpp"
#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include <cch/support/Error.hpp>

#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <ostream>
#include <string>

namespace cch::cli {

/// pi `session-picker.ts` `selectSession` host: the startup-TUI picker over
/// the effective session space. `current_loader`/`all_loader` are pi
/// `SessionManager.list`/`listAll` closures (pi main.ts passes them to
/// `selectSession`); the host returns the picked session path, or nullopt
/// when the user cancelled or exited. The CLI installs the ProcessTerminal
/// host; the in-process CLI test seam injects scripted pickers.
using ResumePickerSink = std::move_only_function<support::Expected<
    std::optional<std::filesystem::path>>(
    coding_agent::tui::SessionListLoader,
    coding_agent::tui::SessionListLoader)>;

/// pi main.ts boot checks, in pi's order: `--fork` conflicts
/// (--session/--continue/--resume/--no-session), then `--session-id`
/// conflicts (--session/--continue/--resume), then the session-id format
/// (pi `assertValidSessionId`). Returns pi's exact error text including the
/// "Error: " prefix, or nullopt when the flags pass. The `--name` guard runs
/// after session selection (pi order) and lives in session_name_guard_error.
[[nodiscard]] std::optional<std::string> session_family_guard_error(
    const CliConfig& config);

/// pi main.ts `--name` guard: the trimmed value must be non-empty. Returns
/// pi's exact error text including the "Error: " prefix, or nullopt.
[[nodiscard]] std::optional<std::string> session_name_guard_error(
    const CliConfig& config);

/// Outcome of pi main.ts `createSessionManager` target assembly.
struct SessionFamilyAssembly {
    /// The assembled session target. Engaged unless a cross-project fork was
    /// declined, in which case pi prints "Aborted." and exits 0.
    coding_agent::SessionTarget target;
    /// True when a global `--session` match was declined at the fork prompt:
    /// the caller prints nothing further and exits 0 (the assembly already
    /// wrote the notice, prompt, and "Aborted." lines to the output stream).
    /// The cancelled `--resume` picker also sets this after printing pi's
    /// "No session selected" line.
    bool aborted{false};
    /// pi `SessionManager.getSessionFile()`: the resume-shaped target's
    /// session file (an existing non-empty file for open-or-create, the
    /// most recent session for `--continue`), or nullopt for fresh,
    /// in-memory, and fork targets. Feeds the boot missing-cwd check
    /// (pi main.ts `getMissingSessionCwdIssue`).
    std::optional<std::filesystem::path> session_file;
};

/// pi main.ts `createSessionManager`: assemble the pi session target from the
/// raw session-family flags. Resolves the session directory chain
/// (--session-dir, then PI_CODING_AGENT_SESSION_DIR, then the settings
/// sessionDir value supplied by the caller), resolves `--session`/`--fork`
/// arguments against the local and global session spaces, prints the
/// warn-create warning to `error`, and runs the cross-project fork prompt on
/// `input`/`output` for global `--session` matches. `--resume` opens the
/// startup-TUI session picker through `resume_picker` (pi `selectSession`);
/// a cancelled picker prints "No session selected" to `output` and aborts
/// with exit 0. Errors carry pi's exact text (e.g. "No session found
/// matching '<arg>'") without a prefix.
[[nodiscard]] support::Expected<SessionFamilyAssembly> assemble_session_target(
    const CliConfig& config,
    const std::optional<std::string>& settings_session_dir,
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    ResumePickerSink resume_picker = {});

/// pi main.ts `getMissingSessionCwdIssue` over the assembled target: the
/// resume-shaped target's stored header cwd when it differs from the launch
/// cwd and no longer exists (SessionFactory applies the same condition at
/// session creation). The interactive host prompts Continue/Cancel before
/// the main TUI boots; the non-interactive host surfaces the stderr error.
[[nodiscard]] std::optional<coding_agent::MissingSessionCwdIssue>
missing_session_cwd_issue(
    const SessionFamilyAssembly& assembly,
    const std::filesystem::path& launch_cwd);

} // namespace cch::cli
