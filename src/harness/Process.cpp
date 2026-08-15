#include "Process.hpp"

#include "ai/BoundedText.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process/v1.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <boost/process/v1/posix.hpp>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cch::harness {
namespace {

struct OutputCapture {
    std::string data;
    std::size_t bytes{0};
    std::size_t lines{0};
    bool truncated{false};
    bool callback_failed{false};
};

void append_limited(
    OutputCapture& capture,
    const char* data,
    std::size_t size,
    const OutputLimit& limit,
    std::optional<std::move_only_function<void(std::string_view)>>& callback) {
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
    // the stream even after retained output stops growing. A throwing callback
    // is contained: it deactivates itself and is recorded, but the pipe keeps
    // draining to EOF so a full pipe can never strand the child process.
    if (callback) {
        try {
            (*callback)(std::string_view(data, size));
        } catch (...) {
            callback.reset();
            capture.callback_failed = true;
        }
    }
    if (offset < size) {
        capture.truncated = true;
    }
}

boost::asio::awaitable<OutputCapture> drain_pipe(
    boost::process::v1::async_pipe& pipe,
    OutputLimit limit,
    std::optional<std::move_only_function<void(std::string_view)>> callback) {
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
            capture.truncated = true;
            break;
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

boost::asio::io_context& io_context_from_executor(const boost::asio::any_io_executor& executor) {
    auto& context = boost::asio::query(executor, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context);
}

class ChildGuard {
public:
    ChildGuard(boost::process::v1::child& child, boost::process::v1::group& group)
        : child_(child), group_(group) {}

    ~ChildGuard() {
        if (released_) {
            return;
        }
        boost::system::error_code ignored;
        if (child_.running()) {
            group_.terminate(ignored);
            child_.terminate(ignored);
        }
        if (child_.joinable()) {
            child_.wait(ignored);
        }
    }

    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ChildGuard(ChildGuard&&) = delete;
    ChildGuard& operator=(ChildGuard&&) = delete;

    void release() { released_ = true; }

private:
    boost::process::v1::child& child_;
    boost::process::v1::group& group_;
    bool released_{false};
};

void request_supported_termination(
    boost::process::v1::child& child,
    boost::process::v1::group& process_group) {
#if defined(__unix__) || defined(__APPLE__)
    if (process_group.valid()) {
        (void)::killpg(process_group.native_handle(), SIGTERM);
    }
#else
    boost::system::error_code ignored;
    process_group.terminate(ignored);
    child.terminate(ignored);
#endif
    (void)child;
}

constexpr std::chrono::milliseconds poll_interval{100};
constexpr std::chrono::milliseconds sigkill_grace_period{100};

} // namespace

boost::asio::awaitable<support::Expected<ProcessResult>> DefaultAsyncProcessRunner::run(ProcessRequest request) {
    namespace bp = boost::process::v1;
    try {
        if (request.stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Operation aborted"));
        }

        auto executor = co_await boost::asio::this_coro::executor;
        auto& io = io_context_from_executor(executor);

        bp::async_pipe stdout_pipe(io);
        bp::async_pipe stderr_pipe(io);
        bp::environment child_environment = boost::this_process::environment();
        if (request.use_explicit_environment || !request.environment.empty()) {
            child_environment = bp::environment{};
            for (const auto& [key, value] : request.environment) {
                child_environment[key] = value;
            }
        }

        if (request.executable.empty()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "process execution failed",
                "process executable is empty"));
        }

        bp::group process_group;
        // When stderr is merged on a supported platform, bind the stdout
        // pipe's raw sink fd for the child's stderr so both streams share one
        // kernel pipe and the reader observes emission order. Binding the
        // same async_pipe twice would close its sink twice at spawn. On other
        // platforms the merge is unavailable and stderr stays a separate
        // drained stream.
#if defined(__unix__) || defined(__APPLE__)
        const bool stderr_merged = request.merge_stderr;
#else
        const bool stderr_merged = false;
#endif
        bp::child child = [&]() {
#if defined(__unix__) || defined(__APPLE__)
            if (stderr_merged) {
                // Handler order matters: the stderr fd bind must dup the pipe
                // sink before the std_out binding closes it in the child.
                return bp::child(
                    request.executable.string(),
                    bp::args(request.arguments),
                    bp::start_dir = request.working_directory.string(),
                    bp::posix::fd.bind(STDERR_FILENO, stdout_pipe.native_sink()),
                    bp::std_out > stdout_pipe,
                    child_environment,
                    process_group);
            }
#endif
            return bp::child(
                request.executable.string(),
                bp::args(request.arguments),
                bp::start_dir = request.working_directory.string(),
                bp::std_out > stdout_pipe,
                bp::std_err > stderr_pipe,
                child_environment,
                process_group);
        }();

