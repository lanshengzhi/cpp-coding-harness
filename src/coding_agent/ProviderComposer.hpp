#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ModelConfig.hpp"

#include <cch/ai/Models.hpp>
#include <cch/support/Error.hpp>
#include "agent/harness/Process.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// The shell/process capability used to resolve executable models.json values.
/// Provider assembly and transport construction stay inside cch_ai.
struct ProviderComposerOptions {
    std::shared_ptr<harness::AsyncProcessRunner> process_runner{nullptr};
};

/// Compose one provider from its built-in definition (optional) plus the
/// models.json `ModelConfig` entry (pi `composeModelProvider` subset): built-in
/// models → models.json overlay/custom-model upsert (same-ID replaces) → model
/// overrides. Config-only providers compose from config while protocol
/// adapters and transports remain private to cch_ai.
///
/// The returned Provider Change installs the composed definition, installs the
/// built-in definition unchanged, or removes the provider when it is absent
/// from both built-ins and config. On composition failure `error` carries the
/// message and the built-in definition is returned as fallback (removal when
/// there is no base), per pi `recomposeProvider`.
[[nodiscard]] ai::ProviderChange compose_provider(std::string_view provider_id,
        std::optional<ai::ProviderDefinition> base,
        const ModelConfig& config,
        const ProviderComposerOptions& options,
        std::optional<std::string>& error);

/// Env var names referenced by configured `apiKey` templates across all
/// providers in the config (pi `getConfigValueEnvVarNames`). Used by the
/// execution environment for secret filtering.
[[nodiscard]] std::vector<std::string> configured_api_key_env_names(
    const ModelConfig& config);

/// Source/type-only auth status derived from a models.json provider entry
/// (pi `configuredRequestAuthStatus`). Never resolves or executes the key
/// value; environment-template status reads the process environment.
[[nodiscard]] std::optional<ModelRuntimeAuthStatus> configured_request_auth_status(
    const ModelsJsonProvider& config);

/// Frozen default-model table for the supported subset
/// (pi `core/model-resolver.ts`): `openai-codex → gpt-5.5`,
/// `kimi-coding → kimi-for-coding`. Config-only providers (e.g. deepseek) have
/// no default model. The TUI auto-selection trigger is a later module; this
/// table is the `coding_agent` core contract.
[[nodiscard]] std::optional<std::string> default_model_for_provider(
    std::string_view provider_id);

} // namespace cch::coding_agent
