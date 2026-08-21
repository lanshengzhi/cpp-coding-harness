#include "SuspendController.hpp"

#include "coding_agent/tui/ErrorPresentation.hpp"

#include <csignal>
#include <utility>

namespace cch::coding_agent::tui {

SuspendController::SuspendController(
    boost::asio::any_io_executor executor,
    SuspendHooks hooks)
    : executor_(std::move(executor)), hooks_(std::move(hooks)) {}

void SuspendController::suspend() {
    if (signals_) return;
    // pi `handleCtrlZ`: stop the TUI first so the terminal is restored
    // before the process group stops; the exit-wait timer keeps the
    // io_context alive while suspended (pi's keep-alive interval).
    if (const auto stopped = hooks_.stop_tui ? hooks_.stop_tui() : support::ExpectedVoid{};
        !stopped) {
        if (hooks_.report_failure) {
            hooks_.report_failure(presentation_error(
                stopped.error(),
                "Native TUI suspension failed"));
        }
        return;
    }

    // Ignore SIGINT while suspended so Ctrl+C in the terminal does not
    // kill the backgrounded process; the handler is removed on resume
    // (pi's `process.on("SIGINT", ignoreSigint)`). SIGCONT restores the
    // TUI and re-renders (pi's `process.once("SIGCONT", ...)`). The
    // shared wait re-arms after a swallowed SIGINT so the resume
    // handler stays registered.
    auto signals = std::make_shared<boost::asio::signal_set>(executor_, SIGCONT);
    signals->add(SIGINT);
    const auto weak = weak_from_this();
    auto arm = std::make_shared<std::move_only_function<void()>>();
    *arm = [weak, signals, arm] {
        signals->async_wait([weak, signals, arm](const boost::system::error_code& error, int fired) {
            // Terminal outcomes (cancellation on resume/teardown, or
            // SIGCONT) clear the re-arm function to break its
            // self-capture cycle; only the swallowed-SIGINT path re-arms
            // (ASan, issue #473).
            if (error) {
                *arm = nullptr;
                return;
            }
            if (fired != SIGCONT) {
                // SIGINT while suspended: swallowed; keep waiting.
                (*arm)();
                return;
            }
            *arm = nullptr;
            const auto self = weak.lock();
            if (self) self->resume_after_suspend();
        });
    };
    (*arm)();
    signals_ = std::move(signals);

    // pi `process.kill(0, "SIGTSTP")` through the host's closed action seam;
    // a null host performs the platform SIGTSTP directly.
    if (hooks_.suspend_process) {
        hooks_.suspend_process();
    }
}

void SuspendController::resume_after_suspend() {
    signals_.reset();
    if (!hooks_.is_running || !hooks_.is_running()) return;
    if (const auto started = hooks_.start_tui ? hooks_.start_tui() : support::ExpectedVoid{};
        !started) {
        if (hooks_.report_failure) {
            hooks_.report_failure(presentation_error(
                started.error(),
                "Native TUI resume after suspend failed"));
        }
        return;
    }
    if (const auto rendered = hooks_.render_tui ? hooks_.render_tui() : support::ExpectedVoid{};
        !rendered) {
        if (hooks_.report_failure) {
            hooks_.report_failure(startup_error(rendered.error()));
        }
        return;
    }
}

} // namespace cch::coding_agent::tui
