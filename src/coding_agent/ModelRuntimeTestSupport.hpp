#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <chrono>
#include <memory>

namespace cch::coding_agent {

/// Test-only transport injection for the vertical wire-path suites. The public
/// `ModelRuntimeOptions` deliberately names no HTTP, WebSocket, or transport
/// type (ADR 0040 / #455); this private seam temporarily re-wraps the
/// cch_ai-composed Providers with scripted transports until the scripted
/// Provider Definition seam is migrated.
struct ModelRuntimeTransportOptions {
    std::shared_ptr<ai::providers::StreamTransport> http_transport{nullptr};
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport{nullptr};
    ai::providers::CodexWebSocketCacheConfig codex_cache_config{};
};

/// Replace the providers in an already-composed Models collection with
/// transport-injected equivalents. This is test-only compatibility for the
/// pre-Provider-Definition scripted wire tests; production ModelRuntime
/// construction never calls this path.
[[nodiscard]] inline support::ExpectedVoid apply_test_transport_options(
        ai::Models& models, const ModelRuntimeTransportOptions& options) {
    const bool custom_cache = options.codex_cache_config.idle_close != std::chrono::minutes{5} ||
                              options.codex_cache_config.max_age != std::chrono::minutes{55};
    if (!options.http_transport && !options.ws_transport && !custom_cache) {
        return {};
    }

    auto http_transport = options.http_transport;
    if (!http_transport) {
        http_transport = std::make_shared<ai::providers::BoostBeastStreamTransport>();
    }
    auto ws_transport = options.ws_transport;
    if (!ws_transport) {
        ws_transport = std::make_shared<ai::providers::BoostBeastWebSocketTransport>();
    }

    for (auto& provider : models.providers()) {
        auto replacement = ai::providers::make_composed_provider(std::string{provider->id()},
                std::string{provider->name()},
                provider->models(),
                std::move(provider->auth()),
                http_transport,
                ws_transport,
                options.codex_cache_config);
        if (auto installed = models.set_provider(std::move(replacement)); !installed) {
            return std::unexpected(installed.error());
        }
    }
    return {};
}

/// Build a ModelRuntime with explicit transport injection. Production
/// construction goes through `ModelRuntime::create` and never calls this
/// test-only compatibility seam.
[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
    ModelRuntimeOptions options,
    ModelRuntimeTransportOptions transports);

} // namespace cch::coding_agent
