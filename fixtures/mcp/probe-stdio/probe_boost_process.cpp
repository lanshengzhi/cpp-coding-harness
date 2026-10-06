// Standalone MCP stdio roundtrip through Boost.Process v2 + Asio pipes (#866).
//
// Second launch mechanic measured against probe_stdio.cpp: instead of the raw
// pipe2/fork/execvp path that mirrors src/agent/harness/Process.cpp, this
// variant launches the child with `boost::process::v2::process` and exchanges
// newline-delimited frames over `boost::asio::readable_pipe` /
// `boost::asio::writable_pipe`. Both are already in the pinned vcpkg baseline
// (boost-process 1.91.0, Boost.Asio 1.91.0), so this is the smallest-LOC
// integration path.
//
// Compiled with the project's strict configuration
// (`-fno-exceptions -DBOOST_ASIO_NO_EXCEPTIONS`).

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <boost/throw_exception.hpp>

#include <glaze/glaze.hpp>

#include <cstdio>
#include <cstdlib>
#include <istream>
#include <optional>
#include <string>
#include <string_view>

#include <signal.h>

namespace boost {

#if defined(BOOST_NO_EXCEPTIONS)

BOOST_NORETURN void throw_exception(const std::exception&) { std::terminate(); }

BOOST_NORETURN void throw_exception(const std::exception&, const boost::source_location&) { std::terminate(); }

#endif

} // namespace boost

namespace {

constexpr std::chrono::milliseconds kReadTimeout{5000};

int g_failures = 0;

void report(bool passed, const std::string& message) {
    if (!passed) {
        ++g_failures;
    }
    std::printf("[probe-bp] %-4s %s\n", passed ? "PASS" : "FAIL", message.c_str());
    std::fflush(stdout);
}

std::optional<int> response_id(std::string_view line) {
    auto decoded = glz::read_json<glz::generic>(line);
    if (!decoded) {
        return std::nullopt;
    }
    const auto* object = decoded->get_if<glz::generic::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto id = object->find("id");
    if (id == object->end() || !id->second.holds<double>()) {
        return std::nullopt;
    }
    return static_cast<int>(id->second.get<double>());
}

boost::asio::awaitable<bool> write_frame(boost::asio::writable_pipe& sink, std::string payload) {
    payload += "\n";
    std::printf("[wire-bp] >> %s", payload.c_str());
    const auto [error, count] = co_await boost::asio::async_write(
            sink, boost::asio::buffer(payload), boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)count;
    co_return !error;
}

boost::asio::awaitable<std::optional<std::string>> read_frame(boost::asio::readable_pipe& source) {
    boost::asio::streambuf buffer;
    const auto [error, count] = co_await boost::asio::async_read_until(
            source, buffer, '\n', boost::asio::as_tuple(boost::asio::use_awaitable));
    if (error) {
        std::printf("[probe-bp] read error: %s\n", error.message().c_str());
        co_return std::nullopt;
    }
    std::istream stream(&buffer);
    std::string line;
    std::getline(stream, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    (void)count;
    std::printf("[wire-bp] << %s\n", line.c_str());
    co_return line;
}

boost::asio::awaitable<void> run_probe(boost::asio::io_context& io, const std::string& script_path, int* exit_code) {
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::readable_pipe stdout_pipe(executor);
    boost::asio::writable_pipe stdin_pipe(executor);

    boost::process::v2::process child(executor,
            "/usr/bin/python3",
            {script_path},
            boost::process::v2::process_stdio{.in = stdin_pipe, .out = stdout_pipe, .err = stderr});
    std::printf("[probe-bp] spawned echo server pid=%d\n", static_cast<int>(child.id()));

    const std::string initialize =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\","
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"probe-bp\",\"version\":\"0.1.0\"}}}";
    if (!co_await write_frame(stdin_pipe, initialize)) {
        report(false, "initialize: write failed");
        *exit_code = 1;
        co_return;
    }
    const auto initialize_line = co_await read_frame(stdout_pipe);
    report(initialize_line.has_value() && response_id(*initialize_line) == 1,
            "initialize: response id=" +
                    std::to_string(initialize_line.has_value() ? response_id(*initialize_line).value_or(-1) : -1));

    const std::string tools_call = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/"
                                   "call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello\"}}}";
    if (!co_await write_frame(stdin_pipe, tools_call)) {
        report(false, "tools/call: write failed");
        *exit_code = 1;
        co_return;
    }
    const auto tools_line = co_await read_frame(stdout_pipe);
    const bool tools_ok = tools_line.has_value() && response_id(*tools_line) == 2 &&
                          tools_line->find("\"text\":\"hello\"") != std::string::npos;
    report(tools_ok,
            "tools/call: response id=" +
                    std::to_string(tools_line.has_value() ? response_id(*tools_line).value_or(-1) : -1));

    boost::system::error_code ignored;
    (void)stdin_pipe.close(ignored);
    (void)child.wait(ignored);
    *exit_code = g_failures == 0 ? 0 : 1;
    (void)io;
    co_return;
}

} // namespace

int main(int argc, char** argv) {
    (void)::signal(SIGPIPE, SIG_IGN);
    const std::string script_path = argc > 1 ? argv[1] : "echo_server.py";

    int exit_code = 1;
    boost::asio::io_context io;
    boost::asio::co_spawn(io, run_probe(io, script_path, &exit_code), boost::asio::detached);
    io.run();

    std::printf("[probe-bp] RESULT: %s (failures=%d)\n", exit_code == 0 ? "PASS" : "FAIL", g_failures);
    return exit_code;
}
