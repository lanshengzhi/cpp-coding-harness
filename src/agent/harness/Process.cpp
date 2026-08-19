#include "Process.hpp"

#include "ai/BoundedText.hpp"
#include "support/UniqueFd.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cch::harness {
namespace {

struct OutputCapture {
    std::string data;
    std::size_t bytes{0};
    std::size_t lines{0};
    bool truncated{false};
    std::optional<support::Error> callback_error;
};

[[nodiscard]] support::Error process_system_error(
    std::string message,
    const std::error_code& error) {
    return support::make_error(
        support::ErrorCode::Process,
        std::move(message),
        error.message());
}

[[nodiscard]] support::Error process_system_error(
    std::string message,
    const boost::system::error_code& error) {
    return support::make_error(
        support::ErrorCode::Process,
        std::move(message),
        error.message());
}

[[nodiscard]] std::error_code current_system_error() noexcept {
    return std::error_code(errno, std::generic_category());
}

void cancel_output_pipe(boost::asio::posix::stream_descriptor& pipe) noexcept {
    boost::system::error_code ignored;
    pipe.cancel(ignored);
}

[[nodiscard]] support::Error callback_failure(const support::Error& error) {
    std::string detail = error.detail.empty() ? error.message : error.detail;
    if (detail.empty()) {
        detail = "callback returned an error";
    }
    return support::make_error(
        support::ErrorCode::Process,
        "process output callback failed",
        std::move(detail));
}

[[nodiscard]] support::ExpectedVoid invoke_output_callback(
    std::move_only_function<support::ExpectedVoid(std::string_view)>& callback,
    std::string_view chunk) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        return callback(chunk);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "process output callback threw"));
    }
#endif
}

void append_limited(
    OutputCapture& capture,
    const char* data,
    std::size_t size,
    const OutputLimit& limit,
    std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>>& callback) {
    bool line_limit_reached = limit.max_lines == 0 || capture.lines >= limit.max_lines;
    std::size_t offset = 0;
    while (offset < size && capture.bytes < limit.max_bytes && !line_limit_reached) {
        const char ch = data[offset++];
        capture.data.push_back(ch);
        ++capture.bytes;
        if (ch == '\n') {
            ++capture.lines;
            if (capture.lines >= limit.max_lines) {
                line_limit_reached = true;
            }
        }
    }

    // Invoke the callback with the full chunk so live consumers keep observing
    // the stream even after retained output stops growing. A returned error
    // deactivates the callback, but the pipe keeps draining to EOF so a full
    // pipe can never strand the child process.
    if (callback) {
        if (auto delivered = invoke_output_callback(
                *callback,
                std::string_view(data, size));
            !delivered) {
            capture.callback_error = callback_failure(delivered.error());
            callback.reset();
        }
    }
    if (offset < size) {
        capture.truncated = true;
    }
}

[[nodiscard]] boost::asio::awaitable<support::Expected<OutputCapture>> drain_pipe(
    boost::asio::posix::stream_descriptor& pipe,
    OutputLimit limit,
    std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>> callback) {
    OutputCapture capture;
    std::array<char, 4096> buffer{};
    for (;;) {
        auto [ec, size] = co_await pipe.async_read_some(
            boost::asio::buffer(buffer),
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec == boost::asio::error::eof || ec == boost::asio::error::operation_aborted) {
            break;
        }
        if (ec) {
            co_return std::unexpected(process_system_error(
                "process output drain failed",
                ec));
        }
        append_limited(capture, buffer.data(), size, limit, callback);
    }
    if (capture.truncated) {
        // The byte cap can stop inside a multibyte UTF-8 character; re-bound
        // the captured text through the shared seam so model-visible output
        // never ends on a split sequence.
        capture.data = ai::bounded_utf8(capture.data, limit.max_bytes);
    }
    co_return capture;
}

