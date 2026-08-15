#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

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

struct ModelRuntimeTransportOptions;

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

/// The sole public model/auth runtime seam (ADR 0029/0030/0032). Owns Agent
/// Config Directory paths, `models.json`, built-in/config composition,
/// availability snapshots, Provider recomposition, and `refresh()`; it holds
/// and delegates `ai::Models` privately. Injected as a `std::shared_ptr` into
/// `CreateAgentSessionOptions` and into the stateful Agent (the sole injectable
/// seam per #326); runtimes are reusable across sessions with no dispose
/// ceremony.
///
/// Concurrency contract: a runtime and everything it owns (Models, Providers,
/// adapters, transports, credential store) are not internally synchronized.
/// All operations on one runtime — including every session sharing it — must
/// be driven by a single-threaded executor or otherwise serialized; do not
/// drive the same runtime from two threads. `AgentSession::prompt_blocking`
/// runs the prompt on a temporary executor created for that call, so it must
/// not interleave with another thread driving the same runtime.
class ModelRuntime {
public:
    /// Construct and refresh a ModelRuntime from the Agent Config Directory.
    /// Filesystem I/O is synchronous; provider recomposition is synchronous.
    [[nodiscard]] static support::Expected<std::shared_ptr<ModelRuntime>> create(
        ModelRuntimeOptions options = {});

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

    // ── Provider / model lookup (live, delegating) ────────────────────────

    [[nodiscard]] std::vector<std::shared_ptr<ai::Provider>> providers() const;
    [[nodiscard]] std::shared_ptr<ai::Provider> provider(
        std::string_view provider_id) const;
    /// The AI-owned Models catalog this runtime composes and delegates to.
    /// The Agent's model-streaming seam is built from this catalog's
    /// `ModelStream` surface (ADR 0040 / #453), not from the runtime itself.
    /// Virtual for the session-seam recording fake (the §7.2 recorded
    /// exception: an impl-less fake serves a scripted catalog).
    [[nodiscard]] virtual std::shared_ptr<ai::Models> ai_models() const;
    [[nodiscard]] std::vector<ai::Model> models(
        std::optional<std::string_view> provider_id = std::nullopt) const;
    /// Virtual for recording test fakes that stand in for the runtime at the
    /// session seam (the §7.2 recorded exception: an impl-less fake serves
    /// model resolution and auth).
    [[nodiscard]] virtual std::optional<ai::Model> model(
        std::string_view provider_id,
        std::string_view model_id) const;

    // ── Availability ───────────────────────────────────────────────────────

    /// Live availability: models whose provider has configured auth. Without a
    /// provider id this runs an availability refresh against the live runtime
    /// and refreshes the snapshot. Cached snapshots never replace live
    /// `get_auth`/`check_auth` semantics. Virtual for recording test fakes
    /// (same recorded exception as `model`).
    [[nodiscard]] virtual boost::asio::awaitable<support::Expected<std::vector<ai::Model>>> get_available(
        std::optional<std::string_view> provider_id = std::nullopt);
    /// Last availability snapshot (synchronous). Never replaces live semantics.
    [[nodiscard]] std::vector<ai::Model> get_available_snapshot() const;

    // ── Authentication ─────────────────────────────────────────────────────

    /// Side-effect-free; OAuth credentials are never refreshed. Virtual for
    /// recording test fakes (same recorded exception as `model`).
    [[nodiscard]] virtual boost::asio::awaitable<support::Expected<std::optional<ai::AuthCheck>>> check_auth(
        std::string provider_id);
    /// Live authentication resolution (delegates to `ai::Models`).
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<ai::AuthResult>>> get_auth(
        std::string provider_id,
        std::optional<std::string> explicit_api_key = std::nullopt);
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<ai::AuthResult>>> get_auth(
        ai::Model model,
        std::optional<std::string> explicit_api_key = std::nullopt);

    /// True when the provider currently resolves as configured (snapshot).
    /// Virtual for recording test fakes (same recorded exception as `model`).
    [[nodiscard]] virtual bool has_configured_auth(std::string_view provider_id) const;
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
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::vector<ai::CredentialInfo>>> list_credentials();

    // ── Login / logout ─────────────────────────────────────────────────────

    /// `Models::login` + `refresh()`. A post-login refresh failure is recorded
    /// in the composition-errors map and never fails the login call.
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::Credential>> login(
        std::string provider_id,
        ai::AuthType type,
        ai::AuthInteraction interaction);

    /// `Models::logout` + `recomposeProvider` + `refresh()` (order preserved).
    /// A post-logout refresh failure is recorded and never fails the logout.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> logout(
        std::string provider_id);

    // ── Streaming ─────────────────────────────────────────────────────────

    // Model streaming is produced through `models()->stream(...)`, the
    // AI-owned `ModelStream` seam (ADR 0040 / #453). The runtime keeps model
    // resolution, authentication, and Provider composition; it no longer
    // exposes a streaming surface of its own.

    // ── Provider registration ──────────────────────────────────────────────

    /// Register a native provider used exactly as given (pi
    /// `registerNativeProvider`). Replaces any built-in or composed provider
    /// with the same id. Empty provider ids are rejected.
    [[nodiscard]] support::ExpectedVoid register_native_provider(
        std::shared_ptr<ai::Provider> provider);

    /// Env var names referenced by configured models.json `apiKey` templates
    /// (pi `getConfigValueEnvVarNames`). Used for execution environment secret
    /// filtering. Virtual for recording test fakes (same recorded exception
    /// as `model`); the impl-less fake serves no env names.
    [[nodiscard]] virtual std::vector<std::string> configured_api_key_env_names() const;

    /// Frozen default-model table for the supported provider subset.
    [[nodiscard]] static std::optional<std::string> default_model_for_provider(
        std::string_view provider_id);

    struct Impl;

protected:
    /// Test-support construction for recording fakes that stand in for the
    /// runtime at the session seam. Leaves the runtime without an impl; only
    /// the virtual model/auth surfaces overridden by the fake are callable.
    /// Production construction always goes through `create`.
    ModelRuntime() noexcept;

private:
    explicit ModelRuntime(std::unique_ptr<Impl> impl);
    /// Private test-support assembly: build a runtime around an externally
    /// constructed `ai::Models` without re-composing providers. The Models
    /// stays privately held; the runtime delegates everything to it. Used by
    /// the private session test seam only.
    static support::Expected<std::shared_ptr<ModelRuntime>> create_from_models_for_testing(
        std::shared_ptr<ai::Models> models,
        ModelRuntimeOptions options = {});

    friend class runtime::SessionFactory;
    friend support::Expected<std::shared_ptr<ModelRuntime>> create_model_runtime_for_testing(
        ModelRuntimeOptions options,
        ModelRuntimeTransportOptions transports);
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
