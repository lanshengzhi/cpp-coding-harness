#include "../../third_party/catch2/catch_test_macros.hpp"

#include "support/PseudoTerminal.hpp"
#include "support/TempWorkspace.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <utility>

namespace {

class ChildProcessCleanup final {
public:
    explicit ChildProcessCleanup(pid_t pid) : pid_(pid) {}
    ChildProcessCleanup(ChildProcessCleanup&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)) {}
    ChildProcessCleanup& operator=(ChildProcessCleanup&& other) noexcept {
        if (this != &other) {
            terminate();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }
    ~ChildProcessCleanup() {
        terminate();
    }
    ChildProcessCleanup(const ChildProcessCleanup&) = delete;
    ChildProcessCleanup& operator=(const ChildProcessCleanup&) = delete;

    void dismiss() {
        pid_ = -1;
    }

private:
    void terminate() {
        if (pid_ <= 0) return;
        (void)::kill(pid_, SIGKILL);
        (void)::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }

    pid_t pid_{-1};
};

} // namespace

TEST_CASE(
    "interactive CLI selects Native TUI and submits a positional initial prompt",
    "[cli][tui][terminal][issue64]") {
    auto pty = cch::tests::open_pseudo_terminal(60, 12);
    REQUIRE(pty);
    termios original{};
    REQUIRE(::tcgetattr(pty->slave.get(), &original) == 0);

    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace config;
    const auto binary = std::string{CCH_BINARY};
    const auto workspace_path = workspace.path().string();
    const auto config_path = config.path().string();

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        (void)pty->master.close();
        if (::setsid() < 0 || ::ioctl(pty->slave.get(), TIOCSCTTY, 0) != 0 ||
            ::dup2(pty->slave.get(), STDIN_FILENO) < 0 ||
            ::dup2(pty->slave.get(), STDOUT_FILENO) < 0 ||
            ::dup2(pty->slave.get(), STDERR_FILENO) < 0 ||
            ::setenv("CCH_CODING_AGENT_DIR", config_path.c_str(), 1) != 0) {
            ::_exit(126);
        }
        ::execl(
            binary.c_str(),
            binary.c_str(),
            "--fake",
            "--no-session",
            "--workspace",
            workspace_path.c_str(),
            "pty initial",
            static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ChildProcessCleanup cleanup{child};

    std::string output;
    REQUIRE(cch::tests::wait_until(
        [&] {
            output.append(cch::tests::read_available(
                pty->master.get(),
                std::chrono::milliseconds(20)));
            return output.find("fake: pty initial") != std::string::npos;
        },
        std::chrono::seconds(4)));

    constexpr char kExit = '\x04';
    REQUIRE(::write(pty->master.get(), &kExit, 1) == 1);
    int status = 0;
    REQUIRE(cch::tests::wait_until(
        [&] { return ::waitpid(child, &status, WNOHANG) == child; },
        std::chrono::seconds(4)));
    cleanup.dismiss();

    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    termios restored{};
    REQUIRE(::tcgetattr(pty->slave.get(), &restored) == 0);
    CHECK(cch::tests::same_terminal_state(restored, original));
}

#else

TEST_CASE(
    "interactive CLI Native TUI smoke is limited to supported platforms",
    "[cli][tui][terminal][issue64]") {
    SUCCEED("Native TUI Process Terminal support is limited to Linux and macOS");
}

#endif
