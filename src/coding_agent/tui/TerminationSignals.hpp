#pragma once

// Native TUI termination handling (pi interactive-mode `registerSignalHandlers`:
// SIGTERM always, SIGHUP): restore the terminal and exit 143/129 even when the
// Runtime loop cannot run. A Runtime-loop handler cannot give that guarantee —
// an Asio signal set delivers its completion on the parked loop, and the loop is
// parked inside a terminal read (holding the terminal's mutex) exactly when this
// escalation is needed. The process-level claim lives here; the frontend arms
// the terminal it owns.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include <cch/support/Error.hpp>

#include "support/UniqueFd.hpp"

#include <atomic>
#include <csignal>
#include <thread>

#include <termios.h>

namespace cch::coding_agent::tui {

/// The interactive frontend's claim on the process's termination signals.
///
/// The signals are claimed process-wide by a handler that only wakes a
/// dedicated thread through a self-pipe (the handler itself does no more than
/// one `write`), and that thread restores the terminal from the pre-raw
/// snapshot taken by arm_terminal() and exits the process. Neither step takes a
/// Runtime lock, a Terminal lock, or the Runtime loop, so the escape hatch
/// survives the state it exists for: a loop blocked in a terminal read. The
/// previous signal dispositions are restored on destruction.
///
/// pi parity boundary: pi's interactive-mode handler also disposes the session
/// before exiting. This claim is only the terminal-and-exit-code guarantee;
/// signal-driven session close is a separate follow-up (print mode's signal
/// path already disposes, and the interactive loop cannot be part of an exit
/// path that must work while the loop is blocked).
class TerminationSignals final {
public:
    /// pi signal exit codes (ADR 0036): the conventional 128 + signal number.
    static constexpr int kSigtermExitCode = 128 + SIGTERM;
    static constexpr int kSighupExitCode = 128 + SIGHUP;

    TerminationSignals();
    TerminationSignals(TerminationSignals&&) = delete;
    TerminationSignals& operator=(TerminationSignals&&) = delete;
    ~TerminationSignals();
    TerminationSignals(const TerminationSignals&) = delete;
    TerminationSignals& operator=(const TerminationSignals&) = delete;

    /// Snapshot the terminal this frontend owns before it enters raw mode. The
    /// snapshot is the exit path's whole restore source: `Terminal::stop()` is
    /// never consulted, because the signal thread must not wait for the mutex a
    /// parked read holds. Reports an error when the escape hatch cannot run —
    /// the signals were not claimed, or the terminal could not be snapshotted —
    /// in which case the frontend continues without the guarantee.
    [[nodiscard]] support::ExpectedVoid arm_terminal(int input_fd, int output_fd) noexcept;

private:
    /// Watches the self-pipe and performs the exit; runs on `watcher_`.
    void watch() noexcept;
    /// Restores the armed terminal and exits with the signal's code. Never
    /// returns.
    [[noreturn]] void terminate(int signal_number) noexcept;

    support::UniqueFd read_fd_;
    support::UniqueFd write_fd_;
    /// Why the claim is unavailable, when it is. A literal, so no allocation
    /// happens on the construction path and it stays valid for the object's
    /// lifetime.
    const char* claim_failure_{nullptr};
    struct sigaction previous_sigterm_{};
    struct sigaction previous_sighup_{};
    bool claimed_sigterm_{false};
    bool claimed_sighup_{false};
    std::atomic<bool> stopping_{false};
    std::thread watcher_;

    /// Written before `armed_` is published and only read after it is observed,
    /// so the signal thread reads a fully initialized snapshot.
    termios saved_termios_{};
    std::atomic<int> input_fd_{-1};
    std::atomic<int> output_fd_{-1};
    std::atomic<bool> armed_{false};
};

} // namespace cch::coding_agent::tui
