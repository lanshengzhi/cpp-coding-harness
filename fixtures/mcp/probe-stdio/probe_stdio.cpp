// Standalone MCP stdio transport roundtrip probe (#866).
//
// Not wired into CMake or the test suite. Compile and run through build.sh /
// run.sh in this directory; see README.md for the exact re-run steps.
//
// It spawns `echo_server.py` as a long-lived child with a *writable* stdin
// pipe (the shape the real MCP stdio transport needs, unlike the one-shot
// `DefaultAsyncProcessRunner`, which gives the child /dev/null on stdin) and
// drives one newline-delimited JSON-RPC roundtrip:
//
//   initialize -> notifications/initialized -> tools/list -> tools/call
//   -> malformed-frame fault -> invalid-JSON-RPC fault
//   -> server-crash-mid-request fault
//
// Process-launch mechanics mirror src/agent/harness/Process.cpp: pipe2 with
// O_CLOEXEC, fork, setpgid, dup2, execvp; stdout is read through a
// boost::asio::posix::stream_descriptor. Compiled with the project's strict
// configuration (`-fno-exceptions -DBOOST_ASIO_NO_EXCEPTIONS`).

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/throw_exception.hpp>

#include <glaze/glaze.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace boost {

#if defined(BOOST_NO_EXCEPTIONS)

// Strict -fno-exceptions build: any Boost path that would throw terminates,
// matching src/support/BoostExceptionHandler.cpp.
BOOST_NORETURN void throw_exception(const std::exception&) { std::terminate(); }

BOOST_NORETURN void throw_exception(const std::exception&, const boost::source_location&) { std::terminate(); }

#endif

} // namespace boost

