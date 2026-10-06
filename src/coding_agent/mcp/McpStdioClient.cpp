// MCP stdio transport client (spec #865, ticket #869). Newline-delimited
// compact JSON framing and the close handshake mirror pi v1.0.4
// `packages/mcp/src/transports/stdio.ts`; the launch mechanics mirror the
// in-repo `src/agent/harness/Process.cpp` (pipe2 + fork + setpgid + exec), with
// the one delta a long-lived server needs: a parent-held writable stdin pipe
// and line-by-line delivery instead of a drain to EOF.
//
// The transport is private to cch_coding_agent and is reached only through
// `McpExtensionToolSource`; nothing here is an Owner Interface.

#include "coding_agent/mcp/McpStdioClient.hpp"

#include "support/Json.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/UniqueFd.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cch::coding_agent::mcp {
namespace {

/// pi `client.ts` request timeout default (30 s); a request that gets no
/// response by then fails with a timeout instead of hanging the queue.
constexpr std::chrono::milliseconds kRequestTimeout{30000};
/// pi `close`: close stdin, wait this long for a clean exit before SIGTERM.
constexpr std::chrono::milliseconds kCloseGrace{500};
/// pi `close`: after SIGTERM wait this long before SIGKILL.
constexpr std::chrono::milliseconds kTerminateGrace{2000};

[[nodiscard]] support::Error transport_error(std::string server, std::string message, std::string cause = {}) {
    std::string summary = "MCP server '" + std::move(server) + "' " + std::move(message);
    std::string detail = summary;
    if (!cause.empty()) {
        detail += ": " + std::move(cause);
    }
    return support::make_error(support::ErrorCode::Process, std::move(summary), std::move(detail));
}

/// The error a pending request fails with when the server's stdout reaches EOF
/// or the write end closes. Message and detail both name the server and the
/// condition; the Agent's tool-failure text reads `detail`.
[[nodiscard]] support::Error closed_error(const std::string& server) {
    return support::make_error(support::ErrorCode::Process,
            "MCP server '" + server + "' connection closed",
            "MCP server '" + server + "' connection closed before responding");
}

[[nodiscard]] std::string json_or_empty(const support::JsonValue& value) {
    auto serialized = support::write_json(value);
    return serialized ? std::move(*serialized) : std::string{"<unserializable>"};
}

[[nodiscard]] support::Error json_rpc_error(const support::JsonValue& error) {
    std::string message = "returned a JSON-RPC error";
    std::string detail = json_or_empty(error);
    if (const auto* object = error.get_if<support::JsonValue::object_t>()) {
        if (const auto it = object->find("message"); it != object->end() && it->second.holds<std::string>()) {
            message = it->second.get_string();
        }
        if (const auto it = object->find("code"); it != object->end() && it->second.holds<double>()) {
            detail = "code " + std::to_string(static_cast<long long>(it->second.get_number()));
            if (const auto message_it = object->find("message");
                    message_it != object->end() && message_it->second.holds<std::string>()) {
                detail += ": " + message_it->second.get_string();
            }
        }
    }
    return support::make_error(support::ErrorCode::Process, std::move(message), std::move(detail));
}

[[nodiscard]] std::string build_request_frame(
        int id, std::string_view method, const std::optional<support::JsonValue>& params) {
    support::JsonValue request{support::JsonValue::object_t{
            {"jsonrpc", "2.0"},
            {"id", static_cast<double>(id)},
            {"method", std::string{method}},
    }};
    if (params) {
        request.get_object().emplace("params", *params);
    }
    auto serialized = support::write_json(request);
    std::string frame = serialized ? std::move(*serialized) : std::string{"{}"};
    frame.push_back('\n');
    return frame;
}

[[nodiscard]] std::string build_notification_frame(
        std::string_view method, const std::optional<support::JsonValue>& params) {
    support::JsonValue notification{support::JsonValue::object_t{
            {"jsonrpc", "2.0"},
            {"method", std::string{method}},
    }};
    if (params) {
        notification.get_object().emplace("params", *params);
    }
    auto serialized = support::write_json(notification);
    std::string frame = serialized ? std::move(*serialized) : std::string{"{}"};
    frame.push_back('\n');
    return frame;
}

/// Validate the `initialize` result like pi `validateInitializeResult`: the
/// protocol version, capabilities object, and serverInfo identity must be
/// present, so a server that cannot speak MCP fails explicitly at connect.
[[nodiscard]] support::ExpectedVoid validate_initialize_result(
        const std::string& server, const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(transport_error(server, "sent an invalid initialize result"));
    }
    const auto protocol_version = object->find("protocolVersion");
    if (protocol_version == object->end() || !protocol_version->second.holds<std::string>()) {
        return std::unexpected(transport_error(server, "initialize result is missing protocolVersion"));
    }
    const auto capabilities = object->find("capabilities");
    if (capabilities == object->end() || capabilities->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(transport_error(server, "initialize result is missing capabilities"));
    }
    const auto server_info = object->find("serverInfo");
    if (server_info == object->end() || server_info->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(transport_error(server, "initialize result is missing serverInfo"));
    }
    return {};
}