/// The pointed-to descriptor and result slots must outlive the returned
/// awaitable; the caller waits for `done` before destroying them.
[[nodiscard]] boost::asio::awaitable<void> drain_pipe_into(
    boost::asio::posix::stream_descriptor* pipe,
    OutputLimit limit,
    std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>> callback,
    OutputCapture* capture,
    std::optional<support::Error>* drain_error,
    bool* done) {
    struct CompletionGuard final {
        bool* done;
        ~CompletionGuard() noexcept { *done = true; }
    } completion_guard{done};

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto outcome = co_await drain_pipe(*pipe, limit, std::move(callback));
        if (outcome) {
            *capture = std::move(*outcome);
        } else {
            *drain_error = std::move(outcome.error());
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (...) {
        *drain_error = support::make_error(
            support::ErrorCode::Process,
            "process output drain failed");
    }
#endif
    co_return;
}

boost::asio::io_context& io_context_from_executor(const boost::asio::any_io_executor& executor) {
    auto& context = boost::asio::query(executor, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context);
}

struct OwnedPipe {
    support::UniqueFd source;
    support::UniqueFd sink;
};

[[nodiscard]] bool set_nonblocking(int fd, std::error_code& error) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1) {
        error = current_system_error();
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        error = current_system_error();
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<OwnedPipe> make_pipe(std::error_code& error) {
    int fds[2]{};
    // CLOEXEC is applied atomically by pipe2 so a concurrent fork can never
    // inherit these descriptors between creation and flag setup.
    if (::pipe2(fds, O_CLOEXEC) == -1) {
        error = current_system_error();
        return std::nullopt;
    }
    return OwnedPipe{
        .source = support::UniqueFd(fds[0]),
        .sink = support::UniqueFd(fds[1]),
    };
}

enum class ChildSetupStage {
    ProcessGroup,
    WorkingDirectory,
    Stdout,
    Stderr,
    Exec,
};

struct ChildSetupFailure {
    int error_number;
    ChildSetupStage stage;
};

void report_child_setup_failure(int error_fd, ChildSetupStage stage, int error_number) noexcept {
    const ChildSetupFailure failure{.error_number = error_number, .stage = stage};
    const char* data = reinterpret_cast<const char*>(&failure);
    std::size_t remaining = sizeof(failure);
    while (remaining > 0) {
        const auto written = ::write(error_fd, data, remaining);
        if (written > 0) {
            data += written;
            remaining -= static_cast<std::size_t>(written);
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

void close_child_fd(int fd) noexcept {
    if (fd > STDERR_FILENO) {
        support::UniqueFd descriptor(fd);
        (void)descriptor.close();
    }
}

[[noreturn]] void run_child(
    const std::string& executable,
    const std::string& working_directory,
    const std::vector<char*>& arguments,
    const std::vector<char*>& environment,
    bool use_explicit_environment,
    int stdout_source,
    int stdout_sink,
    int stderr_source,
    int stderr_sink,
    int error_source,
    int error_sink,
    bool merge_stderr) noexcept {
    if (::setpgid(0, 0) == -1) {
        const int error_number = errno;
        report_child_setup_failure(error_sink, ChildSetupStage::ProcessGroup, error_number);
        _exit(127);
    }
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) == -1) {
        const int error_number = errno;
        report_child_setup_failure(error_sink, ChildSetupStage::WorkingDirectory, error_number);
        _exit(127);
    }
    if (::dup2(stdout_sink, STDOUT_FILENO) == -1) {
        const int error_number = errno;
        report_child_setup_failure(error_sink, ChildSetupStage::Stdout, error_number);
        _exit(127);
    }
    if (merge_stderr) {
        if (::dup2(stdout_sink, STDERR_FILENO) == -1) {
            const int error_number = errno;
            report_child_setup_failure(error_sink, ChildSetupStage::Stderr, error_number);
            _exit(127);
        }
    } else if (::dup2(stderr_sink, STDERR_FILENO) == -1) {
        const int error_number = errno;
        report_child_setup_failure(error_sink, ChildSetupStage::Stderr, error_number);
        _exit(127);
    }

    close_child_fd(stdout_source);
    close_child_fd(stdout_sink);
    close_child_fd(stderr_source);
    close_child_fd(stderr_sink);
    close_child_fd(error_source);

    if (use_explicit_environment) {
        ::execve(executable.c_str(), arguments.data(), environment.data());
    } else {
        ::execvp(executable.c_str(), arguments.data());
    }
    report_child_setup_failure(error_sink, ChildSetupStage::Exec, errno);
    _exit(127);
}

[[nodiscard]] const char* child_setup_stage_name(ChildSetupStage stage) noexcept {
    switch (stage) {
    case ChildSetupStage::ProcessGroup:
        return "process group";
    case ChildSetupStage::WorkingDirectory:
        return "working directory";
    case ChildSetupStage::Stdout:
        return "stdout";
    case ChildSetupStage::Stderr:
        return "stderr";
    case ChildSetupStage::Exec:
        return "exec";
    }
    return "setup";
}

struct ChildSetupReader {
    std::array<char, sizeof(ChildSetupFailure)> data{};
    std::size_t received{0};
};

[[nodiscard]] std::optional<ChildSetupFailure> read_child_setup_failure(
    int error_fd,
    ChildSetupReader& reader,
    std::error_code& error,
    bool& pending) noexcept {
    pending = false;
    while (reader.received < reader.data.size()) {
        const auto read_count = ::read(
            error_fd,
            reader.data.data() + reader.received,
            reader.data.size() - reader.received);
        if (read_count > 0) {
            reader.received += static_cast<std::size_t>(read_count);
            continue;
        }
        if (read_count == 0) {
            if (reader.received != 0) {
                error = std::make_error_code(std::errc::protocol_error);
            }
            return std::nullopt;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pending = true;
            return std::nullopt;
        }
        error = current_system_error();
        return std::nullopt;
    }

    ChildSetupFailure failure{};
    std::memcpy(&failure, reader.data.data(), sizeof(failure));
    return failure;
}

[[nodiscard]] support::Error child_setup_error(const ChildSetupFailure& failure) {
    const auto error = std::error_code(failure.error_number, std::generic_category());
    return support::make_error(
        support::ErrorCode::Process,
        "process execution failed",
        std::format(
            "child {} failed: {}",
            child_setup_stage_name(failure.stage),
            error.message()));
}

void request_supported_termination(pid_t child, pid_t process_group, int signal) noexcept {
    if (process_group > 0) {
        (void)::killpg(process_group, signal);
    }
    if (child > 0) {
        (void)::kill(child, signal);
    }
}

[[nodiscard]] bool poll_child(
    pid_t child,
    int& status,
    bool& reaped,
    std::error_code& error) noexcept {
    for (;;) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            reaped = true;
            return true;
        }
        if (result == 0) {
            return true;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        error = current_system_error();
        return false;
    }
}

constexpr std::chrono::milliseconds poll_interval{100};
constexpr std::chrono::milliseconds sigkill_grace_period{100};

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> wait_for_child(
    pid_t child,
    int& status,
    bool& reaped,
    boost::asio::steady_timer& timer) {
    while (!reaped) {
        std::error_code error;
        if (!poll_child(child, status, reaped, error)) {
            co_return std::unexpected(process_system_error(
                "process wait failed",
                error));
        }
        if (reaped) {
            break;
        }
        timer.expires_after(poll_interval);
        auto [ec] = co_await timer.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec && ec != boost::asio::error::operation_aborted) {
            co_return std::unexpected(process_system_error(
                "process wait timer failed",
                ec));
        }
    }
    co_return support::ExpectedVoid{};
}

