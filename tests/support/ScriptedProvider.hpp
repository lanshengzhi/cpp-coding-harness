#pragma once

#include <cch/ai/Models.hpp>
#include "support/StreamTransport.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cch::tests {

/// Private transport substitution for inference tests.
struct ScriptedTransportOptions {
    std::shared_ptr<support::StreamTransport> http_transport{nullptr};
};

/// Passive identity/catalog data used only by scripted test streams.
struct ScriptedModelCatalog {
    std::string id{};
    std::string name{};
    std::vector<ai::Model> models{};
};

struct ScriptedProviderDefinition {
    ScriptedModelCatalog definition{};
    // The same factory shape ai::Models::stream exposes: a scripted stream is
    // a fake adapter over the Models stream seam, not a Provider install
    // (ADR 0072) — the Caller's options stay exactly what it passed.
    std::move_only_function<ai::ModelStream(ai::Model, ai::AiContext, ai::SimpleStreamOptions)> stream{};
    ScriptedTransportOptions transport{};
};

/// Add passive model values and a scripted stream to private test support.
/// Production Models has no Provider installation or authentication graph.
[[nodiscard]] support::ExpectedVoid apply_scripted_provider(ai::Models& models, ScriptedProviderDefinition definition);

/// Configure a test runtime with a deterministic transport.
/// This helper is private test support; production Models construction always
/// selects its own HTTP transport.
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
