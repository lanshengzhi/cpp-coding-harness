#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {
namespace runtime {
class SessionFactory;
}

struct ModelRuntimeTestOptions;
struct ModelRuntimeTransportTestOptions;

/// Injectable creation options for ModelRuntime (pi `CreateModelRuntimeOptions`
/// subset). Every field is optional; defaults derive from the Agent Config
/// Directory.
struct ModelRuntimeOptions {
    /// Agent Config Directory override (pi `agentDir`). When empty, the
    /// default `agent_config_dir()` applies (`PI_CODING_AGENT_DIR`, then
    /// `~/.pi/agent`).
    std::filesystem::path agent_dir{};
    /// models.json path. When unset, `<agentDir>/models.json`. An explicitly
    /// empty path disables the models.json surface (empty user config).
    std::optional<std::filesystem::path> models_path{std::nullopt};
    /// Credential storage. When unset, a file-backed AuthStorage at
    /// `<agentDir>/auth.json` is constructed and injected into `ai::Models`
    /// (ADR 0030); the ai layer never derives agent-directory paths.
    std::shared_ptr<ai::CredentialStore> credentials{nullptr};
};

/// Provider auth status (pi `AuthStatus`): configured flag plus the source
/// label. Never resolves or executes configured key values.
struct ModelRuntimeAuthStatus {
    bool configured{false};
    std::optional<std::string> source{std::nullopt};
    std::optional<std::string> label{std::nullopt};
};

/// The concrete model/auth runtime (ADR 0029/0030/0040). Owns Agent Config
/// Directory paths, `models.json`, built-in/config composition, availability
/// snapshots, Provider recomposition, and `refresh()`; it holds and delegates
/// `ai::Models` privately. A concrete runtime may be shared across Sessions
/// and has no dispose ceremony.
///
/// Concurrency contract: a runtime and everything it owns (Models, Providers,
/// adapters, transports, credential store) are not internally synchronized.
/// All operations on one runtime — including every session sharing it — must
/// be driven by a single-threaded executor or otherwise serialized; do not
/// drive the same runtime from two threads. `AgentSession::prompt_blocking`
/// runs the prompt on a temporary executor created for that call, so it must
/// not interleave with another thread driving the same runtime.
class ModelRuntime final {
public:
    /// Construct and refresh a ModelRuntime from the Agent Config Directory.
    /// Filesystem I/O is synchronous; provider recomposition is synchronous.
    [[nodiscard]] static support::Expected<std::shared_ptr<ModelRuntime>> create(
        ModelRuntimeOptions options = {});

    /// Test-only wrapper for an already-composed Models value. Production
    /// callers use create() so Models Runtime owns provider composition.
    explicit ModelRuntime(std::shared_ptr<ai::Models> models);

    ModelRuntime(ModelRuntime&&) noexcept;
    ModelRuntime& operator=(ModelRuntime&&) noexcept;
    ~ModelRuntime();
    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;

    /// Resolved Agent Config Directory for this runtime.
    [[nodiscard]] const std::filesystem::path& agent_dir() const noexcept;
    /// Resolved models.json path (empty when the models.json surface is off).
    [[nodiscard]] const std::filesystem::path& models_path() const noexcept;

    /// Reload models.json and recompose all providers. Invalid or unreadable
    /// config becomes empty user config plus diagnostics; a per-Provider
    /// composition failure falls back to its built-in or removes the provider;
    /// there is no global rollback.
    [[nodiscard]] support::ExpectedVoid refresh();

    /// Config, composition, and availability diagnostics. Empty when healthy.
    [[nodiscard]] std::optional<std::string> get_error() const;

    // ── Provider / model lookup (passive, live) ──────────────────────────

