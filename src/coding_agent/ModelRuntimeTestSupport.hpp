#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "support/ScriptedProvider.hpp"

#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

namespace cch::coding_agent {

/// The deliberately small stream view used by the coding-agent test seam.
/// Provider authentication and transport details stay inside cch_ai; vertical
/// tests only need to observe cancellation on a scripted request.
struct ModelRuntimeTestStreamOptions {
    std::stop_token stop_token{};
};

/// Private test-only provider injection. The actual Provider capability and
/// scripted construction stay inside cch_ai's test support; callers submit
/// complete passive definitions through this ModelRuntime seam.
struct ModelRuntimeTestProvider {
    ai::ProviderDefinition definition{};
    std::move_only_function<ai::ModelStream(ai::Model, ai::AiContext, ModelRuntimeTestStreamOptions)> stream{};
};

/// The two substitution points a test runtime offers, in one options value:
/// scripted Provider Definitions replace composition outright, while
/// scripted transports keep the real composed Providers and replace only
/// their network endpoints. The second form is what keeps the frozen
/// DeepSeek wire path observable end to end.
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
        ModelRuntimeOptions options, ModelRuntimeTestOptions test_options);

} // namespace cch::coding_agent
