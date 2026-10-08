#pragma once

#include "coding_agent/mcp/McpProtocol.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// The largest single frame accepted from the server (pi
/// `DEFAULT_MAX_MESSAGE_BYTES` = 16 MiB). An over-size frame is dropped as a
/// transport error rather than buffered without bound.
inline constexpr std::size_t kMcpMaxMessageBytes = 16u * 1024u * 1024u;

/// Long-lived MCP server connection over newline-delimited compact JSON (pi
/// `packages/mcp/src/transports/stdio.ts`): one child process group, one
/// stdin/stdout pipe pair, one JSON-RPC message per `\n`-terminated line with
/// a trailing `\r` stripped and blank lines skipped. One client owns the child
/// process; the destructor tears the process group down (close stdin → grace →
/// SIGTERM → grace → SIGKILL).
///
/// Requests and notifications are queued and served one at a time, so the one
/// transport pair is never written or read concurrently regardless of the tool
/// concurrency the Agent applies. A malformed or over-size frame is a
/// recoverable transport error: the pending request keeps waiting (it fails on
/// timeout or connection close), matching pi's `handleStdout`. After the server
/// dies, the in-flight request fails explicitly (never a silent hang) and the
/// next call reconnects the server (pi `connection.reconnect()`), so a dead
/// server does not end the session.
class McpStdioClient final : public McpServerConnection, public std::enable_shared_from_this<McpStdioClient> {
public:
    /// Launch `config`, run the MCP `initialize` handshake, and return the
    /// connected client. A missing executable, an empty command, or an
    /// invalid handshake response is reported through the shared error channel
    /// — there is no fallback server and no silent empty connection.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::shared_ptr<McpStdioClient>>> connect(
            McpStdioServerConfig config);

    /// Construction passkey (§7.7): `std::make_shared` cannot reach a private
    /// constructor, so construction goes through the public constructor below,
    /// whose key only a member of this class can name. `connect` is the sole
    /// caller; the connection itself is still built behind that factory.
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class McpStdioClient;
    };
    McpStdioClient(ConstructionKey, boost::asio::any_io_executor executor, McpStdioServerConfig config);

    McpStdioClient(const McpStdioClient&) = delete;
    McpStdioClient& operator=(const McpStdioClient&) = delete;
    ~McpStdioClient();

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error (invalid response,
    /// connection closed, timeout). `stop_token` is the caller's cancellation
    /// source (ADR 0020): requesting stop sends `notifications/cancelled` for
    /// this request id and completes with a cancellation error once that
    /// notification has been written, so the server observes the cancellation
    /// before the call returns.
    [[nodiscard]] support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            RequestOptions options = {}) override;

    /// One JSON-RPC notification (no `id`, no response). Ordered against
    /// requests through the same queue.
    void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt) override;

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] const std::string& server_name() const noexcept override { return config_.name; }
    /// pi `initialize` result `instructions`, when the server sent them: the
    /// `mcp_servers` prompt section's summary fallback for a server whose
    /// entry carries no description.
    [[nodiscard]] const std::optional<std::string>& server_instructions() const noexcept {
        return server_instructions_;
    }

private:
    /// One queued frame. A notification carries no `id` and no completion.
    struct QueuedFrame {
        std::string frame;
        int id{0};
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion;
        bool is_notification{false};
        /// pi `onProgress` of this request, installed while its response is
        /// awaited (pi `requestInternal`/`armTimeout`).
        ProgressCallback on_progress{};
    };

    [[nodiscard]] support::ExpectedVoid spawn();
    /// pi `connection.reconnect()`: tear down the dead child, launch a fresh
    /// one, and re-run the `initialize` handshake. Returns the reconnect
    /// failure through the shared error channel, leaving the client closed so
    /// the next call tries once more.
    [[nodiscard]] boost::asio::awaitable<std::optional<support::Error>> reconnect_transport();
    /// Serve the queued frames in order until the queue drains; one pump runs
    /// at a time and is restarted by the next enqueue.
    [[nodiscard]] boost::asio::awaitable<void> pump();
    [[nodiscard]] boost::asio::awaitable<std::optional<support::Error>> write_frame(std::string_view frame);
    /// Read frames until the response for `id` arrives. Malformed/invalid
    /// frames are skipped as recoverable transport errors.
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> await_response(int id);
    /// Read one newline-delimited frame; nullopt on timeout, EOF, wake, or
    /// read error. `last_read_timed_out_` is set for the deadline, and
    /// `last_read_woken_` for a cancellation-driven wake (see
    /// `cancel_request`); `closed_` is set for EOF or an unexpected error.
    [[nodiscard]] boost::asio::awaitable<std::optional<std::string>> read_line();

    /// Resolve a cancellation request for `id` (posted from the stop
    /// callback). Marks the id cancelled and wakes the in-flight response
    /// wait so `await_response` sends `notifications/cancelled` and completes
    /// the caller without waiting for a server response.
    void cancel_request(int id);
    /// Whether `id` is still queued or being served (so a late cancellation is
    /// ignored once the request has already completed).
    [[nodiscard]] bool request_pending(int id) const noexcept;
    /// Write the `notifications/cancelled` frame for `id`. The write error is
    /// ignored: the caller's cancellation outcome is the caller's intent.
    [[nodiscard]] boost::asio::awaitable<void> write_cancellation(int id);

    void enqueue_frame(std::string frame,
            int id,
            std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion,
            ProgressCallback on_progress = {});
    void complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome);
    void teardown_process_group() noexcept;
    void close_transport() noexcept;

    /// Keeps one request's stop callback alive until the request completes; the
    /// callback holds a weak client reference, so the registration never keeps
    /// the client alive.
    using StopRegistration = std::stop_callback<std::function<void()>>;

    boost::asio::any_io_executor executor_;
    McpStdioServerConfig config_;
    boost::asio::posix::stream_descriptor stdin_pipe_;
    boost::asio::posix::stream_descriptor stdout_pipe_;
    std::deque<std::unique_ptr<QueuedFrame>> queue_;
    /// Request ids a caller cancelled while they were still pending. Entries
    /// live only while the request is pending.
    std::set<int> cancelled_;
    std::map<int, std::unique_ptr<StopRegistration>> stop_registrations_;
    std::string read_buffer_;
    int next_id_{1};
    int child_pid_{-1};
    int process_group_{-1};
    /// pi `initialize` result `instructions` (see the accessor).
    std::optional<std::string> server_instructions_;
    /// The request id whose frame the pump is currently writing or awaiting;
    /// 0 when the pump is idle. `awaiting_id_` is set only while a response
    /// read is outstanding, so a cancellation wake targets the right read.
    int current_id_{0};
    int awaiting_id_{0};
    /// pi `onProgress` of the in-flight request: the pump serves one request
    /// at a time, so a single slot is the whole registration. Set while its
    /// response is awaited; a matching `notifications/progress` delivers to it
    /// and the next read restarts the deadline (pi `armTimeout` re-arm).
    ProgressCallback progress_handler_;
    bool pumping_{false};
    bool closed_{false};
    /// The last `read_line` nullopt came from the deadline rather than EOF.
    bool last_read_timed_out_{false};
    /// The last `read_line` nullopt was a cancellation wake, not EOF.
    bool last_read_woken_{false};
    /// An over-size frame has no `\n` yet; skip bytes until the next newline.
    bool discarding_oversize_frame_{false};
};

} // namespace cch::coding_agent::mcp
