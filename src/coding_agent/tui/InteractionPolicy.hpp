#pragma once

#include "coding_agent/tui/UserBashSyntax.hpp"

#include <optional>
#include <string>
#include <variant>

namespace cch::coding_agent::tui {

/// Native TUI interaction routing (pi editor semantics at baseline 864b35c):
/// one decision table for what a focused submission or interrupt does, given
/// the current activity. The owning frontend supplies the facts and performs
/// the routed effect; the precedence rules live here.

enum class InputSubmission { Ordinary, FollowUp };
enum class SubmissionOrigin { FocusedEditor, InitialPrompt };

/// Live activity facts one routing decision depends on.
struct InteractionActivity {
    /// Session assembly provided the Session-owned User Shell.
    bool user_shell_available{false};
    bool user_bash_active{false};
    bool prompt_active{false};
    /// An interrupt was already requested for the active prompt generation.
    bool interrupt_requested{false};
};

struct LaunchUserBash {
    UserBashInvocation invocation;
};
/// One User Bash runs at a time: the rejected invocation returns to the
/// editor as its safe redacted recall text.
struct RestoreUserBashBusy {
    std::string safe_recall;
};
using UserBashRoute = std::variant<LaunchUserBash, RestoreUserBashBusy>;

/// First submission stage. Only a direct focused-editor submission may
/// become User Bash (ADR 0026); everything else falls through (nullopt) to
/// slash-command dispatch and ordinary prompt interpretation.
[[nodiscard]] std::optional<UserBashRoute> route_user_bash(
    const std::string& text,
    SubmissionOrigin origin,
    const InteractionActivity& activity);

/// Final submission stage, after slash-command dispatch declined the text.
enum class PromptRoute {
    /// The active prompt was already asked to abort; input queued behind a
    /// dying run would be lost, so the text returns to the editor instead.
    RestoreInterrupted,
    QueueSteering,
    QueueFollowUp,
    StartPrompt,
};
[[nodiscard]] PromptRoute route_prompt(
    InputSubmission submission,
    const InteractionActivity& activity);

/// Interrupt precedence (pi editor Escape): an active Agent run is the
/// target before an overlapping User Bash, which becomes the target of a
/// later interrupt once the Agent is idle. Idle interrupts do nothing.
enum class InterruptRoute { AbortAgentRun, CancelUserBash, None };
[[nodiscard]] InterruptRoute route_interrupt(const InteractionActivity& activity);

} // namespace cch::coding_agent::tui