/// Non-blocking deferred reap for the emergency path: after the guard has
/// terminated the child, drive a WNOHANG poll on the runtime loop until it is
/// reaped so the event loop is never blocked (§6.6 / ADR 0040).
[[nodiscard]] boost::asio::awaitable<void> reap_child_polled(pid_t child) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);
    int status = 0;
    bool reaped = false;
    auto result = co_await wait_for_child(child, status, reaped, timer);
    (void)result;
}

class ChildGuard {
public:
    ChildGuard(
        boost::asio::any_io_executor executor,
        pid_t child,
        pid_t process_group)
        : executor_(std::move(executor)),
          child_(child),
          process_group_(process_group) {}

    ~ChildGuard() {
        if (reaped_) {
            return;
        }
        request_supported_termination(child_, process_group_, SIGKILL);
        int status = 0;
        bool reaped = false;
        std::error_code ignored;
        // Never block the runtime loop (§6.6): reap immediately with WNOHANG;
        // if the child has not exited yet, hand the reap to a non-blocking
        // poll that the loop drives to completion.
        if (poll_child(child_, status, reaped, ignored) && !reaped) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                boost::asio::co_spawn(
                    executor_,
                    reap_child_polled(child_),
                    boost::asio::detached);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                // A deferred reap could not be scheduled while unwinding; the
                // child is terminated but may remain a zombie until the
                // harness exits. Blocking the runtime loop is never
                // acceptable, so no fallback is attempted here.
            }
#endif
        }
    }

    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ChildGuard(ChildGuard&&) = delete;
    ChildGuard& operator=(ChildGuard&&) = delete;

    void mark_reaped() noexcept { reaped_ = true; }

private:
    boost::asio::any_io_executor executor_;
    pid_t child_;
    pid_t process_group_;
    bool reaped_{false};
};

