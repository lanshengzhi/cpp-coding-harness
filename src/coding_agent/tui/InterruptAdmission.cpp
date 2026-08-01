#include "InterruptAdmission.hpp"

namespace cch::coding_agent::tui {

std::size_t InterruptAdmission::generation() const noexcept {
    return prompt_generation_.load();
}

void InterruptAdmission::note_prompt_started() noexcept {
    (void)prompt_generation_.fetch_add(1);
}

void InterruptAdmission::note_prompt_finished() noexcept {
    (void)prompt_generation_.fetch_add(1);
}

bool InterruptAdmission::interrupt_requested() const noexcept {
    return interrupt_requested_generation_ == prompt_generation_.load();
}

InterruptRoute InterruptAdmission::admit(
    const InteractionActivity& activity,
    std::size_t captured_generation,
    bool pending_bash) noexcept {
    if (captured_generation != prompt_generation_.load()) return InterruptRoute::None;
    if (activity.prompt_active) {
        if (interrupt_requested_generation_ == captured_generation) {
            return InterruptRoute::None;
        }
        interrupt_requested_generation_ = captured_generation;
        return InterruptRoute::AbortAgentRun;
    }
    if (activity.user_bash_active) return InterruptRoute::CancelUserBash;
    if (pending_bash) return InterruptRoute::ClearPendingBash;
    return InterruptRoute::None;
}

} // namespace cch::coding_agent::tui
