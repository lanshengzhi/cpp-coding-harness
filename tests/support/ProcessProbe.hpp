#pragma once

// Process-exit probes for cancellation and reaping contracts (#478 Bucket 1,
// issue #524). A bare `kill(pid, 0) == -1` assertion conflates "the process
// exited" with "the PID left the process table". The second depends on whoever
// adopts the exited process: a container whose PID 1 never reaps (e.g.
// `tail -f /dev/null`, the entrypoint of the Arch pinned CI lane) keeps
// terminated descendants visible as zombies indefinitely. These probes treat
// an exited process — reaped or zombie — as gone, and poll bounded so the
// scheduling delay between SIGTERM/SIGKILL delivery and the observed exit
// cannot turn the check into a flake.

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <cerrno>
#include <signal.h>
#include <sys/types.h>

namespace cch::tests {

/// Kernel process state from /proc/<pid>/stat ('R', 'S', 'Z', ...), or '\0'
/// when /proc has no entry for the pid.
[[nodiscard]] inline char process_state(pid_t pid) noexcept {
    const std::string stat_path = std::format("/proc/{}/stat", pid);
    std::FILE* file = std::fopen(stat_path.c_str(), "r");
    if (file == nullptr) {
        return '\0';
    }
    char buffer[512];
    const bool read_ok = std::fgets(buffer, sizeof(buffer), file) != nullptr;
    std::fclose(file);
    if (!read_ok) {
        return '\0';
    }
    // The comm field may contain spaces and parentheses; the state character
    // follows the final ')'.
    const char* comm_end = std::strrchr(buffer, ')');
    if (comm_end == nullptr || comm_end[1] != ' ') {
        return '\0';
    }
    return comm_end[2];
}

/// True once the process has exited: either the PID is fully gone (reaped,
/// reported as ESRCH) or /proc still lists it in Z state — the kernel has
/// recorded the exit and only an external reaper's wait remains (#524).
[[nodiscard]] inline bool process_exited(pid_t pid) noexcept {
    errno = 0;
    if (::kill(pid, 0) == -1 && errno == ESRCH) {
        return true;
    }
    return process_state(pid) == 'Z';
}

[[nodiscard]] inline std::optional<pid_t> parse_pid(std::string_view pid_text) noexcept {
    // Recorded pid files end with a newline; tolerate surrounding whitespace.
    while (!pid_text.empty() && (pid_text.front() == ' ' || pid_text.front() == '\n' ||
                                    pid_text.front() == '\t' || pid_text.front() == '\r')) {
        pid_text.remove_prefix(1);
    }
    while (!pid_text.empty() && (pid_text.back() == ' ' || pid_text.back() == '\n' ||
                                    pid_text.back() == '\t' || pid_text.back() == '\r')) {
        pid_text.remove_suffix(1);
    }
    pid_t pid{};
    const auto [end, error] =
        std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
    if (error != std::errc{} || end == pid_text.data() || end != pid_text.data() + pid_text.size()) {
        return std::nullopt;
    }
    return pid;
}

/// Immediate probe over a recorded pid string: false when the text does not
/// parse, otherwise process_exited(pid_t).
[[nodiscard]] inline bool process_exited(std::string_view pid_text) noexcept {
    const auto pid = parse_pid(pid_text);
    return pid.has_value() && process_exited(*pid);
}

constexpr std::chrono::milliseconds kDefaultExitPollBudget{2000};
constexpr std::chrono::milliseconds kExitPollInterval{10};

/// Bounded poll loop shared by the exit probes: the predicate is re-checked
/// every kExitPollInterval until it holds or the budget expires (#478 Bucket 1
/// methodology).
template <typename Probe>
[[nodiscard]] bool poll_process(
    pid_t pid,
    Probe&& probe,
    std::chrono::milliseconds budget) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (probe(pid)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(kExitPollInterval);
    }
}

/// Bounded poll for full reaping over a recorded pid string. Use this for a
/// process whose reaping this test process owns (the spawned group leader:
/// DefaultAsyncProcessRunner drives waitpid on it before returning), where
/// accepting a zombie would mask a missing reap path.
[[nodiscard]] inline bool await_process_reaped(
    std::string_view pid_text,
    std::chrono::milliseconds budget = kDefaultExitPollBudget) noexcept {
    const auto pid = parse_pid(pid_text);
    if (!pid.has_value()) {
        return false;
    }
    return poll_process(*pid, [](pid_t candidate) noexcept {
        errno = 0;
        return ::kill(candidate, 0) == -1 && errno == ESRCH;
    }, budget);
}

/// Bounded poll for process_exited over a recorded pid string (#478 Bucket 1
/// methodology): converges within milliseconds on a reaping host and accepts
/// the terminal zombie state where no external reaper exists. Use this only
/// for descendants the test process never owned: after the leader dies they
/// are adopted by an external reaper whose wait timing is outside the
/// harness's control (#524).
[[nodiscard]] inline bool await_process_exit(
    std::string_view pid_text,
    std::chrono::milliseconds budget = kDefaultExitPollBudget) noexcept {
    const auto pid = parse_pid(pid_text);
    if (!pid.has_value()) {
        return false;
    }
    return poll_process(*pid, [](pid_t candidate) noexcept {
        return process_exited(candidate);
    }, budget);
}

} // namespace cch::tests