/// Child side of the launch. Runs between fork and exec only: no allocation,
/// no unwinding. A setup failure is reported to the parent through the error
/// pipe (CLOEXEC, so a successful exec closes it and the parent reads EOF).
[[noreturn]] void run_child(const std::string& command,
        const std::vector<char*>& argv,
        const std::vector<char*>& envp,
        int stdin_read,
        int stdout_write,
        int error_write) noexcept {
    auto fail = [&](int stage_error) {
        (void)::write(error_write, &stage_error, sizeof(stage_error));
        ::_exit(127);
    };
    if (::setpgid(0, 0) == -1) {
        fail(errno);
    }
    if (::dup2(stdin_read, STDIN_FILENO) == -1) {
        fail(errno);
    }
    if (::dup2(stdout_write, STDOUT_FILENO) == -1) {
        fail(errno);
    }
    // The server's stderr never affects framing (pi captures it into a bounded
    // ring; this slice discards it) and must be drained to /dev/null so a
    // chatty server cannot block on a full pipe.
    const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) {
        (void)::dup2(devnull, STDERR_FILENO);
    }
    ::execvpe(command.c_str(), argv.data(), envp.data());
    fail(errno);
    __builtin_unreachable();
}

} // namespace

McpStdioClient::McpStdioClient(boost::asio::any_io_executor executor, McpStdioServerConfig config)
    : executor_(std::move(executor)), config_(std::move(config)), stdin_pipe_(executor_), stdout_pipe_(executor_) {}

McpStdioClient::~McpStdioClient() {
    close_transport();
    teardown_process_group();
}

