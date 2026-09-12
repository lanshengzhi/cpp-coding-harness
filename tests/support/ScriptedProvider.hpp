#pragma once

#include <cch/ai/Models.hpp>
#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::tests {

/// Test-only Provider Definition plus a private stream implementation. The
/// public Provider Definition remains passive; this carrier lets tests inject
/// deterministic stream behavior through the AI-owned construction helper.
struct ScriptedTransportOptions {
    std::shared_ptr<ai::providers::StreamTransport> http_transport{nullptr};
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport{nullptr};
    ai::providers::CodexWebSocketCacheConfig codex_cache_config{};
};

struct ScriptedProviderDefinition {
    ai::ProviderDefinition definition{};
    std::move_only_function<ai::ModelStream(ai::Model, ai::AiContext, ai::ProviderStreamOptions)> stream{};
    ScriptedTransportOptions transport{};
};

/// Install one scripted definition in an existing Models runtime. Provider
/// construction stays inside cch_ai; tests never need to register a Provider
/// pointer or construct a transport themselves.
[[nodiscard]] support::ExpectedVoid apply_scripted_provider(ai::Models& models, ScriptedProviderDefinition definition);

/// Replace composed providers in a test runtime with deterministic transports.
/// This helper is private test support; production Models construction always
/// selects its own HTTP/WebSocket transports.
[[nodiscard]] support::ExpectedVoid apply_scripted_transport_options(
        ai::Models& models, ScriptedTransportOptions options);

// AI-layer fake builders. They compose `ScriptedProviderDefinition` values over
// the cch_ai `Models` seam; the other family lives in `support/ModelsFixture.hpp`
// (`cch::tests`) and builds `coding_agent::ModelRuntimeTestProvider` values over
// the ModelRuntime test seam, keeping the `make_scripted_fake_*` names. Keep
// this family `make_ai_`-prefixed so the two layers cannot redeclare each other.

/// Build the deterministic fake definition used by vertical session/TUI/CLI
/// tests. The definition has an empty catalog because the CLI fake path uses
/// its explicit request-model seam.
[[nodiscard]] ScriptedProviderDefinition make_ai_scripted_fake_provider_definition(std::string provider_id = "fake");

/// Build the two aliases used by the vertical fake-provider fixture.
[[nodiscard]] std::vector<ScriptedProviderDefinition> make_ai_scripted_fake_provider_definitions();

/// Build the cch_ai-only fake Models collection used by the Provider contract
/// tests. Vertical tests use the coding-agent definition seam instead.
[[nodiscard]] std::shared_ptr<ai::Models> make_ai_scripted_fake_models();

} // namespace cch::tests
