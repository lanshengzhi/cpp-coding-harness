#pragma once

#include "ai/providers/StreamTransport.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"
#include "coding_agent/mcp/McpServerConnection.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::mcp {

/// The server-to-client GET stream's reconnect policy (pi
/// `StreamableHttpReconnectOptions` + `openGetStream`). The defaults are pi's
/// `DEFAULT_RECONNECT_INITIAL_DELAY_MS` 1000, `DEFAULT_RECONNECT_MAX_DELAY_MS`
/// 30000, and `DEFAULT_RECONNECT_MAX_RETRIES` 5.
struct McpHttpGetStreamOptions {
    /// pi `openGetStream: false`: do not open the stream at all.
    bool enabled{true};
    /// pi `initialDelayMs`: delay before the first reconnect attempt, unless
    /// the server's SSE `retry:` field overrides it.
    std::chrono::milliseconds initial_delay{1000};
    /// pi `maxDelayMs`: upper bound for the exponential backoff.
    std::chrono::milliseconds max_delay{30000};
    /// pi `maxRetries`: consecutive failed attempts before the stream is
    /// reported dropped.
    int max_retries{5};
};

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
    /// client. When `request_auth` is present, every JSON-RPC POST resolves its
    /// headers at request time and attaches them; a missing credential or a
    /// failed refresh fails the request explicitly. A non-TLS URL, a network
    /// failure, a non-2xx status, or an invalid handshake response is reported
    /// through the shared error channel — there is no plaintext fallback and no
    /// silent empty connection.
    [[nodiscard]] static boost::asio::awaitable<support::Expected<std::shared_ptr<McpHttpClient>>> connect(
            McpHttpServerConfig config,
            std::shared_ptr<ai::providers::StreamTransport> transport,
            std::shared_ptr<McpRequestAuthSource> request_auth = nullptr,
            McpHttpGetStreamOptions get_stream_options = {});

    /// Construction passkey (§7.7): `std::make_shared` cannot reach a private
    /// constructor, so construction goes through the public constructor below,
    /// whose key only a member of this class can name. `connect` is the sole
    /// caller.
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class McpHttpClient;
    };
    McpHttpClient(ConstructionKey,
            boost::asio::any_io_executor executor,
            McpHttpServerConfig config,
            std::shared_ptr<ai::providers::StreamTransport> transport,
            std::shared_ptr<McpRequestAuthSource> request_auth,
            McpHttpGetStreamOptions get_stream_options);

    McpHttpClient(const McpHttpClient&) = delete;
    McpHttpClient& operator=(const McpHttpClient&) = delete;

    /// One JSON-RPC request. Completes with the response `result`, the
    /// server's JSON-RPC error, or a transport error (HTTP status, unsupported
    /// content type, malformed body). `stop_token` is the caller's
    /// cancellation source (ADR 0020): it is carried on the outbound
    /// `StreamRequest`, so aborting the Agent Turn cancels the HTTPS request
    /// and fails the call with a cancellation error.
    [[nodiscard]] support::AsyncResult<support::JsonValue> request(std::string method,
            std::optional<support::JsonValue> params = std::nullopt,
            std::stop_token stop_token = {}) override;

    /// One JSON-RPC notification (no `id`, no response). Ordered against
    /// requests through the same queue.
    void notify(std::string method, std::optional<support::JsonValue> params = std::nullopt) override;

    /// One server-to-client notification received on the GET stream (pi
    /// `TransportEvents`'s `message` for notifications), dispatched on the
    /// client's executor. `notifications/message` logging and the
    /// `.../list_changed` re-list hooks consume this dispatch; the re-list
    /// itself is not implemented here. `params` is an empty object when the
    /// notification carries none.
    using NotificationListener = std::function<void(std::string_view method, const support::JsonValue& params)>;
    void set_notification_listener(NotificationListener listener);

    /// One server-to-client stream failure (pi `TransportEvents`'s `error`):
    /// an exhausted reconnect reports "MCP server-to-client stream dropped and
    /// could not be reopened", and a non-retryable open failure reports the
    /// HTTP error. `close()` reports nothing.
    using ErrorListener = std::function<void(const support::Error& error)>;
    void set_error_listener(ErrorListener listener);

    /// One server-to-client request handler (pi `setRequestHandler`): the
    /// handler answers a JSON-RPC request the server sends on the GET stream.
    /// `ping` is installed by default and returns `{}`; a method with no
    /// handler is answered with `-32601 "Method not found: <method>"`.
    /// `stop_token` is requested when the server sends
    /// `notifications/cancelled` for the in-flight request. The handler's
    /// error becomes the response's `-32603` JSON-RPC error.
    using ServerRequestHandler = std::function<boost::asio::awaitable<support::Expected<support::JsonValue>>(
            const support::JsonValue& params, std::stop_token stop_token)>;
    void set_request_handler(std::string method, ServerRequestHandler handler);

    /// pi `StreamableHttpTransport.close()`: abort the server-to-client GET
    /// stream and end the HTTP session with a best-effort `DELETE` of the
    /// session URL under a 1 s timeout. Idempotent; safe to call without a
    /// session id.
    void close() noexcept;

    [[nodiscard]] const std::string& server_name() const noexcept override { return config_.name; }

