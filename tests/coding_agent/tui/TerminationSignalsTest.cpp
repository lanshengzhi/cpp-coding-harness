#include "coding_agent/tui/TerminationSignals.hpp"

#include "support/PseudoTerminal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

using namespace cch;

namespace {

/// A child that must exit through its termination path: the exit status and the
/// bytes it left on the terminal master carry the evidence.
struct ChildOutcome {
    int exit_code{-1};
    /// Set when the child died by a signal instead of exiting.
    std::optional<int> signal{};
    std::string terminal_output;
};

/// The child could not set itself up. Distinct from every termination outcome,
/// so a broken precondition cannot read as a passing check.
constexpr int kChildSetupFailed = 97;

/// Runs `body` in a forked child and returns what that child did, bounded: a
/// termination path that never exits fails the check instead of hanging the
/// suite.
template <typename Body> [[nodiscard]] ChildOutcome run_terminating_child(tests::PseudoTerminal& pty, Body body) {
    const pid_t child = ::fork();
    REQUIRE(child != -1);
    if (child == 0) {
        body();
        // The termination path exits; reaching this line is the failure.
        _exit(0);
    }
    const auto output = tests::read_available(pty.master.get(), std::chrono::seconds(1));
    int status = 0;
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            reaped = true;
            break;
        }
        if (result == -1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!reaped) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
        return ChildOutcome{.terminal_output = output};
    }
    ChildOutcome outcome{.terminal_output = output};
    if (WIFEXITED(status)) {
        outcome.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.signal = WTERMSIG(status);
    }
    return outcome;
}

/// The child body's shared setup: claim the signals, arm the terminal, and
/// enter raw mode the way a started TUI does.
void arm_started_terminal(
        coding_agent::tui::TerminationSignals& termination, tests::PseudoTerminal& pty, const termios& original) {
    if (!termination.arm_terminal(pty.slave.get(), pty.slave.get())) _exit(kChildSetupFailed);
    termios raw = original;
    ::cfmakeraw(&raw);
    if (::tcsetattr(pty.slave.get(), TCSANOW, &raw) != 0) _exit(kChildSetupFailed);
}

void check_terminal_restored(tests::PseudoTerminal& pty, const termios& original, const ChildOutcome& outcome) {
    termios restored{};
    REQUIRE(::tcgetattr(pty.slave.get(), &restored) == 0);
    CHECK(tests::same_terminal_state(restored, original));
    // The mode disables travel on the terminal itself, so the master sees them.
    CHECK(outcome.terminal_output.find("\x1b[?2004l") != std::string::npos);
    CHECK(outcome.terminal_output.find("\x1b[<u") != std::string::npos);
    CHECK(outcome.terminal_output.find("\x1b[?25h") != std::string::npos);
}

} // namespace

TEST_CASE("termination signals restore the terminal and exit with the signal's code",
        "[coding_agent][tui][signal][spec]") {
    // pi interactive-mode `registerSignalHandlers` exits 143 on SIGTERM and 129
    // on SIGHUP after restoring the terminal; a `kill` on a Native TUI session
    // must not leave raw mode, bracketed paste, or a hidden cursor behind.
    for (const auto& [signal_number, expected_exit_code] :
            {std::pair{SIGTERM, coding_agent::tui::TerminationSignals::kSigtermExitCode},
                    std::pair{SIGHUP, coding_agent::tui::TerminationSignals::kSighupExitCode}}) {
        auto pty = tests::open_pseudo_terminal();
        REQUIRE(pty);
        termios original{};
        REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

        const auto outcome = run_terminating_child(*pty, [&] {
            coding_agent::tui::TerminationSignals termination;
            arm_started_terminal(termination, *pty, original);
            ::raise(signal_number);
            // The watcher restores and exits well before this; sleeping here
            // only keeps a broken path from returning into the harness.
            ::usleep(5'000'000);
        });

        CHECK(outcome.exit_code == expected_exit_code);
        CHECK(outcome.signal == std::nullopt);
        check_terminal_restored(*pty, original, outcome);
    }
}

TEST_CASE("termination restore reaches the terminal while the runtime loop is parked",
        "[coding_agent][tui][signal][spec]") {
    // The guarantee the Runtime loop cannot give: an Asio signal set delivers
    // its completion on the loop thread, so a signal arriving while that thread
    // is parked inside a handler never reaches a handler of its own. The claim
    // is reactor-independent, so the terminal still comes back and the process
    // still exits 143.
    auto pty = tests::open_pseudo_terminal();
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    const auto outcome = run_terminating_child(*pty, [&] {
        coding_agent::tui::TerminationSignals termination;
        arm_started_terminal(termination, *pty, original);

        boost::asio::io_context io;
        auto work = boost::asio::make_work_guard(io);
        std::mutex parked_mutex;
        std::condition_variable parked;
        bool release = false;
        std::atomic<bool> handler_entered{false};
        boost::asio::post(io, [&] {
            std::unique_lock lock(parked_mutex);
            handler_entered.store(true, std::memory_order_release);
            parked.wait(lock, [&] { return release; });
        });
        std::thread loop([&] { io.run(); });
        while (!handler_entered.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        ::raise(SIGTERM);
        ::usleep(5'000'000);
        {
            std::lock_guard lock(parked_mutex);
            release = true;
        }
        parked.notify_all();
        loop.join();
        io.stop();
    });

    CHECK(outcome.exit_code == coding_agent::tui::TerminationSignals::kSigtermExitCode);
    CHECK(outcome.signal == std::nullopt);
    check_terminal_restored(*pty, original, outcome);
}

TEST_CASE("termination signals release the claim on destruction", "[coding_agent][tui][signal][spec]") {
    // The claim is owned by the interactive run: after the frontend is gone the
    // process must answer SIGTERM the way it did before the claim existed.
    auto pty = tests::open_pseudo_terminal();
    REQUIRE(pty);

    const auto outcome = run_terminating_child(*pty, [&] {
        struct sigaction default_action{};
        default_action.sa_handler = SIG_DFL;
        ::sigemptyset(&default_action.sa_mask);
        if (::sigaction(SIGTERM, &default_action, nullptr) != 0) _exit(kChildSetupFailed);
        {
            coding_agent::tui::TerminationSignals termination;
            if (!termination.arm_terminal(pty->slave.get(), pty->slave.get())) _exit(kChildSetupFailed);
        }
        ::raise(SIGTERM);
        ::usleep(5'000'000);
    });

    CHECK(outcome.signal == SIGTERM);
    CHECK(outcome.exit_code == -1);
}