        ChildGuard guard(child, process_group);

        // Spawn the drains with completion handlers so they start immediately
        // and run concurrently with the child-wait loop below. A child that
        // writes more than the kernel pipe buffer would otherwise block forever
        // on a full pipe and never reach EOF, deadlocking until timeout or
        // cancellation. The handlers capture these locals by reference; the
        // drain-completion wait below guarantees the captures outlive them.
        OutputCapture stdout_capture;
        OutputCapture stderr_capture;
        bool stdout_done = false;
        // A merged stderr shares the stdout pipe, so it never gets a separate
        // drain (its pipe has no writer and would block a reader forever).
        bool stderr_done = stderr_merged;
        std::exception_ptr stdout_exception;
        std::exception_ptr stderr_exception;

        boost::asio::co_spawn(
            executor,
            drain_pipe(stdout_pipe, request.output_limit, std::move(request.on_stdout)),
            [&](std::exception_ptr e, OutputCapture capture) {
                stdout_capture = std::move(capture);
                stdout_exception = e;
                stdout_done = true;
            });
        if (!stderr_merged) {
            boost::asio::co_spawn(
                executor,
                drain_pipe(stderr_pipe, request.output_limit, std::move(request.on_stderr)),
                [&](std::exception_ptr e, OutputCapture capture) {
                    stderr_capture = std::move(capture);
                    stderr_exception = e;
                    stderr_done = true;
                });
        }

        ProcessResult result;
        bool cancelled = false;
        const auto timeout_enabled = request.timeout.count() > 0;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + request.timeout;
        boost::asio::steady_timer timer(executor);

        while (child.running()) {
            if (request.stop_token.stop_requested()) {
                cancelled = true;
                stdout_pipe.cancel();
                stderr_pipe.cancel();
                request_supported_termination(child, process_group);
                break;
            }

            auto now = std::chrono::steady_clock::now();
            if (timeout_enabled && now >= deadline) {
                result.timed_out = true;
                stdout_pipe.cancel();
                stderr_pipe.cancel();
                request_supported_termination(child, process_group);
                break;
            }

            auto sleep_ms = timeout_enabled
                ? std::min(poll_interval, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
                : poll_interval;
            timer.expires_after(sleep_ms);
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec && ec != boost::asio::error::operation_aborted) {
                // An unrecoverable timer error: stop the child so the
                // drain-completion wait below cannot strand on a live child.
                stdout_pipe.cancel();
                stderr_pipe.cancel();
                request_supported_termination(child, process_group);
                break;
            }
        }

        if (!cancelled && request.stop_token.stop_requested()) {
            cancelled = true;
            stdout_pipe.cancel();
            stderr_pipe.cancel();
            if (child.running()) {
                request_supported_termination(child, process_group);
            }
        }

        if (cancelled || result.timed_out) {
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            boost::system::error_code ignored;
            if (process_group.valid()) {
                process_group.terminate(ignored);
            }
            if (child.running()) {
                child.terminate(ignored);
            }
        }

        // Wait for both drains to finish: EOF after a normal child exit, or
        // the operation_aborted delivered by the cancel calls above.
        while (!(stdout_done && stderr_done)) {
            timer.expires_after(poll_interval);
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
        }

        if (stdout_exception) {
            std::rethrow_exception(stdout_exception);
        }
        if (stderr_exception) {
            std::rethrow_exception(stderr_exception);
        }
        const bool callback_error =
            stdout_capture.callback_failed || stderr_capture.callback_failed;

        if (child.joinable()) {
            if (result.timed_out) {
                boost::system::error_code ignored;
                child.wait(ignored);
            } else {
                child.wait();
            }
        }

        guard.release();

        if (cancelled || request.stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "Operation aborted"));
        }
        if (callback_error) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Process, "process execution failed", "callback threw an exception"));
        }

        result.exit_code = child.exit_code();
        result.stdout_output = stdout_capture.data;
        result.stderr_output = stderr_capture.data;
        result.stdout_truncated = stdout_capture.truncated;
        result.stderr_truncated = stderr_capture.truncated;

        // Build combined output for compatibility (deterministic: stdout first, then stderr).
        result.output = stdout_capture.data + stderr_capture.data;
        if (stdout_capture.truncated || stderr_capture.truncated) {
            result.output += "\n[output truncated]";
        }
        co_return result;
    } catch (const std::exception& e) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Process, "process execution failed", e.what()));
    }
}

} // namespace cch::harness
