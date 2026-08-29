#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ai/providers/FakeProvider.hpp"

#include <memory>
#include <vector>

namespace cch::coding_agent {

/// Private test-only provider injection. The actual Provider capability and
/// scripted transport construction stay inside cch_ai's test support; callers
/// submit complete scripted definitions through this ModelRuntime seam.
struct ModelRuntimeTestOptions {
    std::vector<ai::providers::ScriptedProviderDefinition> providers{};
    ai::providers::ScriptedTransportOptions transports{};
};

/// Build a ModelRuntime with scripted Provider Definitions installed after the
/// normal runtime composition. This function is compiled only into test
/// targets; production ModelRuntime construction has no scripted-provider
/// dependency.
[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options, ModelRuntimeTestOptions test_options);

} // namespace cch::coding_agent
