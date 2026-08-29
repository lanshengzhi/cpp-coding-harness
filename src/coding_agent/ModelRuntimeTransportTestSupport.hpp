#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ai/providers/FakeProvider.hpp"

namespace cch::coding_agent {

/// Transitional transport-only test seam for the cch_ai wire-path tests.
///
/// debt: remove this seam when the remaining transport tests inject scripted
/// Provider Definitions, upgrade trigger: all coding-agent adapter tests use
/// ModelRuntimeTestOptions instead.
struct ModelRuntimeTransportTestOptions {
    ai::providers::ScriptedTransportOptions transports{};
};

[[nodiscard]] support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options, ModelRuntimeTransportTestOptions test_options);

} // namespace cch::coding_agent
