#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::providers {

/// One WebSocket connection request. The connect timeout bounds the TCP/TLS
/// handshake; the idle timeout bounds each wait for the next text frame and is
/// re-armed per frame, matching pi's WS idle timeout.
struct WebSocketConnectRequest {
    std::string url;
    std::map<std::string, std::string, std::less<>> headers;
    std::chrono::milliseconds connect_timeout{15000};
    std::optional<std::chrono::milliseconds> idle_timeout{std::nullopt};
    std::stop_token stop_token{};
};

/// One bidirectional WebSocket connection. Implementations are driven by a
/// single coroutine: sends and receives never overlap.
class WebSocket {
public:
    virtual ~WebSocket() = default;

    /// Sends one text frame. The borrowed view must remain valid until the
    /// returned awaitable completes.
    [[nodiscard]] virtual boost::asio::awaitable<util::ExpectedVoid> async_send(
        std::string_view text) = 0;

    /// Next text frame; std::nullopt when the peer closed the connection.
    /// Returns a Cancelled error when the request stop token fires.
    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::optional<std::string>>> async_receive() = 0;

    /// Best-effort close; never throws and is idempotent.
    virtual void close() = 0;
};

/// Injectable WebSocket transport used by the `openai-codex-responses` adapter.
class WebSocketTransport {
public:
    virtual ~WebSocketTransport() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<std::shared_ptr<WebSocket>>> async_connect(
        const WebSocketConnectRequest& request) = 0;
};

} // namespace cch::ai::providers
