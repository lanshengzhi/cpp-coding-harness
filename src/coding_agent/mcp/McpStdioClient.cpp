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

#include "coding_agent/mcp/McpProtocol.hpp"

#include "support/Json.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/UniqueFd.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
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
#include <functional>
#include <set>
#include <stop_token>
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

/// One newline-delimited frame: the shared JSON-RPC body plus the stdio
/// delimiter (pi `stdio.ts` writes one compact object per `\n`-terminated
/// line).
[[nodiscard]] std::string newline_frame(std::string body) {
    body.push_back('\n');
    return body;
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
    if (auto valid = detail::validate_initialize_result(client->config_.name, *initialized); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    client->notify("notifications/initialized");
    co_return client;
}

support::AsyncResult<support::JsonValue> McpStdioClient::request(
        std::string method, std::optional<support::JsonValue> params, std::stop_token stop_token) {
    return support::AsyncResult<support::JsonValue>{support::AsyncProducer<support::JsonValue, support::Error>{
            [self = shared_from_this(), method = std::move(method), params = std::move(params), stop_token](
                    support::AsyncCompletion<support::JsonValue, support::Error> completion) mutable noexcept {
                if (self->closed_) {
                    completion(std::unexpected(closed_error(self->config_.name)));
                    return;
                }
                if (stop_token.stop_requested()) {
                    completion(std::unexpected(detail::cancelled_error(self->config_.name)));
                    return;
                }
                const int id = self->next_id_++;
                // The stop callback bridges the caller's cancellation into the
                // serialized domain: it only posts, so it is safe to run on
                // whichever thread requests stop. A weak reference keeps the
                // registration from extending the client's lifetime.
                if (stop_token.stop_possible()) {
                    std::weak_ptr<McpStdioClient> weak = self;
                    self->stop_registrations_[id] =
                            std::make_unique<StopRegistration>(stop_token, [weak, id]() noexcept {
                                if (auto client = weak.lock()) {
                                    boost::asio::post(
                                            client->executor_, [client, id]() { client->cancel_request(id); });
                                }
                            });
                }
                self->enqueue_frame(
                        newline_frame(detail::build_request_body(id, method, params)), id, std::move(completion));
            }}};
}

void McpStdioClient::cancel_request(int id) {
    if (closed_ || !request_pending(id)) {
        return;
    }
    cancelled_.insert(id);
    // Wake the in-flight response read so `await_response` can send the
    // cancellation notification and complete the caller without waiting for a
    // response that may never come.
    if (awaiting_id_ == id) {
        boost::system::error_code ignored;
        stdout_pipe_.cancel(ignored);
    }
}

bool McpStdioClient::request_pending(int id) const noexcept {
    if (current_id_ == id) {
        return true;
    }
    for (const auto& frame : queue_) {
        if (!frame->is_notification && frame->id == id) {
            return true;
        }
    }
    return false;
}

boost::asio::awaitable<void> McpStdioClient::write_cancellation(int id) {
    support::JsonValue params{support::JsonValue::object_t{
            {"requestId", static_cast<double>(id)},
            {"reason", std::string{"cancelled"}},
    }};
    // The write is awaited so the bytes reach the server before the caller is
    // failed; a write failure is ignored because the caller already intended
    // to leave the request (pi `cancelPending`).
    (void)co_await write_frame(newline_frame(detail::build_notification_body("notifications/cancelled", params)));
}

void McpStdioClient::notify(std::string method, std::optional<support::JsonValue> params) {
    if (closed_) {
        return;
    }
    enqueue_frame(newline_frame(detail::build_notification_body(method, params)), 0, std::nullopt);
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
        if (!item->is_notification && cancelled_.contains(item->id)) {
            // Cancelled while queued: the server never saw the request, so
            // there is nothing to notify and the caller fails immediately.
            support::Expected<support::JsonValue> outcome = std::unexpected(detail::cancelled_error(config_.name));
            complete_frame(*item, std::move(outcome));
            continue;
        }
        if (!item->is_notification) {
            current_id_ = item->id;
        }
        if (auto write_error = co_await write_frame(item->frame)) {
            closed_ = true;
            current_id_ = 0;
            support::Expected<support::JsonValue> outcome = std::unexpected(std::move(*write_error));
            complete_frame(*item, std::move(outcome));
            continue;
        }
        if (item->is_notification) {
            continue;
        }
        awaiting_id_ = item->id;
        auto outcome = co_await await_response(item->id);
        awaiting_id_ = 0;
        current_id_ = 0;
        complete_frame(*item, std::move(outcome));
    }
    pumping_ = false;
}

void McpStdioClient::complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome) {
    if (!frame.completion.has_value()) {
        return;
    }
    // The request has settled: its cancellation state and stop registration
    // must not outlive it.
    stop_registrations_.erase(frame.id);
    cancelled_.erase(frame.id);
    (*frame.completion)(std::move(outcome));
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
    last_read_woken_ = false;
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
        if (config_.request_timeout > std::chrono::milliseconds::zero()) {
            timer.expires_after(config_.request_timeout);
            timer.async_wait([this, timed_out](const boost::system::error_code& error) {
                if (!error) {
                    *timed_out = true;
                    boost::system::error_code ignored;
                    stdout_pipe_.cancel(ignored);
                }
            });
        }

        std::array<char, 4096> chunk{};
        const auto [error, count] = co_await stdout_pipe_.async_read_some(
                boost::asio::buffer(chunk), boost::asio::as_tuple(boost::asio::use_awaitable));
        timer.cancel();
        if (error == boost::asio::error::eof) {
            closed_ = true;
            co_return std::nullopt;
        }
        if (error == boost::asio::error::operation_aborted) {
            // Either the request deadline fired or a cancellation woke the
            // read; both leave the connection usable.
            if (*timed_out) {
                last_read_timed_out_ = true;
            } else {
                last_read_woken_ = true;
            }
            co_return std::nullopt;
        }
        if (error) {
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
        if (cancelled_.contains(id)) {
            co_await write_cancellation(id);
            co_return std::unexpected(detail::cancelled_error(config_.name));
        }
        auto line = co_await read_line();
        if (!line) {
            if (cancelled_.contains(id)) {
                co_await write_cancellation(id);
                co_return std::unexpected(detail::cancelled_error(config_.name));
            }
            if (last_read_woken_) {
                // A wake with no cancellation recorded is spurious; keep
                // waiting for this request's own response.
                continue;
            }
            if (last_read_timed_out_) {
                // pi cancels a timed-out request too, so the server stops
                // working on a response nobody will read.
                co_await write_cancellation(id);
                co_return std::unexpected(detail::timeout_error(config_.name, config_.request_timeout));
            }
            co_return std::unexpected(closed_error(config_.name));
        }
        auto parsed = support::read_json(*line);
        if (!parsed) {
            // Malformed JSON is a recoverable transport error: the pending
            // request keeps waiting for its own response (pi `handleStdout`).
            continue;
        }
        auto matched = detail::response_for_id(*parsed, id);
        if (!matched) {
            co_return std::unexpected(std::move(matched.error()));
        }
        if (matched->has_value()) {
            co_return std::move(**matched);
        }
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
