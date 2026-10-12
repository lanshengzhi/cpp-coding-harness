#include "ClipboardProcess.hpp"

#include "support/UniqueFd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// The standard base64 alphabet (pi `Buffer.from(text).toString("base64")`).
[[nodiscard]] std::string base64_encode(std::string_view text) {
    static constexpr std::string_view kAlphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((text.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= text.size()) {
        const auto chunk = (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index])) << 16U) |
                           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1])) << 8U) |
                           static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 2]));
        encoded.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
        encoded.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
        encoded.push_back(kAlphabet[(chunk >> 6U) & 0x3fU]);
        encoded.push_back(kAlphabet[chunk & 0x3fU]);
        index += 3;
    }
    const auto remaining = text.size() - index;
    if (remaining == 1) {
        const auto chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(text[index])) << 16U;
        encoded.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
        encoded.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
        encoded.append("==");
    } else if (remaining == 2) {
        const auto chunk = (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index])) << 16U) |
                           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[index + 1])) << 8U);
        encoded.push_back(kAlphabet[(chunk >> 18U) & 0x3fU]);
        encoded.push_back(kAlphabet[(chunk >> 12U) & 0x3fU]);
        encoded.push_back(kAlphabet[(chunk >> 6U) & 0x3fU]);
        encoded.push_back('=');
    }
    return encoded;
}

[[nodiscard]] std::string environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

/// pi `isWSL` from `wsl.ts`: the distro markers, else `/proc/version` mentioning
/// Microsoft or WSL.
[[nodiscard]] bool detect_wsl_environment() {
    if (!environment_value("WSL_DISTRO_NAME").empty() || !environment_value("WSLENV").empty()) {
        return true;
    }
    std::ifstream version("/proc/version");
    if (!version) return false;
    const std::string release((std::istreambuf_iterator<char>(version)), std::istreambuf_iterator<char>());
    std::string lowered = release;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return lowered.find("microsoft") != std::string::npos || lowered.find("wsl") != std::string::npos;
}

