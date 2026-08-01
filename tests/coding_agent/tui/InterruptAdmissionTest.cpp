#include "coding_agent/tui/InterruptAdmission.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <array>

using cch::coding_agent::tui::InteractionActivity;
using cch::coding_agent::tui::InterruptAdmission;
using cch::coding_agent::tui::InterruptRoute;

namespace {

struct RouteCase {
    bool prompt_active{false};
    bool user_bash_active{false};
    bool pending_bash{false};
    InterruptRoute expected{InterruptRoute::None};
};

constexpr std::array<RouteCase, 8> kRouteCases{{
    {.prompt_active = false, .user_bash_active = false, .pending_bash = false,
     .expected = InterruptRoute::None},
    {.prompt_active = false, .user_bash_active = false, .pending_bash = true,
     .expected = InterruptRoute::ClearPendingBash},
    {.prompt_active = false, .user_bash_active = true, .pending_bash = false,
     .expected = InterruptRoute::CancelUserBash},
    {.prompt_active = false, .user_bash_active = true, .pending_bash = true,
     .expected = InterruptRoute::CancelUserBash},
    {.prompt_active = true, .user_bash_active = false, .pending_bash = false,
     .expected = InterruptRoute::AbortAgentRun},
    {.prompt_active = true, .user_bash_active = false, .pending_bash = true,
     .expected = InterruptRoute::AbortAgentRun},
    {.prompt_active = true, .user_bash_active = true, .pending_bash = false,
     .expected = InterruptRoute::AbortAgentRun},
    {.prompt_active = true, .user_bash_active = true, .pending_bash = true,
     .expected = InterruptRoute::AbortAgentRun},
}};

[[nodiscard]] InteractionActivity activity(const RouteCase& test) {
    return InteractionActivity{
        .user_shell_available = true,
        .user_bash_active = test.user_bash_active,
        .prompt_active = test.prompt_active,
        .interrupt_requested = false,
    };
}

} // namespace

TEST_CASE(
    "Interrupt Admission routes the complete current-activity table by pi precedence",
    "[coding_agent][tui][interrupt-admission][issue92][issue93]") {
    for (const auto& test : kRouteCases) {
        InterruptAdmission admission;
        CHECK(admission.admit(
            activity(test),
            admission.generation(),
            test.pending_bash) == test.expected);
    }
}

TEST_CASE(
    "Interrupt Admission rejects every stale activity route",
    "[coding_agent][tui][interrupt-admission][issue92]") {
    for (const auto& test : kRouteCases) {
        InterruptAdmission admission;
        const auto stale_generation = admission.generation();
        admission.note_prompt_started();

        CHECK(admission.admit(
            activity(test),
            stale_generation,
            test.pending_bash) == InterruptRoute::None);
        CHECK_FALSE(admission.interrupt_requested());
    }
}

TEST_CASE(
    "Interrupt Admission rejects a request after its Agent prompt finishes",
    "[coding_agent][tui][interrupt-admission][issue92]") {
    InterruptAdmission admission;
    admission.note_prompt_started();
    const auto active_generation = admission.generation();
    admission.note_prompt_finished();

    CHECK(admission.admit(
        activity(kRouteCases[2]),
        active_generation,
        false) == InterruptRoute::None);
    CHECK_FALSE(admission.interrupt_requested());
}

TEST_CASE(
    "Interrupt Admission coalesces one Agent generation without hiding later targets",
    "[coding_agent][tui][interrupt-admission][issue92]") {
    InterruptAdmission admission;
    const auto first_generation = admission.generation();

    CHECK_FALSE(admission.interrupt_requested());
    CHECK(admission.admit(
        activity(kRouteCases[7]),
        first_generation,
        true) == InterruptRoute::AbortAgentRun);
    CHECK(admission.interrupt_requested());
    CHECK(admission.admit(
        activity(kRouteCases[7]),
        first_generation,
        true) == InterruptRoute::None);

    // The overlapping Bash and pending editor remain interruptible after the
    // Agent becomes idle, without weakening repeated-Agent coalescing.
    CHECK(admission.admit(
        activity(kRouteCases[2]),
        first_generation,
        false) == InterruptRoute::CancelUserBash);
    CHECK(admission.admit(
        activity(kRouteCases[1]),
        first_generation,
        true) == InterruptRoute::ClearPendingBash);

    admission.note_prompt_started();
    CHECK(admission.generation() == first_generation + 1);
    CHECK_FALSE(admission.interrupt_requested());
    CHECK(admission.admit(
        activity(kRouteCases[4]),
        admission.generation(),
        false) == InterruptRoute::AbortAgentRun);
    CHECK(admission.interrupt_requested());

    admission.note_prompt_finished();
    CHECK(admission.generation() == first_generation + 2);
    CHECK_FALSE(admission.interrupt_requested());
}
