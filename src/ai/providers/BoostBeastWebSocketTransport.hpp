#pragma once

#include "ai/providers/WebSocketTransport.hpp"

namespace cch::ai::providers {

/// Boost.Beast-backed WebSocket transport for the `openai-codex-responses`
/// adapter. Supports `wss://` and `ws://` URLs.
class BoostBeastWebSocketTransport final : public WebSocketTransport {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::shared_ptr<WebSocket>>> async_connect(
        const WebSocketConnectRequest& request) override;
};

} // namespace cch::ai::providers
