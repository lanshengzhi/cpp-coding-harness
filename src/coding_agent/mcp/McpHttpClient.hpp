#pragma once

#include "ai/providers/StreamTransport.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// Long-lived MCP server connection over the streamable HTTP transport (pi
/// `packages/mcp/src/transports/streamable-http.ts`): one JSON-RPC POST per
/// request, an `application/json` or `text/event-stream` response, and an
/// `mcp-session-id` captured from the server and echoed on later requests.
/// The HTTPS round trip reuses the existing outbound client transport
/// (`ai::providers::StreamTransport`, ADR 0054) — this client owns only the
/// MCP protocol on top of it and opens no second HTTP stack.
///
/// requests and notifications are queued and served one at a time, so the one
/// transport is never driven concurrently regardless of the tool concurrency
/// the Agent applies, matching `McpStdioClient`. A response that is not a
/// JSON-RPC reply for the pending request fails that request explicitly; the
/// client never treats a 200 with a non-MCP body as success.
class McpHttpClient final : public McpServerConnection, public std::enable_shared_from_this<McpHttpClient> {
public:
    /// Connect to `config` (TLS-only, ADR 0054), run the MCP `initialize`
    /// handshake over the injected HTTPS transport, and return the connected
    /// client. A non-TLS URL, a network failure, a non-2xx status, or an
    /// invalid handshake response is reported through the shared error channel
    /// — there is no plaintext fallback and no silent empty connection.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::shared_ptr<McpHttpClient>>> connect(
            McpHttpServerConfig config, std::shared_ptr<ai::providers::StreamTransport> transport);

    McpHttpClient(const McpHttpClient&) = delete;
    McpHttpClient& operator=(const McpHttpClient&) = delete;

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error (HTTP status, unsupported
    /// content type, malformed body).
    [[nodiscard]] support::AsyncResult<support::JsonValue> request(
            std::string method, std::optional<support::JsonValue> params = std::nullopt) override;

    /// One JSON-RPC notification (no `id`, no response). Ordered against
    /// requests through the same queue.
    void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt) override;

    [[nodiscard]] const std::string& server_name() const noexcept override { return config_.name; }

private:
    McpHttpClient(boost::asio::any_io_executor executor,
            McpHttpServerConfig config,
            std::shared_ptr<ai::providers::StreamTransport> transport);

    /// One queued request body. A notification carries no `id` and no
    /// completion.
    struct QueuedFrame {
        std::string frame;
        int id{0};
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion;
        bool is_notification{false};
    };

    /// Serve the queued frames in order until the queue drains; one pump runs
    /// at a time and is restarted by the next enqueue.
    [[nodiscard]] boost::asio::awaitable<void> pump();
    /// POST one request body and interpret the response for `id`.
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> send_request(
            std::string_view frame, int id);
    /// POST one notification body; any 2xx is success and the body is ignored.
    [[nodiscard]] boost::asio::awaitable<std::optional<support::Error>> send_notification(std::string_view frame);
    /// One HTTPS POST through the reused transport, buffering the full
    /// response body (JSON or SSE).
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::providers::StreamResponse>> post(std::string_view body);

    [[nodiscard]] std::map<std::string, std::string> request_headers() const;
    void capture_session(const ai::providers::StreamResponse& response);
    [[nodiscard]] support::Expected<support::JsonValue> interpret_response(
            int id, const ai::providers::StreamResponse& response) const;

    void enqueue_frame(std::string frame,
            int id,
            std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion);
    void complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome);

    boost::asio::any_io_executor executor_;
    McpHttpServerConfig config_;
    std::shared_ptr<ai::providers::StreamTransport> transport_;
    std::deque<std::unique_ptr<QueuedFrame>> queue_;
    std::string session_id_;
    std::string protocol_version_;
    int next_id_{1};
    bool pumping_{false};
};

} // namespace cch::coding_agent::mcp
