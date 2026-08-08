#pragma once

#include "cli/CliConfig.hpp"
#include "coding_agent/SessionTarget.hpp"
#include <cch/util/Error.hpp>

#include <istream>
#include <ostream>
#include <string>

namespace cch::cli {

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
    bool aborted{false};
};

/// pi main.ts `createSessionManager`: assemble the pi session target from the
/// raw session-family flags. Resolves the session directory chain
/// (--session-dir, then PI_CODING_AGENT_SESSION_DIR, then the settings
/// sessionDir value supplied by the caller), resolves `--session`/`--fork`
/// arguments against the local and global session spaces, prints the
/// warn-create warning to `error`, and runs the cross-project fork prompt on
/// `input`/`output` for global `--session` matches. Errors carry pi's exact
/// text (e.g. "No session found matching '<arg>'") without a prefix.
[[nodiscard]] util::Expected<SessionFamilyAssembly> assemble_session_target(
    const CliConfig& config,
    const std::optional<std::string>& settings_session_dir,
    std::istream& input,
    std::ostream& output,
    std::ostream& error);

} // namespace cch::cli
