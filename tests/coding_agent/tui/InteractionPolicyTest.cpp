#include "coding_agent/tui/InteractionPolicy.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <string>
#include <variant>

using namespace cch::coding_agent::tui;

namespace {

[[nodiscard]] InteractionActivity idle_with_shell() {
    return InteractionActivity{
        .user_shell_available = true,
        .user_bash_active = false,
        .prompt_active = false,
        .interrupt_requested = false,
    };
}

} // namespace

TEST_CASE(
    "route_user_bash launches only direct focused-editor submissions",
    "[coding_agent][tui][interaction-policy]") {
    const auto activity = idle_with_shell();

    auto focused = route_user_bash("!ls", SubmissionOrigin::FocusedEditor, activity);
    REQUIRE(focused.has_value());
    const auto* launch = std::get_if<LaunchUserBash>(&*focused);
    REQUIRE(launch != nullptr);
    CHECK(launch->invocation.command == "ls");
    CHECK_FALSE(launch->invocation.exclude_from_context);

    // Positional initial input keeps ordinary Agent Prompt semantics
    // (ADR 0026).
    CHECK_FALSE(route_user_bash("!ls", SubmissionOrigin::InitialPrompt, activity)
                    .has_value());

    // Without the Session-owned User Shell the prefix is ordinary text.
    auto no_shell = idle_with_shell();
    no_shell.user_shell_available = false;
    CHECK_FALSE(route_user_bash("!ls", SubmissionOrigin::FocusedEditor, no_shell)
                    .has_value());

    // Non-bash text falls through regardless of activity.
    CHECK_FALSE(route_user_bash("hello", SubmissionOrigin::FocusedEditor, activity)
                    .has_value());
    CHECK_FALSE(route_user_bash("!", SubmissionOrigin::FocusedEditor, activity)
                    .has_value());
}

TEST_CASE(
    "route_user_bash restores a busy invocation as its safe recall",
    "[coding_agent][tui][interaction-policy]") {
    auto busy = idle_with_shell();
    busy.user_bash_active = true;

    auto route = route_user_bash("!!rm -rf ./tmp", SubmissionOrigin::FocusedEditor, busy);
    REQUIRE(route.has_value());
    const auto* restore = std::get_if<RestoreUserBashBusy>(&*route);
    REQUIRE(restore != nullptr);
    CHECK(restore->safe_recall == "!! rm -rf ./tmp");
}

TEST_CASE(
    "route_prompt admits queue input only while an uninterrupted run is active",
    "[coding_agent][tui][interaction-policy]") {
    auto activity = idle_with_shell();
    CHECK(route_prompt(InputSubmission::Ordinary, activity) == PromptRoute::StartPrompt);
    CHECK(route_prompt(InputSubmission::FollowUp, activity) == PromptRoute::StartPrompt);

    activity.prompt_active = true;
    CHECK(route_prompt(InputSubmission::Ordinary, activity) == PromptRoute::QueueSteering);
    CHECK(route_prompt(InputSubmission::FollowUp, activity) == PromptRoute::QueueFollowUp);

    // Once the active run was asked to abort, queued input would die with
    // it: the text returns to the editor instead.
    activity.interrupt_requested = true;
    CHECK(route_prompt(InputSubmission::Ordinary, activity) ==
        PromptRoute::RestoreInterrupted);
    CHECK(route_prompt(InputSubmission::FollowUp, activity) ==
        PromptRoute::RestoreInterrupted);
}

TEST_CASE(
    "route_interrupt targets the Agent run before an overlapping User Bash",
    "[coding_agent][tui][interaction-policy]") {
    auto activity = idle_with_shell();
    CHECK(route_interrupt(activity) == InterruptRoute::None);

    activity.user_bash_active = true;
    CHECK(route_interrupt(activity) == InterruptRoute::CancelUserBash);

    // pi editor Escape precedence: the Agent run first; the Bash becomes the
    // target of a later interrupt once the Agent is idle.
    activity.prompt_active = true;
    CHECK(route_interrupt(activity) == InterruptRoute::AbortAgentRun);

    activity.user_bash_active = false;
    CHECK(route_interrupt(activity) == InterruptRoute::AbortAgentRun);
}