support::ExpectedVoid McpStdioClient::spawn() {
    if (config_.command.empty()) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "MCP server '" + config_.name + "' has no command"));
    }

    // A write to a server that has exited must fail with EPIPE rather than
    // terminate this process. The pid is the process-group leader, so the
    // shutdown handshake reaches a wrapper's descendants too.
    (void)::signal(SIGPIPE, SIG_IGN);

    int stdin_fds[2]{-1, -1};
    int stdout_fds[2]{-1, -1};
    int error_fds[2]{-1, -1};
    const auto close_pair = [](int fds[2]) {
        if (fds[0] >= 0) {
            (void)::close(fds[0]);
        }
        if (fds[1] >= 0) {
            (void)::close(fds[1]);
        }
    };
    if (::pipe2(stdin_fds, O_CLOEXEC) == -1) {
        return std::unexpected(transport_error(config_.name, "stdin pipe creation failed", std::strerror(errno)));
    }
    if (::pipe2(stdout_fds, O_CLOEXEC) == -1) {
        close_pair(stdin_fds);
        return std::unexpected(transport_error(config_.name, "stdout pipe creation failed", std::strerror(errno)));
    }
    if (::pipe2(error_fds, O_CLOEXEC) == -1) {
        close_pair(stdin_fds);
        close_pair(stdout_fds);
        return std::unexpected(transport_error(config_.name, "setup pipe creation failed", std::strerror(errno)));
    }

    std::vector<std::string> argument_storage;
    argument_storage.reserve(config_.args.size() + 1);
    argument_storage.push_back(config_.command);
    for (const auto& argument : config_.args) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(argument_storage.size() + 1);
    for (auto& argument : argument_storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    // The child environment is built here (never after fork): the inherited
    // environment with the configured entries layered over it.
    std::vector<std::string> environment_storage;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        environment_storage.emplace_back(*entry);
    }
    for (const auto& [key, value] : config_.env) {
        std::string entry = key;
        entry += '=';
        entry += value;
        const auto existing = std::find_if(
                environment_storage.begin(), environment_storage.end(), [&key](const std::string& candidate) {
                    return candidate.size() > key.size() && candidate.compare(0, key.size(), key) == 0 &&
                           candidate[key.size()] == '=';
                });
        if (existing != environment_storage.end()) {
            *existing = std::move(entry);
        } else {
            environment_storage.push_back(std::move(entry));
        }
    }
    std::vector<char*> envp;
    envp.reserve(environment_storage.size() + 1);
    for (auto& entry : environment_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    const pid_t child = ::fork();
    if (child == -1) {
        close_pair(stdin_fds);
        close_pair(stdout_fds);
        close_pair(error_fds);
        return std::unexpected(transport_error(config_.name, "fork failed", std::strerror(errno)));
    }
    if (child == 0) {
        run_child(config_.command, argv, envp, stdin_fds[0], stdout_fds[1], error_fds[1]);
    }

    (void)::close(stdin_fds[0]);
    (void)::close(stdout_fds[1]);
    (void)::close(error_fds[1]);

    boost::system::error_code assign_error;
    stdin_pipe_.assign(stdin_fds[1], assign_error);
    if (assign_error) {
        (void)::close(stdin_fds[1]);
        (void)::close(stdout_fds[0]);
        (void)::close(error_fds[0]);
        ::kill(child, SIGKILL);
        int status = 0;
        (void)::waitpid(child, &status, 0);
        return std::unexpected(transport_error(config_.name, "stdin pipe setup failed", assign_error.message()));
    }
    assign_error.clear();
    stdout_pipe_.assign(stdout_fds[0], assign_error);
    if (assign_error) {
        (void)::close(stdout_fds[0]);
        (void)::close(error_fds[0]);
        close_transport();
        ::kill(child, SIGKILL);
        int status = 0;
        (void)::waitpid(child, &status, 0);
        return std::unexpected(transport_error(config_.name, "stdout pipe setup failed", assign_error.message()));
    }

    child_pid_ = child;
    process_group_ = child;
    // The child establishes its own process group before exec; the parent
    // repeats it to close the fork/kill race (EACCES: already exec'd, ESRCH:
    // already exited).
    if (::setpgid(child, child) == -1 && errno != EACCES && errno != ESRCH) {
        const std::string detail = std::strerror(errno);
        (void)::close(error_fds[0]);
        close_transport();
        ::kill(child, SIGKILL);
        int status = 0;
        (void)::waitpid(child, &status, 0);
        child_pid_ = -1;
        return std::unexpected(transport_error(config_.name, "process group setup failed", detail));
    }

    // A successful exec atomically closes the CLOEXEC error pipe, so a
    // blocking read reports the launch outcome without a timer: EOF means the
    // server is running; a written errno means setup or exec failed.
    int setup_error = 0;
    ssize_t read_count = 0;
    do {
        read_count = ::read(error_fds[0], &setup_error, sizeof(setup_error));
    } while (read_count == -1 && errno == EINTR);
    (void)::close(error_fds[0]);
    if (read_count > 0) {
        int status = 0;
        (void)::waitpid(child, &status, 0);
        child_pid_ = -1;
        process_group_ = -1;
        close_transport();
        return std::unexpected(transport_error(config_.name, "could not be launched", std::strerror(setup_error)));
    }
    return {};
}

