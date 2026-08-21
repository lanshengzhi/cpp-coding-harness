#pragma once

// Native TUI process suspension (pi `handleCtrlZ` + the SIGCONT resume
// handler): stop the TUI so the terminal restores, ignore SIGINT while
// suspended, stop the process group with SIGTSTP, and on SIGCONT restart
// the TUI with a forced render. Extraction #506 from the pre-split
// monolith; the host wires the terminal lifecycle and failure reporting
// through SuspendHooks.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/signal_set.hpp>

#include <functional>
#include <memory>

namespace cch::coding_agent::tui {

/// Host operations used by SuspendController. The controller owns the signal
/// registration and suspend/resume sequencing; the host owns the terminal
/// lifecycle and the run's completion/error path. Hooks capture the host
/// weakly; a null hook fails closed (the step no-ops or reports generic).
struct SuspendHooks {
    /// Stop the TUI (restore the terminal) before the process group stops.
    std::move_only_function<support::ExpectedVoid()> stop_tui{nullptr};
    /// Restart the TUI after SIGCONT.
    std::move_only_function<support::ExpectedVoid()> start_tui{nullptr};
    /// Force one full render after the restart (pi `requestRender(true)`).
    std::move_only_function<support::ExpectedVoid()> render_tui{nullptr};
    /// Report a terminal lifecycle failure: the host records the run's
    /// completion error and requests exit.
    std::move_only_function<void(support::Error)> report_failure{nullptr};
    /// Send SIGTSTP to the process group (pi `process.kill(0, "SIGTSTP")`
    /// through the host's closed action seam).
    std::move_only_function<void()> suspend_process{nullptr};
    /// Whether the host run is still live when SIGCONT arrives.
    std::move_only_function<bool()> is_running{nullptr};
};

/// pi `handleCtrlZ`: Ctrl+Z suspends the process group after the terminal
/// restores; SIGCONT resumes the TUI and forces a re-render (pi's
/// `ui.start()` + `requestRender(true)`). Executor-confined like the host.
class SuspendController final : public std::enable_shared_from_this<SuspendController> {
public:
    SuspendController(
        boost::asio::any_io_executor executor,
        SuspendHooks hooks);
    SuspendController(SuspendController&&) = delete;
    SuspendController& operator=(SuspendController&&) = delete;
    ~SuspendController() = default;
    SuspendController(const SuspendController&) = delete;
    SuspendController& operator=(const SuspendController&) = delete;

    /// pi `handleCtrlZ`: a no-op while a suspend is already in flight.
    void suspend();

    /// Whether the suspend signal handlers are registered (a suspend is in
    /// flight and SIGCONT has not arrived).
    [[nodiscard]] bool active() const noexcept { return signals_ != nullptr; }

private:
    /// pi's SIGCONT handler body: restore the TUI and request a full render.
    void resume_after_suspend();

    boost::asio::any_io_executor executor_;
    SuspendHooks hooks_;
    /// SIGCONT/SIGINT registration while suspended (pi's suspend signal
    /// handlers); reset restores the previous handlers on resume.
    std::shared_ptr<boost::asio::signal_set> signals_;
};

} // namespace cch::coding_agent::tui
