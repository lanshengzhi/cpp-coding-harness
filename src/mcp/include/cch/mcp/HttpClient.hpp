#pragma once

// Long-lived MCP server connection over the streamable HTTP transport (pi
// `packages/mcp/src/transports/streamable-http.ts`): one JSON-RPC POST per
// request, an `application/json` or `text/event-stream` response, an
// `mcp-session-id` captured from the server and echoed on later requests, and
// a reconnecting server-to-client GET stream. The wire implementation is
// private to cch_mcp.

#include <cch/mcp/Client.hpp>
#include <cch/mcp/HttpServerConfig.hpp>
#include <cch/mcp/RequestAuthSource.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cch::support {
class StreamTransport;
} // namespace cch::support

namespace cch::mcp {

/// The server-to-client GET stream's reconnect policy (pi
/// `StreamableHttpReconnectOptions` + `openGetStream`). The defaults are pi's
/// `DEFAULT_RECONNECT_INITIAL_DELAY_MS` 1000, `DEFAULT_RECONNECT_MAX_DELAY_MS`
/// 30000, and `DEFAULT_RECONNECT_MAX_RETRIES` 5.
struct HttpGetStreamOptions {
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

/// Long-lived MCP server connection over streamable HTTP. A caller injects
/// `transport` only to script the wire (tests); a null transport selects the
/// production HTTPS stack.
class HttpClient : public Client {
public:
    /// Connect to `config`, run the `initialize` handshake, and return the
    /// connected client. When `request_auth` is present, every JSON-RPC POST
    /// resolves its headers at request time and attaches them; a missing
    /// credential or a failed refresh fails the request explicitly. A
    /// malformed URL, a network failure, a non-2xx status, or an invalid
    /// handshake response is reported through the shared error channel.
    /// Redirects (301/302/303/307/308 with a Location) are followed with the
    /// frozen pi platform fetch rules; a cross-origin hop drops the
    /// credential headers, so a redirect target never receives the caller's
    /// Authorization (audit-1052 `transports.json`).
    [[nodiscard]] static support::AsyncResult<std::shared_ptr<HttpClient>> connect(HttpServerConfig config,
            std::shared_ptr<support::StreamTransport> transport = nullptr,
            std::shared_ptr<RequestAuthSource> request_auth = nullptr,
            HttpGetStreamOptions get_stream_options = {});

    /// One server-to-client notification received on the GET stream (pi
    /// `TransportEvents`'s `message`).
    void set_notification_listener(NotificationSink listener) override = 0;

    /// One server-to-client stream failure (pi `TransportEvents`'s `error`):
    /// an exhausted reconnect reports "MCP server-to-client stream dropped and
    /// could not be reopened", and a non-retryable open failure reports the
    /// HTTP error. `close()` reports nothing.
    using ErrorListener = std::function<void(const support::Error& error)>;
    virtual void set_error_listener(ErrorListener listener) = 0;

    /// One server-to-client request handler (pi `setRequestHandler`): the
    /// handler answers a JSON-RPC request the server sends on the GET stream.
    /// `ping` is installed by default and returns `{}`; a method with no
    /// handler is answered with `-32601 "Method not found: <method>"`.
    /// `stop_token` is requested when the server sends
    /// `notifications/cancelled` for the in-flight request. The handler's
    /// error becomes the response's `-32603` JSON-RPC error.
    using ServerRequestHandler = std::function<support::AsyncResult<support::JsonValue>(
            const support::JsonValue& params, std::stop_token stop_token)>;
    virtual void set_request_handler(std::string method, ServerRequestHandler handler) = 0;
};

} // namespace cch::mcp
