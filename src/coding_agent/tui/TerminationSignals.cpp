#include "coding_agent/tui/TerminationSignals.hpp"

#include <cch/tui/Terminal.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <format>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// The self-pipe the handler writes to. A signal handler may only call
/// async-signal-safe functions, so the descriptor is a `volatile sig_atomic_t`
/// rather than shared object state, and the handler's whole body is one write.
/// One claim per process: the interactive frontend owns this claim.
volatile sig_atomic_t g_termination_fd = -1;

/// Longest the exit path waits for the terminal to accept the mode restore
/// before exiting anyway: the restore is hygiene, the exit is the guarantee.
constexpr auto kRestoreWriteBudget = std::chrono::milliseconds(200);

extern "C" void handle_termination_signal(int signal_number) {
    const sig_atomic_t descriptor = g_termination_fd;
    if (descriptor < 0) return;
    const char payload = static_cast<char>(signal_number);
    (void)::write(static_cast<int>(descriptor), &payload, 1);
}

[[nodiscard]] bool claim_signal(int signal_number, struct sigaction& previous) noexcept {
    struct sigaction action{};
    action.sa_handler = &handle_termination_signal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return ::sigaction(signal_number, &action, &previous) == 0;
}

/// Best-effort bounded write: a parked Runtime loop may have left the
/// terminal's output queue full, and the exit decision must not wait on it.
void write_restore_sequence(int descriptor, std::string_view sequence) noexcept {
    if (descriptor < 0) return;
    pollfd item{.fd = descriptor, .events = POLLOUT, .revents = 0};
    if (::poll(&item, 1, static_cast<int>(kRestoreWriteBudget.count())) <= 0) return;
    std::size_t written = 0;
    while (written < sequence.size()) {
        const auto count = ::write(descriptor, sequence.data() + written, sequence.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return;
    }
}

} // namespace

TerminationSignals::TerminationSignals() {
    int fds[2]{};
    // Both ends are non-blocking: the handler must never block, and the watcher
    // polls rather than assuming a pending payload.
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        claim_failure_ = "the signal self-pipe could not be created";
        return;
    }
    read_fd_.reset(fds[0]);
    write_fd_.reset(fds[1]);
    g_termination_fd = write_fd_.get();
    // SIGTERM always, SIGHUP where the platform has one (pi registerSignalHandlers).
    claimed_sigterm_ = claim_signal(SIGTERM, previous_sigterm_);
#ifdef SIGHUP
    claimed_sighup_ = claim_signal(SIGHUP, previous_sighup_);
#endif
    if (!claimed_sigterm_ && !claimed_sighup_) {
        g_termination_fd = -1;
        (void)write_fd_.close();
        (void)read_fd_.close();
        claim_failure_ = "SIGTERM and SIGHUP could not be claimed";
        return;
    }
    watcher_ = std::thread([this] { watch(); });
}

TerminationSignals::~TerminationSignals() {
    // Retire the claim before anything else: a handler that already loaded the
    // descriptor must not observe a descriptor this object no longer owns, and
    // an already-delivered signal must take the disposition that was in force
    // before the claim. The descriptors themselves stay open until the members
    // are destroyed, so the whole teardown below is inside that guarantee.
    if (claimed_sigterm_) (void)::sigaction(SIGTERM, &previous_sigterm_, nullptr);
    if (claimed_sighup_) (void)::sigaction(SIGHUP, &previous_sighup_, nullptr);
    g_termination_fd = -1;
    stopping_.store(true, std::memory_order_release);
    // The watcher polls its descriptor, so it observes `stopping_` without a
    // wake byte.
    if (watcher_.joinable()) watcher_.join();
}

support::ExpectedVoid TerminationSignals::arm_terminal(int input_fd, int output_fd) noexcept {
    if (claim_failure_ != nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Process, "termination signals unavailable", claim_failure_));
    }
    if (::tcgetattr(input_fd, &saved_termios_) != 0) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "could not snapshot the terminal for the termination path",
                std::format("tcgetattr: {}", std::error_code(errno, std::generic_category()).message())));
    }
    input_fd_.store(input_fd, std::memory_order_relaxed);
    output_fd_.store(output_fd, std::memory_order_relaxed);
    // Publishes the snapshot above to the signal thread.
    armed_.store(true, std::memory_order_release);
    return {};
}

void TerminationSignals::watch() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
        pollfd item{.fd = read_fd_.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&item, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) continue;
        char payload = 0;
        const auto count = ::read(read_fd_.get(), &payload, sizeof(payload));
        if (count <= 0) continue;
        terminate(static_cast<unsigned char>(payload));
    }
}

void TerminationSignals::terminate(int signal_number) noexcept {
    const int exit_code = signal_number == SIGHUP ? kSighupExitCode : kSigtermExitCode;
    if (armed_.load(std::memory_order_acquire)) {
        const termios saved = saved_termios_;
        (void)::tcsetattr(input_fd_.load(std::memory_order_relaxed), TCSAFLUSH, &saved);
        write_restore_sequence(output_fd_.load(std::memory_order_relaxed), cch::tui::kTerminalEmergencyRestoreSequence);
    }
    ::_exit(exit_code);
}

} // namespace cch::coding_agent::tui