[[nodiscard]] int process_exit_code(int status) noexcept {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        // Match the prior Boost.Process `child::exit_code()` mapping: a
        // signaled exit reports the terminating signal, not 128 + signal.
        return WTERMSIG(status);
    }
    return -1;
}

} // namespace

boost::asio::awaitable<support::Expected<ProcessResult>> DefaultAsyncProcessRunner::run(ProcessRequest request) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Operation aborted"));
    }
    if (request.executable.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "process execution failed",
            "process executable is empty"));
    }

    auto executor = co_await boost::asio::this_coro::executor;
    auto& io = io_context_from_executor(executor);
    boost::asio::steady_timer timer(executor);

    std::error_code pipe_error;
    auto stdout_fds = make_pipe(pipe_error);
    if (!stdout_fds) {
        co_return std::unexpected(process_system_error(
            "process stdout pipe creation failed",
            pipe_error));
    }
    std::optional<OwnedPipe> stderr_fds;
    if (!request.merge_stderr) {
        stderr_fds = make_pipe(pipe_error);
        if (!stderr_fds) {
            co_return std::unexpected(process_system_error(
                "process stderr pipe creation failed",
                pipe_error));
        }
    }
    auto error_fds = make_pipe(pipe_error);
    if (!error_fds) {
        co_return std::unexpected(process_system_error(
            "process setup pipe creation failed",
            pipe_error));
    }

    const std::string executable = request.executable.string();
    const std::string working_directory = request.working_directory.string();
    std::vector<std::string> argument_storage;
    argument_storage.reserve(request.arguments.size() + 1);
    argument_storage.push_back(executable);
    for (auto& argument : request.arguments) {
        argument_storage.push_back(std::move(argument));
    }
    std::vector<char*> arguments;
    arguments.reserve(argument_storage.size() + 1);
    for (auto& argument : argument_storage) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    std::vector<std::string> environment_storage;
    std::vector<char*> environment;
    const bool use_explicit_environment =
        request.use_explicit_environment || !request.environment.empty();
    if (use_explicit_environment) {
        environment_storage.reserve(request.environment.size());
        for (const auto& [key, value] : request.environment) {
            std::string entry = key;
            entry += '=';
            entry += value;
            environment_storage.push_back(std::move(entry));
        }
        environment.reserve(environment_storage.size() + 1);
        for (auto& entry : environment_storage) {
            environment.push_back(entry.data());
        }
        environment.push_back(nullptr);
    }

    const pid_t child = ::fork();
    if (child == -1) {
        co_return std::unexpected(process_system_error(
            "process fork failed",
            current_system_error()));
    }
    if (child == 0) {
        run_child(
            executable,
            working_directory,
            arguments,
            environment,
            use_explicit_environment,
            stdout_fds->source.get(),
            stdout_fds->sink.get(),
            stderr_fds ? stderr_fds->source.get() : -1,
            stderr_fds ? stderr_fds->sink.get() : -1,
            error_fds->source.get(),
            error_fds->sink.get(),
            request.merge_stderr);
    }

    ChildGuard guard(executor, child, child);
    (void)stdout_fds->sink.close();
    if (stderr_fds) {
        (void)stderr_fds->sink.close();
    }
    (void)error_fds->sink.close();
    if (!set_nonblocking(error_fds->source.get(), pipe_error)) {
        co_return std::unexpected(process_system_error(
            "process setup status pipe configuration failed",
            pipe_error));
    }

    // The child establishes its own process group before exec. The parent
    // repeats the operation to close the short fork/kill race; EACCES means
    // the child already exec'd and ESRCH means it has already exited.
    if (::setpgid(child, child) == -1 && errno != EACCES && errno != ESRCH) {
        const auto error = current_system_error();
        request_supported_termination(child, child, SIGKILL);
        int status = 0;
        bool reaped = false;
        auto reap = co_await wait_for_child(child, status, reaped, timer);
        if (!reap) {
            co_return std::unexpected(std::move(reap.error()));
        }
        guard.mark_reaped();
        co_return std::unexpected(process_system_error(
            "process group setup failed",
            error));
    }

    boost::asio::posix::stream_descriptor stdout_pipe(io);
    boost::system::error_code assign_error;
    stdout_pipe.assign(stdout_fds->source.get(), assign_error);
    if (assign_error) {
        co_return std::unexpected(process_system_error(
            "process stdout pipe setup failed",
            assign_error));
    }
    (void)stdout_fds->source.release();

    boost::asio::posix::stream_descriptor stderr_pipe(io);
    if (stderr_fds) {
        assign_error.clear();
        stderr_pipe.assign(stderr_fds->source.get(), assign_error);
        if (assign_error) {
            co_return std::unexpected(process_system_error(
                "process stderr pipe setup failed",
                assign_error));
        }
        (void)stderr_fds->source.release();
    }

    OutputCapture stdout_capture;
    OutputCapture stderr_capture;
    std::optional<support::Error> stdout_drain_error;
    std::optional<support::Error> stderr_drain_error;
    bool stdout_done = false;
    const bool stderr_merged = request.merge_stderr;
    bool stderr_done = stderr_merged;
    std::optional<support::Error> terminal_error;

    auto start_drain = [&](boost::asio::posix::stream_descriptor& pipe,
                           OutputLimit limit,
                           std::optional<std::move_only_function<support::ExpectedVoid(std::string_view)>> callback,
                           OutputCapture& capture,
                           std::optional<support::Error>& drain_error,
                           bool& done) -> bool {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            boost::asio::co_spawn(
                executor,
                drain_pipe_into(
                    &pipe,
                    limit,
                    std::move(callback),
                    &capture,
                    &drain_error,
                    &done),
                boost::asio::detached);
            return true;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            done = true;
            drain_error = support::make_error(
                support::ErrorCode::Process,
                "process output drain setup failed");
            return false;
        }
