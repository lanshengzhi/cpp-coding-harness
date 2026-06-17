#include "Process.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process/v1.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <string>

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
    std::size_t max_lines) {
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
    if (offset < size) {
        capture.truncated = true;
    }
}

boost::asio::awaitable<void> drain_pipe(
    boost::process::v1::async_pipe& pipe,
    OutputCapture& capture,
    std::size_t max_bytes,
    std::size_t max_lines,
    bool& done) {
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
        append_limited(capture, buffer.data(), size, max_bytes, max_lines);
    }
    done = true;
}

boost::asio::io_context& io_context_from_executor(const boost::asio::any_io_executor& executor) {
    auto& context = boost::asio::query(executor, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context);
}

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

        OutputCapture stdout_capture;
        OutputCapture stderr_capture;
        bool stdout_done = false;
        bool stderr_done = false;

        boost::asio::co_spawn(
            executor,
            drain_pipe(stdout_pipe, stdout_capture, request.max_output_bytes, request.max_output_lines, stdout_done),
            boost::asio::detached);
        boost::asio::co_spawn(
            executor,
            drain_pipe(stderr_pipe, stderr_capture, request.max_output_bytes, request.max_output_lines, stderr_done),
            boost::asio::detached);

        ProcessResult result;
        const auto timeout_enabled = request.timeout.count() > 0;
        const auto deadline = std::chrono::steady_clock::now() + request.timeout;
        boost::asio::steady_timer timer(executor);

        while (child.running()) {
            if (timeout_enabled && std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                boost::system::error_code ignored;
                stdout_pipe.cancel();
                stderr_pipe.cancel();
                process_group.terminate(ignored);
                child.terminate(ignored);
                break;
            }
            timer.expires_after(std::chrono::milliseconds(10));
            auto [ec] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec && ec != boost::asio::error::operation_aborted) {
                break;
            }
        }

        while (child.running()) {
            timer.expires_after(std::chrono::milliseconds(10));
            co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        }
        if (!result.timed_out) {
            child.wait();
        } else {
            boost::system::error_code ignored;
            child.wait(ignored);
        }

        while (!stdout_done || !stderr_done) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        }

        result.exit_code = child.exit_code();
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