/// Resolve one helper against the injected `PATH`-shaped search path (pi's
/// `spawn` uses the process `PATH`; injection keeps the lookup deterministic in
/// tests). Absolute and relative command names are used as written.
[[nodiscard]] std::optional<std::string> resolve_executable(
        std::string_view search_path, const std::string& command) {
    if (command.empty()) return std::nullopt;
    if (command.find('/') != std::string::npos) {
        if (::access(command.c_str(), X_OK) == 0) return command;
        return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= search_path.size()) {
        const auto end = search_path.find(':', start);
        const auto directory = search_path.substr(start, end == std::string_view::npos ? end : end - start);
        if (!directory.empty()) {
            const auto candidate = (std::filesystem::path{directory} / command).string();
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

[[nodiscard]] bool set_non_blocking(const support::UniqueFd& fd) {
    if (!fd) return false;
    const auto flags = ::fcntl(fd.get(), F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) == 0;
}

/// SIGPIPE is blocked for the calling thread while a helper's stdin is
/// written: a `write` on a pipe whose reader already exited would otherwise
/// kill the whole process instead of reporting EPIPE (pi swallows that error
/// too). Blocking is per-thread, so no other process thread is affected.
class ScopedSigpipeBlock final {
public:
    ScopedSigpipeBlock() {
        sigemptyset(&pipemask_);
        sigaddset(&pipemask_, SIGPIPE);
        (void)::pthread_sigmask(SIG_BLOCK, &pipemask_, &previous_);
    }
    ~ScopedSigpipeBlock() {
        // Consume the pending SIGPIPE this thread's own write raised before the
        // mask is restored, so it cannot terminate the process later.
        sigset_t pending{};
        if (::sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            const timespec no_wait{0, 0};
            (void)::sigtimedwait(&pipemask_, nullptr, &no_wait);
        }
        (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
    ScopedSigpipeBlock(const ScopedSigpipeBlock&) = delete;
    ScopedSigpipeBlock& operator=(const ScopedSigpipeBlock&) = delete;

private:
    sigset_t pipemask_{};
    sigset_t previous_{};
};

void write_to_pipe(const support::UniqueFd& fd, std::string_view input, std::size_t& written, bool& closed) {
    const ScopedSigpipeBlock sigpipe_block;
    while (written < input.size()) {
        const auto count = ::write(fd.get(), input.data() + written, input.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        // EPIPE (the helper exited early) or a real failure: pi ignores it.
        closed = true;
        break;
    }
}

/// One forked helper: the owned descriptors, the input still to write, the
/// captured output, and the accounting both drivers share.
struct ForkedCommand {
    pid_t pid{-1};
    support::UniqueFd stdin_fd{};
    support::UniqueFd stdout_fd{};
    std::string_view input{};
    std::size_t written{0};
    bool stdin_closed{false};
    bool stdout_closed{false};
    bool overflow{false};
    bool timed_out{false};
    bool cancelled{false};
    std::string output{};
    std::size_t output_limit{0};
    std::chrono::steady_clock::time_point deadline{};
    std::optional<int> exit_code{std::nullopt};
    bool reaped{false};

    ForkedCommand() = default;
    ForkedCommand(ForkedCommand&& other) noexcept
        : pid(std::exchange(other.pid, -1)),
          stdin_fd(std::move(other.stdin_fd)),
          stdout_fd(std::move(other.stdout_fd)),
          input(other.input),
          written(other.written),
          stdin_closed(other.stdin_closed),
          stdout_closed(other.stdout_closed),
          overflow(other.overflow),
          timed_out(other.timed_out),
          cancelled(other.cancelled),
          output(std::move(other.output)),
          output_limit(other.output_limit),
          deadline(other.deadline),
          exit_code(other.exit_code),
          reaped(std::exchange(other.reaped, true)) {}
    ForkedCommand& operator=(ForkedCommand&& other) noexcept {
        if (this != &other) {
            reap_now();
            pid = std::exchange(other.pid, -1);
            stdin_fd = std::move(other.stdin_fd);
            stdout_fd = std::move(other.stdout_fd);
            input = other.input;
            written = other.written;
            stdin_closed = other.stdin_closed;
            stdout_closed = other.stdout_closed;
            overflow = other.overflow;
            timed_out = other.timed_out;
            cancelled = other.cancelled;
            output = std::move(other.output);
            output_limit = other.output_limit;
            deadline = other.deadline;
            exit_code = other.exit_code;
            reaped = std::exchange(other.reaped, true);
        }
        return *this;
    }
    ForkedCommand(const ForkedCommand&) = delete;
    ForkedCommand& operator=(const ForkedCommand&) = delete;

    /// Never leave a child behind: an unfinished helper is killed and reaped.
    ~ForkedCommand() { reap_now(); }

    /// Block until the helper is reaped (SIGKILL cannot be ignored).
    void reap_now() {
        if (reaped || pid <= 0) return;
        const auto killed = ::kill(pid, SIGKILL);
        (void)killed;
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        reaped = true;
        exit_code = std::nullopt;
    }

    /// Abort the helper the way pi's `abort()` does: SIGKILL, drop the output
    /// pipe (so a daemonized grandchild cannot hold this call open), and reap.
    void abort() {
        (void)stdin_fd.close();
        (void)stdout_fd.close();
        stdin_closed = true;
        stdout_closed = true;
        output.clear();
        exit_code = std::nullopt;
        reap_now();
    }
};

/// Launch one helper with pi's stdio contract:
/// `["pipe", input === undefined ? "pipe" : "ignore", "ignore"]`.
[[nodiscard]] std::optional<ForkedCommand> launch_command(const ClipboardCommandRequest& request) {
    const auto executable = resolve_executable(request.search_path, request.command);
    if (!executable) return std::nullopt;
    const bool capture_stdout = !request.input.has_value();

    int stdin_pipe[2];
    if (::pipe(stdin_pipe) != 0) return std::nullopt;
    support::UniqueFd stdin_read{stdin_pipe[0]};
    support::UniqueFd stdin_write{stdin_pipe[1]};
    support::UniqueFd stdout_read;
    support::UniqueFd stdout_write;
    if (capture_stdout) {
        int stdout_pipe[2];
        if (::pipe(stdout_pipe) != 0) return std::nullopt;
        stdout_read.reset(stdout_pipe[0]);
        stdout_write.reset(stdout_pipe[1]);
    }

    const auto pid = ::fork();
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
        // Child. The descriptors carrying the other ends must not survive into
        // the helper: a leaked output write end would keep this call waiting
        // for an EOF that never arrives.
        support::UniqueFd devnull{::open("/dev/null", O_RDWR)};
        if (::dup2(stdin_read.get(), STDIN_FILENO) < 0) ::_exit(127);
        if (capture_stdout) {
            if (::dup2(stdout_write.get(), STDOUT_FILENO) < 0) ::_exit(127);
        } else if (devnull) {
            (void)::dup2(devnull.get(), STDOUT_FILENO);
        }
        if (devnull) (void)::dup2(devnull.get(), STDERR_FILENO);
        (void)stdin_read.close();
        (void)stdin_write.close();
        if (capture_stdout) (void)stdout_read.close();
        (void)stdout_write.close();
        std::vector<char*> argv;
        argv.reserve(request.args.size() + 2);
        argv.push_back(const_cast<char*>(request.command.c_str()));
        for (const auto& arg : request.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable->c_str(), argv.data());
        ::_exit(127);
    }

    // Parent: the read end of the helper's stdin and the write end of its
    // stdout belong to the helper alone.
    (void)stdin_read.close();
    (void)stdout_write.close();
    ForkedCommand run;
    run.pid = pid;
    run.stdin_fd = std::move(stdin_write);
    run.stdout_fd = std::move(stdout_read);
    run.input = request.input ? std::string_view{*request.input} : std::string_view{};
    run.output_limit = request.max_buffer_bytes;
    run.deadline = std::chrono::steady_clock::now() + request.timeout;
    (void)set_non_blocking(run.stdin_fd);
    (void)set_non_blocking(run.stdout_fd);
    return run;
}

void advance_input(ForkedCommand& run) {
    if (run.stdin_closed) return;
    if (run.written < run.input.size()) {
        write_to_pipe(run.stdin_fd, run.input, run.written, run.stdin_closed);
    }
    if (!run.stdin_closed && run.written >= run.input.size()) {
        (void)run.stdin_fd.close();
        run.stdin_closed = true;
    }
}

void advance_output(ForkedCommand& run) {
    if (!run.stdout_fd) {
        run.stdout_closed = true;
        return;
    }
    std::array<char, 64 * 1024> buffer{};
    while (!run.stdout_closed) {
        const auto count = ::read(run.stdout_fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            const auto length = static_cast<std::size_t>(count);
            if (run.output.size() + length > run.output_limit) {
                run.overflow = true;
                return;
            }
            run.output.append(buffer.data(), length);
            continue;
        }
        if (count == 0) {
            run.stdout_closed = true;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        run.stdout_closed = true;
        return;
    }
}

/// Advance one helper by one non-blocking step; true when it reached its
/// terminal outcome. A reaped helper still yields its captured stdout until
/// EOF, which is what pi's `close` event waits for.
[[nodiscard]] bool advance(ForkedCommand& run) {
    advance_input(run);
    advance_output(run);
    if (run.overflow) {
        run.abort();
        return true;
    }
    if (!run.reaped) {
        int status = 0;
        const auto waited = ::waitpid(run.pid, &status, WNOHANG);
        if (waited == run.pid) {
            run.reaped = true;
            run.exit_code = WIFEXITED(status) ? std::optional<int>{WEXITSTATUS(status)} : std::nullopt;
        } else if (waited < 0 && errno != EINTR) {
            run.reaped = true;
            run.exit_code = std::nullopt;
        }
    }
    if (!run.reaped) return false;
    return run.stdout_closed || !run.stdout_fd;
}

/// pi's `undefined`: the helper produced no result, whatever the reason.
[[nodiscard]] ClipboardCommandResult take_result(ForkedCommand& run) {
    ClipboardCommandResult result;
    result.timed_out = run.timed_out;
    result.cancelled = run.cancelled;
    result.output_overflow = run.overflow;
    result.exit_code = run.exit_code;
    if (run.overflow) {
        result.exit_code = std::nullopt;
    } else {
        result.output = std::move(run.output);
    }
    return result;
}

/// One polling step shared by both drivers: 1 ms keeps a cancelled or timed-out
/// helper's SIGKILL close to the moment the caller asked for it while costing
/// nothing for the helpers that finish in their first poll.
constexpr std::chrono::milliseconds kClipboardPollInterval{1};

} // namespace

bool clipboard_is_remote_session(const ClipboardEnvironment& environment) {
    return !environment.ssh_connection.empty() || !environment.ssh_client.empty() ||
           !environment.mosh_connection.empty();
}

bool clipboard_is_headless(const ClipboardEnvironment& environment) {
    return environment.is_linux && environment.display.empty() && environment.wayland_display.empty() &&
           environment.termux_version.empty();
}

ClipboardEnvironment clipboard_environment_from_process() {
    ClipboardEnvironment environment;
#if defined(__linux__)
    environment.is_linux = true;
#else
    environment.is_linux = false;
#endif
    environment.termux_version = environment_value("TERMUX_VERSION");
    environment.wayland_display = environment_value("WAYLAND_DISPLAY");
    environment.display = environment_value("DISPLAY");
    environment.ssh_connection = environment_value("SSH_CONNECTION");
    environment.ssh_client = environment_value("SSH_CLIENT");
    environment.mosh_connection = environment_value("MOSH_CONNECTION");
    environment.wt_session = environment_value("WT_SESSION");
    environment.is_wsl = detect_wsl_environment();
    environment.search_path = environment_value("PATH");
    return environment;
}

boost::asio::awaitable<ClipboardCommandResult> run_clipboard_command(
        ClipboardCommandRequest request, std::stop_token stop_token) {
    auto run = launch_command(request);
    if (!run) co_return ClipboardCommandResult{};
    const auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer tick(executor);
    while (!advance(*run)) {
        if (stop_token.stop_requested()) {
            run->cancelled = true;
            run->abort();
            break;
        }
        if (std::chrono::steady_clock::now() >= run->deadline) {
            run->timed_out = true;
            run->abort();
            break;
        }
        tick.expires_after(kClipboardPollInterval);
        boost::system::error_code wait_error;
        co_await tick.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    }
    co_return take_result(*run);
}

ClipboardCommandResult run_clipboard_command_blocking(const ClipboardCommandRequest& request) {
    auto run = launch_command(request);
    if (!run) return ClipboardCommandResult{};
    while (!advance(*run)) {
        if (std::chrono::steady_clock::now() >= run->deadline) {
            run->timed_out = true;
            run->abort();
            break;
        }
        std::this_thread::sleep_for(kClipboardPollInterval);
    }
    return take_result(*run);
}

std::optional<std::string> encode_clipboard_osc52(std::string_view text) {
    auto encoded = base64_encode(text);
    if (encoded.size() > kMaxOsc52EncodedLength) return std::nullopt;
    std::string sequence;
    sequence.reserve(encoded.size() + 7);
    sequence.append("\x1b]52;c;");
    sequence.append(encoded);
    sequence.push_back('\x07');
    return sequence;
}

std::string make_clipboard_unique_token() {
    std::random_device random;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random());
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::string value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value.push_back('-');
        value += std::format("{:02x}", bytes[index]);
    }
    return value;
}

} // namespace cch::coding_agent::tui
