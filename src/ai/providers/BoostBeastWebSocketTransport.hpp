#pragma once

#include "ai/providers/WebSocketTransport.hpp"

namespace cch::ai::providers {

/// Boost.Beast-backed WebSocket transport for the `openai-codex-responses`
/// adapter. Client transports are TLS-only (ADR 0054): only `wss://` URLs are
/// accepted; `ws://` and every other scheme fail with a Validation error.
class BoostBeastWebSocketTransport final : public WebSocketTransport {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::shared_ptr<WebSocket>>> async_connect(
        const WebSocketConnectRequest& request) override;
};

} // namespace cch::ai::providers