boost::asio::awaitable<support::Expected<std::shared_ptr<McpStdioClient>>> McpStdioClient::connect(
        McpStdioServerConfig config) {
    auto executor = co_await boost::asio::this_coro::executor;
    auto client = std::shared_ptr<McpStdioClient>(new McpStdioClient(std::move(executor), std::move(config)));
    if (auto spawned = client->spawn(); !spawned) {
        co_return std::unexpected(std::move(spawned.error()));
    }

    support::JsonValue params{support::JsonValue::object_t{
            {"protocolVersion", std::string{kMcpProtocolVersion}},
            {"capabilities", support::JsonValue::object_t{}},
            {"clientInfo",
                    support::JsonValue::object_t{
                            {"name", std::string{kMcpClientName}},
                            {"version", std::string{kMcpClientVersion}},
                    }},
    }};
    auto initialized = co_await support::detail::await_async_result(client->request("initialize", std::move(params)));
    if (!initialized) {
        co_return std::unexpected(std::move(initialized.error()));
    }
    if (auto valid = validate_initialize_result(client->config_.name, *initialized); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    client->notify("notifications/initialized");
    co_return client;
}

support::AsyncResult<support::JsonValue> McpStdioClient::request(
        std::string method, std::optional<support::JsonValue> params) {
    return support::AsyncResult<support::JsonValue>{support::AsyncProducer<support::JsonValue, support::Error>{
            [self = shared_from_this(), method = std::move(method), params = std::move(params)](
                    support::AsyncCompletion<support::JsonValue, support::Error> completion) mutable noexcept {
                if (self->closed_) {
                    completion(std::unexpected(closed_error(self->config_.name)));
                    return;
                }
                const int id = self->next_id_++;
                self->enqueue_frame(build_request_frame(id, method, params), id, std::move(completion));
            }}};
}

void McpStdioClient::notify(std::string method, std::optional<support::JsonValue> params) {
    if (closed_) {
        return;
    }
    enqueue_frame(build_notification_frame(method, params), 0, std::nullopt);
}

void McpStdioClient::enqueue_frame(std::string frame,
        int id,
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion) {
    auto item = std::make_unique<QueuedFrame>();
    item->frame = std::move(frame);
    item->id = id;
    item->completion = std::move(completion);
    item->is_notification = !item->completion.has_value();
    queue_.push_back(std::move(item));
    if (!pumping_) {
        pumping_ = true;
        // Capture the shared owner in the coroutine frame at creation, so the
        // client outlives the pump even if every external reference drops.
        boost::asio::co_spawn(
                executor_,
                [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->pump(); },
                boost::asio::detached);
    }
}

boost::asio::awaitable<void> McpStdioClient::pump() {
    while (!queue_.empty()) {
        auto item = std::move(queue_.front());
        queue_.pop_front();
        if (closed_) {
            support::Expected<support::JsonValue> outcome = std::unexpected(closed_error(config_.name));
            complete_frame(*item, std::move(outcome));
            continue;
        }
        if (auto write_error = co_await write_frame(item->frame)) {
            closed_ = true;
            support::Expected<support::JsonValue> outcome = std::unexpected(std::move(*write_error));
            complete_frame(*item, std::move(outcome));
            continue;
        }
        if (item->is_notification) {
            continue;
        }
        auto outcome = co_await await_response(item->id);
        complete_frame(*item, std::move(outcome));
    }
    pumping_ = false;
}

void McpStdioClient::complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome) {
    if (frame.completion.has_value()) {
        (*frame.completion)(std::move(outcome));
    }
}

boost::asio::awaitable<std::optional<support::Error>> McpStdioClient::write_frame(std::string_view frame) {
    const auto [error, written] = co_await boost::asio::async_write(stdin_pipe_,
            boost::asio::buffer(frame.data(), frame.size()),
            boost::asio::as_tuple(boost::asio::use_awaitable));
    (void)written;
    if (error) {
        co_return transport_error(config_.name, "stdin write failed", error.message());
    }
    co_return std::nullopt;
}

