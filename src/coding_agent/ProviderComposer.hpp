#pragma once

#include <cch/coding_agent/ModelRuntime.hpp>
#include "ModelConfig.hpp"

#include <cch/ai/Model.hpp>
#include <cch/ai/Provider.hpp>
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/WebSocketTransport.hpp"
#include "ai/api/OpenAICodexResponsesAdapter.hpp"
#include <cch/util/Error.hpp>
#include "util/Process.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// Dependencies shared by every composed Provider. Transports and the process
/// runner are injectable so tests can script the wire and shell surfaces.
struct ProviderComposerOptions {
    std::shared_ptr<ai::providers::StreamTransport> http_transport{nullptr};
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport{nullptr};
    ai::providers::CodexWebSocketCacheConfig codex_cache_config{};
    std::shared_ptr<util::AsyncProcessRunner> process_runner{nullptr};
};

/// Built-in Providers in the supported subset, keyed by provider id
/// (`openai-codex` with the frozen Codex 7 catalog and OAuth auth,
/// `kimi-coding` with the frozen Kimi 4 catalog and env API-key + OAuth auth).
[[nodiscard]] std::map<std::string, std::shared_ptr<ai::Provider>, std::less<>>
builtin_providers(const ProviderComposerOptions& options);

/// Compose one provider from its built-in base (optional) plus the models.json
/// `ModelConfig` entry (pi `composeModelProvider` subset): built-in
/// Provider/models → models.json overlay/custom-model upsert (same-ID
/// replaces) → model overrides. Config-only providers compose from config plus
/// the privately registered API adapters; identities stay string-based.
///
/// Returns nullptr when the provider is absent from both built-ins and config.
/// On composition failure `error` carries the message and the built-in base is
/// returned as fallback (removal when there is no base), per pi
/// `recomposeProvider`.
[[nodiscard]] std::shared_ptr<ai::Provider> compose_provider(
    std::string_view provider_id,
    std::shared_ptr<ai::Provider> base,
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
