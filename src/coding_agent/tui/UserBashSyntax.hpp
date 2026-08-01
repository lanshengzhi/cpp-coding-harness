#pragma once

#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// Focused-editor User Bash syntax (ADR 0026, pi baseline 864b35c): only a
/// direct focused Native TUI editor submission interprets the `!`/`!!`
/// prefixes. Positional initial input, one-shot, JSON, RPC, SDK, and Skill or
/// Prompt Template expansions keep ordinary Agent Prompt semantics, so this
/// module is a pure value vocabulary with no session or terminal dependency.

struct UserBashInvocation {
    std::string command;
    bool exclude_from_context{false};
};

/// Trims ASCII whitespace from both ends of one editor submission.
[[nodiscard]] std::string trim_editor_submission(std::string text);

/// Parses one trimmed submission as User Bash. `!` runs with later model
/// context; `!!` runs excluded from model conversion; `!!!foo` is excluded
/// User Bash running `!foo`. A bare `!` or `!!` yields no invocation and
/// falls through to an ordinary Agent Prompt.
[[nodiscard]] std::optional<UserBashInvocation> parse_user_bash_invocation(
    std::string text);

/// Rebuilds the redacted, control-stripped `!`/`!!` text used to restore a
/// rejected invocation to the editor and to name the execution in
/// diagnostics.
[[nodiscard]] std::string safe_user_bash_invocation(
    const UserBashInvocation& invocation);

/// Bash mode is the unsubmitted editor state whose trimmed text begins with
/// `!`; it exists only where User Bash dispatch is available.
[[nodiscard]] bool user_bash_editor_mode(
    std::string text,
    bool user_bash_available);

} // namespace cch::coding_agent::tui