    /// Installed provider metadata for hosts and frontends. No capability
    /// pointer or authentication hook escapes this runtime.
    [[nodiscard]] std::vector<ai::ProviderInfo> providers() const;
    /// Installed provider metadata by identity, or no value when absent.
    [[nodiscard]] std::optional<ai::ProviderInfo> provider(std::string_view provider_id) const;
    /// The AI-owned Models catalog this runtime composes and delegates to.
    /// The Agent's model-streaming seam is built from this catalog's
    /// `ModelStream` surface (ADR 0040 / #453), not from the runtime itself.
    [[nodiscard]] std::shared_ptr<ai::Models> ai_models() const;
    [[nodiscard]] std::vector<ai::Model> models(
        std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<ai::Model> model(std::string_view provider_id, std::string_view model_id) const;

    // ── Availability ───────────────────────────────────────────────────────

    /// Live availability: models whose provider has configured auth. Without a
    /// provider id this runs an availability refresh against the live runtime
    /// and refreshes the snapshot. Cached snapshots never replace live
    /// `get_auth`/`check_auth` semantics.
    [[nodiscard]] support::AsyncResult<std::vector<ai::Model>> get_available(
            std::optional<std::string> provider_id = std::nullopt);
    /// Last availability snapshot (synchronous). Never replaces live semantics.
    [[nodiscard]] std::vector<ai::Model> get_available_snapshot() const;

    // ── Authentication ─────────────────────────────────────────────────────

    /// Side-effect-free; OAuth credentials are never refreshed.
    [[nodiscard]] support::AsyncResult<std::optional<ai::AuthCheck>> check_auth(std::string provider_id);
    /// Live authentication resolution (delegates to `ai::Models`).
    [[nodiscard]] support::AsyncResult<std::optional<ai::AuthResult>> get_auth(
            std::string provider_id, std::optional<std::string> explicit_api_key = std::nullopt);
    [[nodiscard]] support::AsyncResult<std::optional<ai::AuthResult>> get_auth(
            ai::Model model, std::optional<std::string> explicit_api_key = std::nullopt);

    /// True when the provider currently resolves as configured (snapshot).
    [[nodiscard]] bool has_configured_auth(std::string_view provider_id) const;
    /// True when the provider authenticates through an OAuth credential.
    [[nodiscard]] bool is_using_oauth(std::string_view provider_id) const;

    /// Install a process-lifetime, in-memory runtime API key override for one
    /// provider (pi `setRuntimeApiKey`). Never persisted. Takes the highest
    /// precedence in Request Authentication (runtime override → stored
    /// auth.json → environment → models.json configured key). Refreshes the
    /// availability snapshot so the provider resolves as configured.
    [[nodiscard]] support::ExpectedVoid set_runtime_api_key(
        std::string provider_id,
        std::string api_key);
    /// Remove a runtime API key override, restoring the underlying resolution.
    [[nodiscard]] support::ExpectedVoid remove_runtime_api_key(
        std::string provider_id);
    /// True when a runtime API key override is installed for the provider.
    [[nodiscard]] bool has_runtime_api_key(std::string_view provider_id) const;
    /// Source/type-only auth status for status UI (never triggers network).
    [[nodiscard]] std::optional<ModelRuntimeAuthStatus> get_provider_auth_status(
        std::string_view provider_id) const;
    [[nodiscard]] support::AsyncResult<std::vector<ai::CredentialInfo>> list_credentials();

    // ── Login / logout ─────────────────────────────────────────────────────

    /// `Models::login` + `refresh()`. A post-login refresh failure is recorded
    /// in the composition-errors map and never fails the login call.
    [[nodiscard]] support::AsyncResult<ai::Credential> login(
            std::string provider_id, ai::AuthType type, ai::AuthInteraction interaction);

    /// `Models::logout` + `recomposeProvider` + `refresh()` (order preserved).
    /// A post-logout refresh failure is recorded and never fails the logout.
    [[nodiscard]] support::AsyncResult<void> logout(std::string provider_id);

    // ── Streaming ─────────────────────────────────────────────────────────

    // Model streaming is produced through `models()->stream(...)`, the
    // AI-owned `ModelStream` seam (ADR 0040 / #453). The runtime keeps model
    // resolution, authentication, and Provider composition; it no longer
    // exposes a streaming surface of its own.

    /// Env var names referenced by configured models.json `apiKey` templates
    /// (pi `getConfigValueEnvVarNames`). Used for execution environment secret
    /// filtering.
    [[nodiscard]] std::vector<std::string> configured_api_key_env_names() const;

    /// Frozen default-model table for the supported provider subset.
    [[nodiscard]] static std::optional<std::string> default_model_for_provider(
        std::string_view provider_id);

private:
    struct Impl;

    [[nodiscard]] static support::Expected<std::shared_ptr<ModelRuntime>> create_impl(ModelRuntimeOptions options);
    explicit ModelRuntime(std::unique_ptr<Impl> impl);

    friend class runtime::SessionFactory;
    friend support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
            ModelRuntimeOptions options, ModelRuntimeTestOptions test_options);
    friend support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
            ModelRuntimeOptions options, ModelRuntimeTransportTestOptions test_options);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
