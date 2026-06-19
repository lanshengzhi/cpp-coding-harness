#include "Process.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
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
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cch::util {
namespace {

struct OutputCapture {
    std::string data;
    std::size_t bytes{0};
    std::size_t lines{0};
    bool truncated{false};
};

void append_limited(
    OutputCapture& capture,
    const char* data,
    std::size_t size,
    std::size_t max_bytes,
    std::size_t max_lines,
    std::optional<std::move_only_function<void(std::string_view)>>& callback) {
    bool line_limit_reached = max_lines == 0 || capture.lines >= max_lines;
    std::size_t offset = 0;
    while (offset < size && capture.bytes < max_bytes && !line_limit_reached) {
        const char ch = data[offset++];
        capture.data.push_back(ch);
        ++capture.bytes;
        if (ch == '\n') {
            ++capture.lines;
            if (capture.lines >= max_lines) {
                line_limit_reached = true;
            }
        }
    }
    // Invoke callback with the chunk we read (before truncation).
    if (callback) {
        try {
            (*callback)(std::string_view(data, size));
        } catch (...) {
            // Callback failure becomes an execution error at the caller level.
            throw;
        }
    }
    if (offset < size) {
        capture.truncated = true;
    }
}

boost::asio::awaitable<OutputCapture> drain_pipe(
    boost::process::v1::async_pipe& pipe,
    std::size_t max_bytes,
    std::size_t max_lines,
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
        try {
            append_limited(capture, buffer.data(), size, max_bytes, max_lines, callback);
        } catch (...) {
            // Callback threw — rethrow to propagate as an error.
            throw;
        }
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
        if (!released_ && child_.running()) {
            boost::system::error_code ignored;
            group_.terminate(ignored);
            child_.terminate(ignored);
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

constexpr std::chrono::milliseconds poll_interval{100};
constexpr std::chrono::milliseconds sigkill_grace_period{100};

} // namespace

boost::asio::awaitable<Expected<ProcessResult>> DefaultProcessRunner::run(ProcessRequest request) {
    namespace bp = boost::process::v1;
    try {
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

        bp::group process_group;
        bp::child child(
            bp::search_path("bash"),
            "-lc",
            request.command,
            bp::start_dir = request.working_directory.string(),
            bp::std_out > stdout_pipe,
            bp::std_err > stderr_pipe,
            child_environment,
            process_group);

        ChildGuard guard(child, process_group);

        // Move callbacks into the drain coroutines.
        auto stdout_drain = boost::asio::co_spawn(
            executor,
            drain_pipe(stdout_pipe, request.max_output_bytes, request.max_output_lines,
                       std::move(request.on_stdout)),
            boost::asio::deferred);
        auto stderr_drain = boost::asio::co_spawn(
            executor,
            drain_pipe(stderr_pipe, request.max_output_bytes, request.max_output_lines,
                       std::move(request.on_stderr)),
            boost::asio::deferred);

        ProcessResult result;
        const auto timeout_enabled = request.timeout.count() > 0;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + request.timeout;
        boost::asio::steady_timer timer(executor);

        while (child.running()) {
            auto now = std::chrono::steady_clock::now();
            if (timeout_enabled && now >= deadline) {
                result.timed_out = true;
                boost::system::error_code ignored;
                stdout_pipe.cancel();
                stderr_pipe.cancel();
                process_group.terminate(ignored);
                child.terminate(ignored);
                break;
            }

            auto sleep_ms = timeout_enabled
                ? std::min(poll_interval, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
                : poll_interval;
            timer.expires_after(sleep_ms);
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec && ec != boost::asio::error::operation_aborted) {
                break;
            }
        }

        if (result.timed_out) {
            timer.expires_after(sigkill_grace_period);
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            (void)ec;
            if (child.running()) {
#if defined(__unix__) || defined(__APPLE__)
                ::kill(child.id(), SIGKILL);
#else
                boost::system::error_code ignored;
                process_group.terminate(ignored);
                child.terminate(ignored);
#endif
            }
        }

        OutputCapture stdout_capture;
        OutputCapture stderr_capture;
        bool callback_error = false;

        try {
            stdout_capture = co_await std::move(stdout_drain);
        } catch (...) {
            callback_error = true;
        }
        try {
            stderr_capture = co_await std::move(stderr_drain);
        } catch (...) {
            callback_error = true;
        }

        if (child.joinable()) {
            if (result.timed_out) {
                boost::system::error_code ignored;
                child.wait(ignored);
            } else {
                child.wait();
            }
        }

        guard.release();

        if (callback_error) {
            co_return std::unexpected(make_error(ErrorCode::Process, "process execution failed", "callback threw an exception"));
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
        co_return std::unexpected(make_error(ErrorCode::Process, "process execution failed", e.what()));
    }
}

} // namespace cch::util
