#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "support/ScriptedProvider.hpp"

#include <functional>
#include <exception>
#include <memory>
#include <stop_token>
#include <vector>

namespace cch::coding_agent {

/// The deliberately small stream view used by the coding-agent test seam.
/// Request Authentication and transport stay inside cch_ai; vertical tests
/// only need to observe cancellation on a scripted request.
struct ModelRuntimeTestStreamOptions {
    std::stop_token stop_token{};
};

/// Private test-only catalog and stream injection. These values describe a
/// deterministic model source, without model authentication hooks.
struct ModelRuntimeTestProvider {
    tests::ScriptedModelCatalog definition{};
    std::move_only_function<ai::ModelStream(ai::Model, ai::AiContext, ModelRuntimeTestStreamOptions)> stream{};
};

/// Replace the passive catalog/scripted stream, or keep real gateway execution
/// and replace its private transport.
struct ModelRuntimeTestOptions {
    std::vector<ModelRuntimeTestProvider> providers{};
    tests::ScriptedTransportOptions transports{};
};

/// Build a ModelRuntime with the scripted test options applied after the
/// normal runtime composition. This function is compiled only into test
/// targets; production ModelRuntime construction has no scripted-provider
/// dependency.
[[nodiscard]] support::ExpectedVoid apply_model_runtime_test_provider(
        ai::Models& models, ModelRuntimeTestProvider provider);

[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options,
        ModelRuntimeTestOptions test_options,
        std::shared_ptr<support::StreamTransport> catalog_transport = {});

[[nodiscard]] std::shared_ptr<support::StreamTransport> make_empty_magpie_transport();

/// Keep interactive / session-assembly tests off the live Magpie catalog at
/// 127.0.0.1:3425. Production create_agent_session keeps the product default;
/// tests must set the catalog seam explicitly.
inline void isolate_magpie_gateway(runtime::AgentSessionCreationRequest& request) {
    if (request.magpie_base_url.empty()) {
        request.magpie_base_url = "http://127.0.0.1:34259";
    }
    if (!request.model_runtime) {
        auto runtime = create_model_runtime_for_testing(
                {.api_key = request.session_facts.api_key, .magpie_base_url = request.magpie_base_url},
                {},
                make_empty_magpie_transport());
        if (!runtime) std::terminate();
        request.model_runtime = std::move(*runtime);
    }
}

} // namespace cch::coding_agent
