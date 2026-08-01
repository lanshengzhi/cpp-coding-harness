#pragma once

#include "coding_agent/tui/InteractionPolicy.hpp"

#include <atomic>
#include <cstddef>
#include <optional>

namespace cch::coding_agent::tui {

/// Routed effect for one admitted interrupt request.
enum class InterruptRoute {
    AbortAgentRun,
    CancelUserBash,
    ClearPendingBash,
    None,
};

/// Owns Native TUI interrupt precedence, prompt-generation staleness, and
/// repeated Agent-interrupt coalescing. The owning frontend supplies activity
/// facts and performs the returned effect. generation() is input-thread safe;
/// every other operation is confined to the owning frontend's executor.
class InterruptAdmission final {
public:
    /// Captures the prompt generation when an input-thread request is posted.
    [[nodiscard]] std::size_t generation() const noexcept;

    /// Advances admission state immediately before a new Agent prompt starts.
    void note_prompt_started() noexcept;

    /// Invalidates requests captured before the active Agent prompt finished.
    void note_prompt_finished() noexcept;

    /// Reports whether the active prompt generation already admitted an abort.
    [[nodiscard]] bool interrupt_requested() const noexcept;

    /// Admits a current request and selects its pi-ordered target.
    [[nodiscard]] InterruptRoute admit(
        const InteractionActivity& activity,
        std::size_t captured_generation,
        bool pending_bash) noexcept;

private:
    std::atomic<std::size_t> prompt_generation_{0};
    std::optional<std::size_t> interrupt_requested_generation_;
};

} // namespace cch::coding_agent::tui
