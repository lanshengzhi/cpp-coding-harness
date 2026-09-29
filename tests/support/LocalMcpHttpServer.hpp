#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests {

/// One HTTP request the local MCP server received, kept as the bytes reached
/// it. Tests assert on `raw_head` when the question is what the client
/// actually wrote, and on the parsed members when it is what the request
/// meant.
struct RecordedHttpRequest {
    std::string method{};
    std::string target{};
    /// The request line and the header block, byte for byte.
    std::string raw_head{};
    /// Header values keyed by the lowercased field name, so a lookup does not
    /// depend on how the client spelled the name's case.
    std::map<std::string, std::string> headers{};
    std::string body{};

    [[nodiscard]] std::string header(std::string_view name) const;
    [[nodiscard]] bool has_header(std::string_view name) const;
};

/// How the local MCP server answers one request.
struct McpServerReply {
    int status_code{200};
    std::string content_type{"application/json"};
    /// The whole response body, sent as one message body.
    std::string body{};
    /// Send `body` as a `text/event-stream` of `data:` frames. When
    /// `event_payloads` is set it replaces `body`, one frame per element, so
    /// a case can put a server notification ahead of the response the client
    /// is waiting for.
    bool as_event_stream{false};
    std::vector<std::string> event_payloads{};
    /// Close the connection once this many body bytes have been written,
    /// leaving the response stream broken mid-body. The 2026-07-28 revision
    /// has nothing to resume it with, so the in-flight request is simply
    /// lost.
    std::optional<std::size_t> truncate_after_bytes{};
    /// Complete the response body but leave the last event-stream frame
    /// without its blank-line terminator, so the stream ends mid-frame.
    bool end_stream_mid_frame{false};
    /// Keep writing filler of this many bytes after the body, for an Upstream
    /// that floods the client. The client is expected to terminate the
    /// exchange long before the last byte; the server stops when the client
    /// stops reading.
    std::size_t flood_bytes{0};
};

struct LocalMcpHttpServerOptions {
    /// Refuse the TLS handshake of the first accepted connection, so a client
    /// whose first attempt fails before the request is written can be observed
    /// re-attempting it.
    bool abort_first_handshake{false};
    /// Stop serving after this long, so a case that never finishes cannot
    /// outlive its shard.
    std::chrono::seconds lifetime{std::chrono::seconds{30}};
};

/// A test-only local Upstream MCP Server: a TLS HTTP/1.1 server on an
/// ephemeral loopback port, secured by the committed test certificate, that
/// answers with whatever the case scripts.
///
/// It is the ADR 0054 test-CA pattern applied to HTTP instead of WebSocket, and
/// it is a fixture, never a second production seam: it lives in
/// `tests/support/`, it is reached only through the one `McpTransport` seam,
/// and the production transport under test is the Beast TLS client the
/// Runtime will use. Each instance owns its port and its thread, and
/// `stop()` releases both.
class LocalMcpHttpServer {
public:
    /// `std::nullopt` means this fixture does not answer that request, and the
    /// client sees an empty refusal rather than a silent success.
    using Handler = std::function<std::optional<McpServerReply>(const RecordedHttpRequest&)>;

    explicit LocalMcpHttpServer(Handler handler, LocalMcpHttpServerOptions options = {});
    ~LocalMcpHttpServer();

    LocalMcpHttpServer(const LocalMcpHttpServer&) = delete;
    LocalMcpHttpServer& operator=(const LocalMcpHttpServer&) = delete;

    /// Whether the loopback endpoint came up. A fixture whose case is about
    /// anything else requires this, because a fixture with no endpoint turns
    /// every later assertion into an ordinary connection failure.
    [[nodiscard]] bool ready() const noexcept;

    /// Why the fixture did not come up; empty when it did.
    [[nodiscard]] const std::string& setup_error() const noexcept;

    /// The loopback port the server bound.
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// The `https://` endpoint URL of `path` on that port.
    [[nodiscard]] std::string url(std::string path = "/mcp") const;

    /// Every request the server has received so far, in order.
    [[nodiscard]] std::vector<RecordedHttpRequest> requests() const;

    /// Stop accepting, release the live connections, and join the server
    /// thread. Idempotent, and called by the destructor.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tests
