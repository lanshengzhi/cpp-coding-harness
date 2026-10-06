#pragma once

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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// The MCP protocol version Pike requests on a stdio connection (pi v1.0.4's
/// default for the newline-delimited stdio transport).
inline constexpr std::string_view kMcpProtocolVersion = "2025-06-18";

/// Client identity sent in the `initialize` handshake.
inline constexpr std::string_view kMcpClientName = "pike";
inline constexpr std::string_view kMcpClientVersion = "0.1.0";

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
/// timeout or connection close), matching pi's `handleStdout`.
class McpStdioClient final : public std::enable_shared_from_this<McpStdioClient> {
public:
    /// Launch `config`, run the MCP `initialize` handshake, and return the
    /// connected client. A missing executable, an empty command, or an
    /// invalid handshake response is reported through the shared error channel
    /// — there is no fallback server and no silent empty connection.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::shared_ptr<McpStdioClient>>> connect(
            McpStdioServerConfig config);

    McpStdioClient(const McpStdioClient&) = delete;
    McpStdioClient& operator=(const McpStdioClient&) = delete;
    ~McpStdioClient();

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error (invalid response,
    /// connection closed, timeout).
    [[nodiscard]] support::AsyncResult<support::JsonValue> request(
            std::string method, std::optional<support::JsonValue> params = std::nullopt);

    /// One JSON-RPC notification (no `id`, no response). Ordered against
    /// requests through the same queue.
    void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt);

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] const std::string& server_name() const noexcept { return config_.name; }

private:
    McpStdioClient(boost::asio::any_io_executor executor, McpStdioServerConfig config);

    /// One queued frame. A notification carries no `id` and no completion.
    struct QueuedFrame {
        std::string frame;
        int id{0};
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion;
        bool is_notification{false};
    };

    [[nodiscard]] support::ExpectedVoid spawn();
    /// Serve the queued frames in order until the queue drains; one pump runs
    /// at a time and is restarted by the next enqueue.
    [[nodiscard]] boost::asio::awaitable<void> pump();
    [[nodiscard]] boost::asio::awaitable<std::optional<support::Error>> write_frame(std::string_view frame);
    /// Read frames until the response for `id` arrives. Malformed/invalid
    /// frames are skipped as recoverable transport errors.
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> await_response(int id);
    /// Read one newline-delimited frame; nullopt on timeout, EOF, or read
    /// error (`closed_` is set for EOF/error).
    [[nodiscard]] boost::asio::awaitable<std::optional<std::string>> read_line();

    void enqueue_frame(std::string frame,
            int id,
            std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion);
    void complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome);
    void teardown_process_group() noexcept;
    void close_transport() noexcept;

    boost::asio::any_io_executor executor_;
    McpStdioServerConfig config_;
    boost::asio::posix::stream_descriptor stdin_pipe_;
    boost::asio::posix::stream_descriptor stdout_pipe_;
    std::deque<std::unique_ptr<QueuedFrame>> queue_;
    std::string read_buffer_;
    int next_id_{1};
    int child_pid_{-1};
    int process_group_{-1};
    bool pumping_{false};
    bool closed_{false};
    /// The last `read_line` nullopt came from the deadline rather than EOF.
    bool last_read_timed_out_{false};
    /// An over-size frame has no `\n` yet; skip bytes until the next newline.
    bool discarding_oversize_frame_{false};
};

} // namespace cch::coding_agent::mcp