namespace {

using Generic = glz::generic;
using Object = Generic::object_t;

constexpr std::chrono::milliseconds kReadTimeout{5000};

int g_failures = 0;
bool g_trace = false;

void trace(std::string_view direction, std::string_view frame) {
    if (g_trace) {
        std::printf("[wire] %s %.*s\n", direction.data(), static_cast<int>(frame.size()), frame.data());
        std::fflush(stdout);
    }
}

void report(bool passed, const std::string& message) {
    if (!passed) {
        ++g_failures;
    }
    std::printf("[probe] %-4s %s\n", passed ? "PASS" : "FAIL", message.c_str());
    std::fflush(stdout);
}

const Generic* member(const Generic& value, std::string_view key) {
    const auto* object = value.get_if<Object>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto it = object->find(key);
    if (it == object->end()) {
        return nullptr;
    }
    return &it->second;
}

const Generic* element(const Generic& value, std::size_t index) {
    const auto* array = value.get_if<Generic::array_t>();
    if (array == nullptr || index >= array->size()) {
        return nullptr;
    }
    return &(*array)[index];
}

std::optional<std::string> as_string(const Generic* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* text = value->get_if<std::string>();
    if (text == nullptr) {
        return std::nullopt;
    }
    return *text;
}

/// `member` for a possibly-null parent, so extraction chains read linearly.
const Generic* member_of(const Generic* value, std::string_view key) {
    if (value == nullptr) {
        return nullptr;
    }
    return member(*value, key);
}

/// `element` for a possibly-null parent.
const Generic* element_of(const Generic* value, std::size_t index) {
    if (value == nullptr) {
        return nullptr;
    }
    return element(*value, index);
}

enum class LineKind {
    Response,
    Notification,
    MalformedJson,
    InvalidJsonRpc,
};

struct ParsedLine {
    LineKind kind{LineKind::MalformedJson};
    int id{0};
    bool is_error{false};
    Generic value;
};

/// Classifies one frame the way pi's `parseJsonRpcMessage` does: a line that is
/// not JSON is a parse failure; a JSON object that is not a request,
/// notification, or response is an invalid message.
ParsedLine parse_line(const std::string& line) {
    ParsedLine parsed;
    auto decoded = glz::read_json<Generic>(line);
    if (!decoded) {
        parsed.kind = LineKind::MalformedJson;
        return parsed;
    }
    parsed.value = std::move(*decoded);
    const auto* object = parsed.value.get_if<Object>();
    if (object == nullptr) {
        parsed.kind = LineKind::InvalidJsonRpc;
        return parsed;
    }
    const auto version = object->find("jsonrpc");
    if (version == object->end() || !version->second.holds<std::string>() ||
            version->second.get<std::string>() != "2.0") {
        parsed.kind = LineKind::InvalidJsonRpc;
        return parsed;
    }
    const auto id = object->find("id");
    const auto method = object->find("method");
    const bool has_id = id != object->end() && id->second.holds<double>();
    const bool has_method = method != object->end() && method->second.holds<std::string>();
    if (has_id) {
        parsed.kind = LineKind::Response;
        parsed.id = static_cast<int>(id->second.get<double>());
        parsed.is_error = object->find("error") != object->end();
        return parsed;
    }
    if (has_method) {
        parsed.kind = LineKind::Notification;
        return parsed;
    }
    parsed.kind = LineKind::InvalidJsonRpc;
    return parsed;
}

bool write_all(int fd, std::string_view text) {
    std::size_t written = 0;
    while (written < text.size()) {
        const auto count = ::write(fd, text.data() + written, text.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool make_pipe(int fds[2]) { return ::pipe2(fds, O_CLOEXEC) == 0; }

struct SpawnedServer {
    pid_t pid{-1};
    int stdin_write_fd{-1};
    int stdout_read_fd{-1};
    int stderr_read_fd{-1};
};

std::optional<SpawnedServer> spawn_server(const std::string& script_path) {
    int stdin_pipe[2]{};
    int stdout_pipe[2]{};
    int stderr_pipe[2]{};
    if (!make_pipe(stdin_pipe) || !make_pipe(stdout_pipe) || !make_pipe(stderr_pipe)) {
        return std::nullopt;
    }
    const pid_t child = ::fork();
    if (child == -1) {
        return std::nullopt;
    }
    if (child == 0) {
        (void)::setpgid(0, 0);
        (void)::dup2(stdin_pipe[0], STDIN_FILENO);
        (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        (void)::dup2(stderr_pipe[1], STDERR_FILENO);
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        (void)::close(stderr_pipe[0]);
        (void)::close(stderr_pipe[1]);
        ::execlp("python3", "python3", script_path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (::setpgid(child, child) == -1 && errno != EACCES && errno != ESRCH) {
        // The child sets its own group before exec; the parent repeats it to
        // close the fork/kill race. EACCES/ESRCH mean it already exec'd/exited.
    }
    (void)::close(stdin_pipe[0]);
    (void)::close(stdout_pipe[1]);
    (void)::close(stderr_pipe[1]);
    return SpawnedServer{
            .pid = child,
            .stdin_write_fd = stdin_pipe[1],
            .stdout_read_fd = stdout_pipe[0],
            .stderr_read_fd = stderr_pipe[0],
    };
}

enum class ReadStatus {
    Line,
    Eof,
    Timeout,
    Error,
};

struct ReadResult {
    ReadStatus status{ReadStatus::Error};
    std::string line;
};

class StdioProbe {
public:
    StdioProbe(boost::asio::io_context& io, int stdout_read_fd, int stdin_write_fd)
        : pipe_(io), stdin_write_fd_(stdin_write_fd) {
        boost::system::error_code error;
        pipe_.assign(stdout_read_fd, error);
    }

    [[nodiscard]] int stdin_write_fd() const { return stdin_write_fd_; }

    /// Reads one newline-delimited frame. A trailing `\r` is stripped, matching
    /// pi's `handleStdout`.
    boost::asio::awaitable<ReadResult> read_line() {
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }
                co_return ReadResult{.status = ReadStatus::Line, .line = std::move(line)};
            }

            read_timed_out_ = false;
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            timer.expires_after(kReadTimeout);
            timer.async_wait([this](const boost::system::error_code& error) {
                if (!error) {
                    read_timed_out_ = true;
                    boost::system::error_code ignored;
                    pipe_.cancel(ignored);
                }
            });

            std::array<char, 4096> chunk{};
            const auto [error, count] = co_await pipe_.async_read_some(
                    boost::asio::buffer(chunk), boost::asio::as_tuple(boost::asio::use_awaitable));
            timer.cancel();
            if (error == boost::asio::error::eof) {
                co_return ReadResult{.status = ReadStatus::Eof, .line = {}};
            }
            if (error) {
                co_return ReadResult{.status = read_timed_out_ ? ReadStatus::Timeout : ReadStatus::Error, .line = {}};
            }
            if (count == 0) {
                continue;
            }
            buffer_.append(chunk.data(), count);
        }
    }

    bool send_request(int id, std::string_view method, std::string_view params_json) {
        std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
        payload += std::to_string(id);
        payload += ",\"method\":\"";
        payload += method;
        payload += "\"";
        if (!params_json.empty()) {
            payload += ",\"params\":";
            payload += params_json;
        }
        payload += "}";
        payload += "\n";
        trace(">>", std::string_view(payload).substr(0, payload.size() - 1));
        return write_all(stdin_write_fd_, payload);
    }

    bool send_notification(std::string_view method, std::string_view params_json) {
        std::string payload = "{\"jsonrpc\":\"2.0\",\"method\":\"";
        payload += method;
        payload += "\"";
        if (!params_json.empty()) {
            payload += ",\"params\":";
            payload += params_json;
        }
        payload += "}";
        payload += "\n";
        trace(">>", std::string_view(payload).substr(0, payload.size() - 1));
        return write_all(stdin_write_fd_, payload);
    }

private:
    boost::asio::posix::stream_descriptor pipe_;
    int stdin_write_fd_;
    std::string buffer_;
    bool read_timed_out_{false};
};

struct RpcOutcome {
    bool responded{false};
    bool is_error{false};
    bool closed{false};
    bool timed_out{false};
    int id{0};
    Generic result;
    std::vector<std::string> transport_errors;
};

boost::asio::awaitable<RpcOutcome> await_response(StdioProbe& probe, int id) {
    RpcOutcome outcome;
    outcome.id = id;
    for (;;) {
        const auto read = co_await probe.read_line();
        if (read.status == ReadStatus::Eof) {
            outcome.closed = true;
            co_return outcome;
        }
        if (read.status == ReadStatus::Timeout) {
            outcome.timed_out = true;
            co_return outcome;
        }
        if (read.status == ReadStatus::Error) {
            outcome.closed = true;
            co_return outcome;
        }
        auto parsed = parse_line(read.line);
        trace("<<", read.line);
        switch (parsed.kind) {
        case LineKind::MalformedJson:
            outcome.transport_errors.push_back("malformed JSON frame: " + read.line);
            continue;
        case LineKind::InvalidJsonRpc:
            outcome.transport_errors.push_back("invalid JSON-RPC message: " + read.line);
            continue;
        case LineKind::Notification:
            continue;
        case LineKind::Response:
            if (parsed.id == id) {
                outcome.responded = true;
                outcome.is_error = parsed.is_error;
                outcome.result = std::move(parsed.value);
                co_return outcome;
            }
            outcome.transport_errors.push_back("response for unknown id " + std::to_string(parsed.id));
            continue;
        }
    }
}

std::string describe_transport_errors(const std::vector<std::string>& errors) {
    std::string joined;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i != 0) {
            joined += " | ";
        }
        joined += errors[i];
    }
    return joined;
}

boost::asio::awaitable<void> run_probe(StdioProbe& probe, pid_t child_pid, int* exit_code) {
    // 1. initialize
    if (!probe.send_request(1,
                "initialize",
                "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"probe\","
                "\"version\":\"0.1.0\"}}")) {
        report(false, "initialize: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto initialize = co_await await_response(probe, 1);
    const auto* initialized = member_of(&initialize.result, "result");
    const auto* protocol_version = member_of(initialized, "protocolVersion");
    const auto server_name = as_string(member_of(member_of(initialized, "serverInfo"), "name"));
    report(initialize.responded && as_string(protocol_version).has_value(),
            "initialize: responded=" + std::to_string(initialize.responded) +
                    " protocolVersion=" + as_string(protocol_version).value_or("<none>") +
                    " serverInfo.name=" + server_name.value_or("<none>"));

    // 2. notifications/initialized (no id, no response expected)
    (void)probe.send_notification("notifications/initialized", "");

    // 3. tools/list
    if (!probe.send_request(2, "tools/list", "")) {
        report(false, "tools/list: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto tools_list = co_await await_response(probe, 2);
    const auto tool_name =
            as_string(member_of(element_of(member_of(member_of(&tools_list.result, "result"), "tools"), 0), "name"));
    report(tools_list.responded && tool_name.has_value(),
            "tools/list: responded=" + std::to_string(tools_list.responded) +
                    " first tool=" + tool_name.value_or("<none>"));

    // 4. tools/call
    if (!probe.send_request(3, "tools/call", "{\"name\":\"echo\",\"arguments\":{\"text\":\"hello\"}}")) {
        report(false, "tools/call: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto tools_call = co_await await_response(probe, 3);
    const auto echoed =
            as_string(member_of(element_of(member_of(member_of(&tools_call.result, "result"), "content"), 0), "text"));
    report(tools_call.responded && echoed == std::optional<std::string>("hello"),
            "tools/call: responded=" + std::to_string(tools_call.responded) +
                    " echoed text=" + echoed.value_or("<none>"));

    // 5. malformed frame: the server writes a non-JSON line, then the response.
    if (!probe.send_request(4, "debug/emit_garbage", "")) {
        report(false, "malformed-frame: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto garbage = co_await await_response(probe, 4);
    const bool garbage_flagged = !garbage.transport_errors.empty();
    report(garbage.responded && garbage_flagged,
            "malformed-frame: recovered=" + std::to_string(garbage.responded) + " transport-error=\"" +
                    describe_transport_errors(garbage.transport_errors) + "\"");

    // 6. valid JSON, invalid JSON-RPC message.
    if (!probe.send_request(5, "debug/emit_invalid_jsonrpc", "")) {
        report(false, "invalid-jsonrpc: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto invalid = co_await await_response(probe, 5);
    const bool invalid_flagged = !invalid.transport_errors.empty() &&
                                 invalid.transport_errors.front().find("invalid JSON-RPC") != std::string::npos;
    report(invalid.responded && invalid_flagged,
            "invalid-jsonrpc: recovered=" + std::to_string(invalid.responded) + " transport-error=\"" +
                    describe_transport_errors(invalid.transport_errors) + "\"");

    // 7. server crash mid-request: no response, stdout closes.
    if (!probe.send_request(6, "debug/crash", "")) {
        report(false, "crash-mid-request: request write failed");
        *exit_code = 1;
        co_return;
    }
    auto crash = co_await await_response(probe, 6);
    int status = 0;
    const pid_t reaped = ::waitpid(child_pid, &status, 0);
    const int wait_exit = reaped == child_pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    report(crash.closed && !crash.responded,
            "crash-mid-request: closed=" + std::to_string(crash.closed) +
                    " pending-request-failed=true child-exit=" + std::to_string(wait_exit));

    *exit_code = g_failures == 0 ? 0 : 1;
    co_return;
}

std::string drain_fd(int fd) {
    std::string captured;
    std::array<char, 4096> chunk{};
    for (;;) {
        const auto count = ::read(fd, chunk.data(), chunk.size());
        if (count > 0) {
            captured.append(chunk.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    return captured;
}

void reap_server(pid_t pid) {
    int status = 0;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return;
        }
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (::kill(pid, 0) == -1 && errno == ESRCH) {
            return;
        }
        ::usleep(10000);
    }
}

} // namespace

int main(int argc, char** argv) {
    (void)::signal(SIGPIPE, SIG_IGN);
    g_trace = std::getenv("PROBE_TRACE") != nullptr;

    const std::string script_path = argc > 1 ? argv[1] : "echo_server.py";
    auto server = spawn_server(script_path);
    if (!server) {
        std::fprintf(stderr, "probe: failed to spawn %s\n", script_path.c_str());
        return 2;
    }
    std::printf("[probe] spawned echo server pid=%d\n", static_cast<int>(server->pid));

    int exit_code = 1;
    boost::asio::io_context io;
    StdioProbe probe(io, server->stdout_read_fd, server->stdin_write_fd);
    boost::asio::co_spawn(io, run_probe(probe, server->pid, &exit_code), boost::asio::detached);
    io.run();

    // Normal shutdown: close stdin, let the server exit on EOF, then force it.
    (void)::close(server->stdin_write_fd);
    reap_server(server->pid);
    const std::string server_stderr = drain_fd(server->stderr_read_fd);
    if (!server_stderr.empty()) {
        std::printf("[probe] server stderr: %s", server_stderr.c_str());
    }
    (void)::close(server->stdout_read_fd);
    (void)::close(server->stderr_read_fd);
    ::kill(-server->pid, SIGKILL);

    std::printf("[probe] RESULT: %s (failures=%d)\n", exit_code == 0 ? "PASS" : "FAIL", g_failures);
    return exit_code;
}