boost::asio::awaitable<std::optional<std::string>> McpStdioClient::read_line() {
    last_read_timed_out_ = false;
    for (;;) {
        if (!discarding_oversize_frame_) {
            const auto newline = read_buffer_.find('\n');
            if (newline != std::string::npos) {
                std::string line = read_buffer_.substr(0, newline);
                read_buffer_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }
                co_return line;
            }
            if (read_buffer_.size() > kMcpMaxMessageBytes) {
                // pi drops an over-size message as a transport error and keeps
                // serving; discard the partial line and resynchronize on the
                // next newline.
                read_buffer_.clear();
                discarding_oversize_frame_ = true;
            }
        }

        const auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer timer(executor);
        auto timed_out = std::make_shared<bool>(false);
        timer.expires_after(kRequestTimeout);
        timer.async_wait([this, timed_out](const boost::system::error_code& error) {
            if (!error) {
                *timed_out = true;
                boost::system::error_code ignored;
                stdout_pipe_.cancel(ignored);
            }
        });

        std::array<char, 4096> chunk{};
        const auto [error, count] = co_await stdout_pipe_.async_read_some(
                boost::asio::buffer(chunk), boost::asio::as_tuple(boost::asio::use_awaitable));
        timer.cancel();
        if (error == boost::asio::error::eof) {
            closed_ = true;
            co_return std::nullopt;
        }
        if (error) {
            if (*timed_out) {
                last_read_timed_out_ = true;
                co_return std::nullopt;
            }
            closed_ = true;
            co_return std::nullopt;
        }
        if (count == 0) {
            continue;
        }
        if (discarding_oversize_frame_) {
            const auto newline = std::string_view{chunk.data(), count}.find('\n');
            if (newline == std::string_view::npos) {
                continue;
            }
            read_buffer_.assign(chunk.data() + newline + 1, count - newline - 1);
            discarding_oversize_frame_ = false;
            continue;
        }
        read_buffer_.append(chunk.data(), count);
    }
}

boost::asio::awaitable<support::Expected<support::JsonValue>> McpStdioClient::await_response(int id) {
    for (;;) {
        auto line = co_await read_line();
        if (!line) {
            if (last_read_timed_out_) {
                co_return std::unexpected(support::make_error(
                        support::ErrorCode::Timeout, "MCP server '" + config_.name + "' did not respond in time"));
            }
            co_return std::unexpected(closed_error(config_.name));
        }
        auto parsed = support::read_json(*line);
        if (!parsed) {
            // Malformed JSON is a recoverable transport error: the pending
            // request keeps waiting for its own response (pi `handleStdout`).
            continue;
        }
        const auto* object = parsed->get_if<support::JsonValue::object_t>();
        if (object == nullptr) {
            continue;
        }
        const auto version = object->find("jsonrpc");
        if (version == object->end() || !version->second.holds<std::string>() ||
                version->second.get_string() != "2.0") {
            continue;
        }
        const auto response_id = object->find("id");
        if (response_id == object->end() || !response_id->second.holds<double>()) {
            continue;
        }
        if (static_cast<int>(response_id->second.get_number()) != id) {
            continue;
        }
        if (const auto error = object->find("error"); error != object->end()) {
            co_return std::unexpected(json_rpc_error(error->second));
        }
        const auto result = object->find("result");
        if (result == object->end()) {
            co_return std::unexpected(transport_error(config_.name, "sent a response without a result"));
        }
        co_return result->second;
    }
}

void McpStdioClient::close_transport() noexcept {
    boost::system::error_code ignored;
    if (stdin_pipe_.is_open()) {
        stdin_pipe_.close(ignored);
    }
    if (stdout_pipe_.is_open()) {
        stdout_pipe_.close(ignored);
    }
}

void McpStdioClient::teardown_process_group() noexcept {
    if (child_pid_ <= 0) {
        return;
    }
    int status = 0;
    const auto reap_within = [&](std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            const pid_t reaped = ::waitpid(child_pid_, &status, WNOHANG);
            if (reaped == child_pid_) {
                return true;
            }
            if (reaped == -1 && errno != EINTR) {
                return true; // ECHILD: already reaped
            }
            if (reaped == -1 && errno == EINTR) {
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            ::usleep(1000);
        }
    };

    // pi `close` handshake: stdin is already closed (EOF); grace; SIGTERM;
    // grace; SIGKILL — against the process group so a wrapper's descendants do
    // not survive.
    if (!reap_within(kCloseGrace)) {
        if (process_group_ > 0) {
            (void)::killpg(process_group_, SIGTERM);
        }
        (void)::kill(child_pid_, SIGTERM);
        if (!reap_within(kTerminateGrace)) {
            (void)::killpg(process_group_, SIGKILL);
            (void)::kill(child_pid_, SIGKILL);
            (void)::waitpid(child_pid_, &status, 0);
        }
    }
    if (process_group_ > 0) {
        (void)::killpg(process_group_, SIGKILL);
    }
    child_pid_ = -1;
}

} // namespace cch::coding_agent::mcp