#endif
    };

    const bool stdout_started = start_drain(
        stdout_pipe,
        request.output_limit,
        std::move(request.on_stdout),
        stdout_capture,
        stdout_drain_error,
        stdout_done);
    if (!stdout_started) {
        terminal_error = std::move(stdout_drain_error);
        if (!stderr_merged) {
            stderr_done = true;
        }
    } else if (!stderr_merged && !start_drain(
                              stderr_pipe,
                              request.output_limit,
                              std::move(request.on_stderr),
                              stderr_capture,
                              stderr_drain_error,
                              stderr_done)) {
        terminal_error = std::move(stderr_drain_error);
    }

    ProcessResult result;
    bool cancelled = false;
    bool child_reaped = false;
    int child_status = 0;
    const auto timeout_enabled = request.timeout.count() > 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + request.timeout;

    if (terminal_error) {
        cancel_output_pipe(stdout_pipe);
        if (!stderr_merged) {
            cancel_output_pipe(stderr_pipe);
        }
        request_supported_termination(child, child, SIGTERM);
    }

    while (!child_reaped) {
        std::error_code wait_error;
        if (!poll_child(child, child_status, child_reaped, wait_error)) {
            terminal_error = process_system_error("process wait failed", wait_error);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            break;
        }
        if (child_reaped) {
            guard.mark_reaped();
            break;
        }
        if (request.stop_token.stop_requested()) {
            cancelled = true;
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (timeout_enabled && now >= deadline) {
            result.timed_out = true;
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            break;
        }
        if (terminal_error || stdout_drain_error || stderr_drain_error) {
            if (!terminal_error) {
                terminal_error = stdout_drain_error
                    ? std::move(stdout_drain_error)
                    : std::move(stderr_drain_error);
            }
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            break;
        }

        const auto sleep_ms = timeout_enabled
            ? std::min(
                poll_interval,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
            : poll_interval;
        timer.expires_after(sleep_ms);
        auto [ec] = co_await timer.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec && ec != boost::asio::error::operation_aborted) {
            terminal_error = process_system_error("process wait timer failed", ec);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            break;
        }
    }

    if (!cancelled && request.stop_token.stop_requested()) {
        cancelled = true;
        cancel_output_pipe(stdout_pipe);
        if (!stderr_merged) {
            cancel_output_pipe(stderr_pipe);
        }
        request_supported_termination(child, child, SIGTERM);
    }

    if (cancelled || result.timed_out || terminal_error) {
        timer.expires_after(sigkill_grace_period);
        auto [ec] = co_await timer.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        (void)ec;
        request_supported_termination(child, child, SIGKILL);
        if (!child_reaped) {
            auto reap = co_await wait_for_child(
                child,
                child_status,
                child_reaped,
                timer);
            if (!reap) {
                terminal_error = std::move(reap.error());
            }
            if (child_reaped) {
                guard.mark_reaped();
            }
        }
    }

    // Wait for both drains to finish: EOF after a normal child exit, or
    // operation_aborted delivered by the cancellation calls above. Both
    // streams remain active after retained output reaches its bound.
    while (!(stdout_done && stderr_done)) {
        if (!terminal_error && (stdout_drain_error || stderr_drain_error)) {
            terminal_error = stdout_drain_error
                ? std::move(stdout_drain_error)
                : std::move(stderr_drain_error);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            // A drain can fail after the leader has exited while a descendant
            // still owns the pipe. Stop that process group before waiting for
            // the sibling drain to quiesce.
            request_supported_termination(child, child, SIGTERM);
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            request_supported_termination(child, child, SIGKILL);
        }
        if (!cancelled && request.stop_token.stop_requested()) {
            cancelled = true;
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            request_supported_termination(child, child, SIGKILL);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
        } else if (timeout_enabled && !result.timed_out &&
                   std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
            request_supported_termination(child, child, SIGTERM);
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            request_supported_termination(child, child, SIGKILL);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
        }
        if (stdout_done && stderr_done) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto sleep_ms = timeout_enabled && !result.timed_out
            ? std::min(
                poll_interval,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
            : poll_interval;
        timer.expires_after(sleep_ms);
        auto [ec] = co_await timer.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec && ec != boost::asio::error::operation_aborted) {
            terminal_error = process_system_error("process output wait failed", ec);
            cancel_output_pipe(stdout_pipe);
            if (!stderr_merged) {
                cancel_output_pipe(stderr_pipe);
            }
        }
    }

    if (!child_reaped) {
        auto reap = co_await wait_for_child(
            child,
            child_status,
            child_reaped,
            timer);
        if (!reap) {
            terminal_error = std::move(reap.error());
        }
        if (child_reaped) {
            guard.mark_reaped();
        }
    }

    if (!terminal_error && (stdout_drain_error || stderr_drain_error)) {
        terminal_error = stdout_drain_error
            ? std::move(stdout_drain_error)
            : std::move(stderr_drain_error);
    }

    ChildSetupReader setup_reader;
    bool setup_read_pending = false;
    const auto setup_read_deadline =
        std::chrono::steady_clock::now() + sigkill_grace_period;
    for (;;) {
        std::error_code setup_read_error;
        auto setup_failure = read_child_setup_failure(
            error_fds->source.get(),
            setup_reader,
            setup_read_error,
            setup_read_pending);
        if (setup_read_error) {
            terminal_error = process_system_error(
                "process setup status read failed",
                setup_read_error);
            break;
        }
        if (setup_failure) {
            terminal_error = child_setup_error(*setup_failure);
            break;
        }
        if (!setup_read_pending) {
            break;
        }
        if (request.stop_token.stop_requested()) {
            cancelled = true;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= setup_read_deadline) {
            if (!result.timed_out && timeout_enabled && now >= deadline) {
                result.timed_out = true;
            }
            if (!result.timed_out) {
                terminal_error = support::make_error(
                    support::ErrorCode::Process,
                    "process setup status pipe did not close");
            }
            request_supported_termination(child, child, SIGTERM);
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(
                boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            request_supported_termination(child, child, SIGKILL);
            break;
        }
        timer.expires_after(poll_interval);
        auto [ec] = co_await timer.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec && ec != boost::asio::error::operation_aborted) {
            terminal_error = process_system_error(
                "process setup status wait failed",
                ec);
            break;
        }
    }

    if (cancelled || request.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Operation aborted"));
    }
    if (stdout_capture.callback_error) {
        co_return std::unexpected(std::move(*stdout_capture.callback_error));
    }
    if (stderr_capture.callback_error) {
        co_return std::unexpected(std::move(*stderr_capture.callback_error));
    }
    if (terminal_error) {
        co_return std::unexpected(std::move(*terminal_error));
    }
    result.exit_code = process_exit_code(child_status);
    result.stdout_output = std::move(stdout_capture.data);
    result.stderr_output = std::move(stderr_capture.data);
    result.stdout_truncated = stdout_capture.truncated;
    result.stderr_truncated = stderr_capture.truncated;

    // Build combined output for compatibility (deterministic: stdout first,
    // then stderr).
    result.output = result.stdout_output + result.stderr_output;
    if (result.stdout_truncated || result.stderr_truncated) {
        result.output += "\n[output truncated]";
    }
    co_return result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "process execution failed",
            error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "process execution failed"));
    }
#endif
}

} // namespace cch::harness
