#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <memory>

namespace cch::coding_agent {

/// Test-only transport injection for the vertical wire-path suites. The public
/// `ModelRuntimeOptions` deliberately names no HTTP, WebSocket, or transport
/// type (ADR 0040 / #455); tests inject scripted transports through this
/// private seam so the full composition stays reachable without widening the
/// public surface.
struct ModelRuntimeTransportOptions {
    std::shared_ptr<ai::providers::StreamTransport> http_transport{nullptr};
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport{nullptr};
    ai::providers::CodexWebSocketCacheConfig codex_cache_config{};
};

/// Build a ModelRuntime with explicit transport injection. Production
/// construction goes through `ModelRuntime::create`, which fills the default
/// Boost.Beast transports and delegates here.
[[nodiscard]] util::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
    ModelRuntimeOptions options,
    ModelRuntimeTransportOptions transports);

} // namespace cch::coding_agent
