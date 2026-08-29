#pragma once

#include <cch/ai/Models.hpp>
#include "ai/providers/Provider.hpp"
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::ai::providers {

/// Test-only Provider Definition plus a private stream implementation. The
/// public Provider Definition remains passive; this carrier lets tests inject
/// deterministic stream behavior through the AI-owned construction helper.
struct ScriptedTransportOptions {
    std::shared_ptr<StreamTransport> http_transport{nullptr};
    std::shared_ptr<WebSocketTransport> ws_transport{nullptr};
    CodexWebSocketCacheConfig codex_cache_config{};
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

/// Build the deterministic fake definition used by vertical session/TUI/CLI
/// tests. The definition has an empty catalog because the CLI fake path uses
/// its explicit request-model seam.
[[nodiscard]] ScriptedProviderDefinition make_scripted_fake_provider_definition(std::string provider_id = "fake");

/// Build the two aliases used by the vertical fake-provider fixture.
[[nodiscard]] std::vector<ScriptedProviderDefinition> make_scripted_fake_provider_definitions();

/// Transitional Provider-pointer helper for older test fixtures. It is
/// implemented by this test-only source and is not part of the cch_ai
/// production target; new vertical tests use scripted definitions instead.
[[nodiscard]] std::shared_ptr<ai::Provider> make_scripted_fake_provider(
    std::string provider_id = "fake");

/// In-memory credential storage for scripted runtime tests. It avoids writing
/// an auth.json file while preserving the real Models authentication path.
[[nodiscard]] std::shared_ptr<ai::CredentialStore> make_scripted_credential_store();

/// Legacy test helper retained while cch_ai contract tests migrate to the
/// definition-oriented construction helper. It is implemented by this
/// test-only source and is not part of the cch_ai production target.
[[nodiscard]] std::shared_ptr<ai::Models> make_scripted_fake_models();

} // namespace cch::ai::providers