private:
    /// One queued request body. A notification carries no `id` and no
    /// completion.
    struct QueuedFrame {
        std::string frame;
        int id{0};
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion;
        bool is_notification{false};
        std::stop_token stop_token{};
        /// pi `cancellable`: `method !== "initialize"`. A cancellable request
        /// that is aborted or times out emits `notifications/cancelled` (pi
        /// `cancelPending`).
        bool cancellable{false};
    };

    /// Serve the queued frames in order until the queue drains; one pump runs
    /// at a time and is restarted by the next enqueue.
    [[nodiscard]] boost::asio::awaitable<void> pump();
    /// POST one request body and interpret the response for `id`; `stop_token`
    /// cancels the in-flight HTTPS request. A `cancellable` request that the
    /// stop token aborts, or that times out, emits `notifications/cancelled`
    /// (pi `cancelPending`) before completing.
    [[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> send_request(
            std::string_view frame, int id, std::stop_token stop_token, bool cancellable);
    /// POST one notification body; any 2xx is success and the body is ignored.
    [[nodiscard]] boost::asio::awaitable<std::optional<support::Error>> send_notification(std::string_view frame);
    /// Tell the server that `id` is cancelled (pi `cancelPending`'s
    /// `notifications/cancelled`), carrying the pi reason string. Best-effort:
    /// a failed notification send never fails the cancelled request.
    [[nodiscard]] boost::asio::awaitable<void> notify_cancelled(int id, std::string reason);
    /// One HTTPS POST through the reused transport, buffering the full
    /// response body (JSON or SSE). `stop_token` is carried on the request so
    /// the transport can cancel it.
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::providers::StreamResponse>> post(
            std::string_view body, std::stop_token stop_token);

    /// The disposition of one server-to-client GET stream attempt (pi
    /// `runGetStream`'s loop body).
    enum class GetStreamAttempt {
        /// HTTP 405: the server offers no GET stream; the feature is absent.
        Absent,
        /// The stream ended (cleanly or by server close); reconnect.
        Ended,
        /// A transient open/read failure worth retrying.
        RetryableFailure,
        /// A non-retryable failure; report it and stop.
        FatalFailure,
    };
    /// Reconnect bookkeeping carried across attempts (pi `StreamCursor`).
    struct GetStreamCursor {
        std::optional<std::string> last_event_id;
        std::optional<int> retry_ms;
        /// Whether the stream delivered any event since it was (re)opened.
        bool received{false};
    };

    /// Open the GET stream and consume it to its end (or the first read
    /// failure), dispatching server-to-client requests and notifications. The
    /// SSE `id`/`retry` fields update `cursor` so a resume echoes
    /// `last-event-id` and honors a server delay override.
    [[nodiscard]] boost::asio::awaitable<std::pair<GetStreamAttempt, support::Error>> consume_get_stream(
            GetStreamCursor& cursor);
    /// pi `runGetStream`: keep the server-to-client stream open, reconnecting
    /// with backoff when it drops; a stream that delivered an event or stayed
    /// up longer than `maxDelay` resets the attempt counter. Exhausted retries
    /// report "MCP server-to-client stream dropped and could not be reopened".
    [[nodiscard]] boost::asio::awaitable<void> run_get_stream();
    /// Start the GET stream once, after `notifications/initialized` was sent.
    void start_get_stream();
    /// One GET request: `accept: text/event-stream` plus `last-event-id` when
    /// resuming, and the session/protocol/auth headers every request carries.
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>>
    get_stream_headers(const std::optional<std::string>& last_event_id);
    /// Await `delay` unless `close()` stops the stream first; false when the
    /// stream was stopped while waiting (pi `sleep`).
    [[nodiscard]] boost::asio::awaitable<bool> reconnect_wait(std::chrono::milliseconds delay);
    /// pi `reconnectDelay`: the server's `retry:` field when present, else the
    /// exponential backoff bounded by `max_delay`.
    [[nodiscard]] std::chrono::milliseconds reconnect_delay(int attempt, const std::optional<int>& server_delay_ms) const;

    /// Dispatch one JSON-RPC message received on the GET stream: a
    /// server-to-client request is answered, a notification is dispatched to
    /// the listener (and `notifications/cancelled` aborts the matching
    /// in-flight handler), and anything else is ignored.
    void handle_stream_message(const support::JsonValue& message);
    /// Answer one server-to-client request through a registered handler (pi
    /// `handleRequest`), or `-32601` when the method has none.
    [[nodiscard]] boost::asio::awaitable<void> serve_server_request(
            support::JsonValue id, std::string method, support::JsonValue params);
    /// POST one JSON-RPC response message (a raw frame, no completion).
    void post_message(support::JsonValue message);

    /// pi `close()`'s session teardown: one best-effort `DELETE` with a 1 s
    /// timeout, its outcome ignored.
    [[nodiscard]] boost::asio::awaitable<void> delete_session();

    [[nodiscard]] std::map<std::string, std::string> request_headers() const;
    /// Layer, when a request-auth source is configured, the source's current
    /// headers resolved for this request onto `headers`.
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> resolve_headers(
            std::map<std::string, std::string> headers);
    /// The same headers plus, when a request-auth source is configured, the
    /// source's current headers resolved for this request.
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>>
    request_headers_for_call();
    void capture_session(const ai::providers::StreamResponse& response);
    [[nodiscard]] support::Expected<support::JsonValue> interpret_response(
            int id, const ai::providers::StreamResponse& response) const;

    void enqueue_frame(std::string frame,
            int id,
            std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion,
            std::stop_token stop_token = {},
            bool cancellable = false);
    void complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome);

    boost::asio::any_io_executor executor_;
    McpHttpServerConfig config_;
    std::shared_ptr<ai::providers::StreamTransport> transport_;
    std::shared_ptr<McpRequestAuthSource> request_auth_;
    std::deque<std::unique_ptr<QueuedFrame>> queue_;
    std::string session_id_;
    std::string protocol_version_;
    int next_id_{1};
    bool pumping_{false};

    McpHttpGetStreamOptions get_stream_options_;
    /// The server-to-client stream's cancellation source: `close()` requests
    /// it to abort the GET request and the reconnect wait.
    std::stop_source stream_stop_;
    bool closed_{false};
    NotificationListener notification_listener_;
    ErrorListener error_listener_;
    std::map<std::string, ServerRequestHandler> request_handlers_;
    /// In-flight server-to-client request handlers, keyed by the serialized
    /// request id the server chose, so `notifications/cancelled` can abort the
    /// matching handler (pi `incoming`).
    std::map<std::string, std::stop_source> incoming_handlers_;
};

} // namespace cch::coding_agent::mcp
