#include "coding_agent/tui/InteractionPolicy.hpp"

#include <utility>

namespace cch::coding_agent::tui {

std::optional<UserBashRoute> route_user_bash(
    const std::string& text,
    SubmissionOrigin origin,
    const InteractionActivity& activity) {
    if (origin != SubmissionOrigin::FocusedEditor) return std::nullopt;
    if (!activity.user_shell_available) return std::nullopt;
    auto invocation = parse_user_bash_invocation(text);
    if (!invocation) return std::nullopt;
    if (activity.user_bash_active) {
        return UserBashRoute{RestoreUserBashBusy{
            .safe_recall = safe_user_bash_invocation(*invocation),
        }};
    }
    return UserBashRoute{LaunchUserBash{.invocation = std::move(*invocation)}};
}

PromptRoute route_prompt(
    InputSubmission submission,
    const InteractionActivity& activity) {
    if (!activity.prompt_active) return PromptRoute::StartPrompt;
    if (activity.interrupt_requested) return PromptRoute::RestoreInterrupted;
    return submission == InputSubmission::FollowUp
        ? PromptRoute::QueueFollowUp
        : PromptRoute::QueueSteering;
}

} // namespace cch::coding_agent::tui
