#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace cch::mcp::transport {

struct StreamableHttpTransportOptions {
    /// An extra PEM trust anchor merged into the platform verify paths, in the
    /// ADR 0054 test-CA pattern. It exists so a test can reach a local
    /// `https://` fixture that presents a committed test certificate; the
    /// production transport leaves it empty, and peer and host-name
    /// verification are unchanged either way.
    std::optional<std::string> trusted_ca_certificate_pem{};

    /// The attempts one exchange may make, including the first. Only a
    /// retryable failure class consumes more than one
    /// (`mcp/transport/RetryPolicy.hpp`); the default admits exactly one
    /// re-attempt.
    std::size_t max_attempts{2};
};

/// The MCP Host's production transport: Streamable HTTP over Beast TLS, the
/// one implementation of the `McpTransport` seam the client stack runs on
/// (ADR 0065, ADR 0054).
///
/// It is a client transport, so it is TLS-only: an `http://` endpoint is
/// refused before any network work. One exchange is one connection, because
/// the 2026-07-28 revision has no `Mcp-Session-Id` and no SSE resumability to
/// keep a connection worth reusing; the consequence is that a flooding or
/// misbehaving Upstream owns nothing between two calls and cannot stall a
/// later one or the connection's Close.
///
/// The transport holds no Upstream state, so it has nothing to close: the
/// per-exchange socket is released when the exchange ends, and a cancelled
/// call releases it with its stop token.
class BoostBeastStreamableHttpTransport final : public McpTransport {
public:
    /// `executor` is the serialized execution domain the connection runs on,
    /// as the `McpTransport` contract requires. Every exchange is driven
    /// there, so the transport never starts a thread and never synchronizes
    /// itself.
    explicit BoostBeastStreamableHttpTransport(
            boost::asio::any_io_executor executor, StreamableHttpTransportOptions options = {});

    /// Bind to the serialized execution domain of the first exchange instead
    /// of naming it here. The `McpTransport` contract already says the
    /// transport is driven by the caller's execution domain, so this is the
    /// construction an Owner that may not name an Asio type takes (ADR
    /// 0065): the domain is the one that initiates the first exchange, and
    /// every later exchange on the same transport reuses it, which is the
    /// "one connection's operations on one serialized executor" rule the seam
    /// already states. A transport that has never been bound and is handed a
    /// request outside any serialized domain fails the exchange instead of
    /// guessing one.
    explicit BoostBeastStreamableHttpTransport(StreamableHttpTransportOptions options);

    /// One POST of the framed JSON-RPC request, answered either as a whole
    /// response body or as a `text/event-stream` whose frames are assembled
    /// into that body. The request's `timeout` bounds connection setup,
    /// request dispatch, the response headers, and the wait for the first
    /// response byte; past that point the response stream is governed by the
    /// retention bound and the request's stop token, because an Upstream may
    /// legitimately hold an event stream open while it works.
    [[nodiscard]] cch::support::AsyncResult<McpResponse> send(McpRequest request) override;

private:
    /// The domain this transport's exchanges run on: bound at construction,
    /// or by the first exchange for a late-bound transport and reused by
    /// every exchange after it.
    std::optional<boost::asio::any_io_executor> executor_{};
    StreamableHttpTransportOptions options_;
};

} // namespace cch::mcp::transport
