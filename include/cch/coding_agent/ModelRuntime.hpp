#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/ChatClient.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include <cch/ai/providers/WebSocketTransport.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/util/Error.hpp>

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

/// Injectable creation options for ModelRuntime (pi `CreateModelRuntimeOptions`
/// subset). Every field is optional; defaults derive from the Agent Config
/// Directory.
struct ModelRuntimeOptions {
    /// Agent Config Directory override (SDK `agentDir`). When empty, the
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
    /// HTTP transport for the scoped adapters. When unset, the Boost.Beast
    /// implementation is used.
    std::shared_ptr<ai::providers::StreamTransport> http_transport{nullptr};
    /// WebSocket transport for the openai-codex adapter. When unset, the
    /// Boost.Beast implementation is used.
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport{nullptr};
    /// Codex WebSocket socket-reuse windows. Injectable so tests can shrink
    /// the expiry windows; production uses pi's frozen 5-minute/55-minute
    /// defaults.
    ai::providers::CodexWebSocketCacheConfig codex_cache_config{};
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
/// `CreateAgentSessionOptions`; runtimes are reusable across sessions with no
/// dispose ceremony.
class ModelRuntime final : public ai::StreamingChatClient {
public:
    /// Construct and refresh a ModelRuntime from the Agent Config Directory.
    /// Filesystem I/O is synchronous; provider recomposition is synchronous.
    [[nodiscard]] static util::Expected<std::shared_ptr<ModelRuntime>> create(
        ModelRuntimeOptions options = {});

    ModelRuntime(ModelRuntime&&) noexcept;
    ModelRuntime& operator=(ModelRuntime&&) noexcept;
    ~ModelRuntime() override;
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
    [[nodiscard]] util::ExpectedVoid refresh();

    /// Config, composition, and availability diagnostics. Empty when healthy.
    [[nodiscard]] std::optional<std::string> get_error() const;

    // ── Provider / model lookup (live, delegating) ────────────────────────

    [[nodiscard]] std::vector<std::shared_ptr<ai::Provider>> providers() const;
    [[nodiscard]] std::shared_ptr<ai::Provider> provider(
        std::string_view provider_id) const;
    [[nodiscard]] std::vector<ai::Model> models(
        std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<ai::Model> model(
        std::string_view provider_id,
        std::string_view model_id) const;

    // ── Availability ───────────────────────────────────────────────────────

    /// Live availability: models whose provider has configured auth. Without a
    /// provider id this runs an availability refresh against the live runtime
    /// and refreshes the snapshot. Cached snapshots never replace live
    /// `get_auth`/`check_auth` semantics.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::Model>>> get_available(
        std::optional<std::string_view> provider_id = std::nullopt);
    /// Last availability snapshot (synchronous). Never replaces live semantics.
    [[nodiscard]] std::vector<ai::Model> get_available_snapshot() const;

    // ── Authentication ─────────────────────────────────────────────────────

    /// Side-effect-free; OAuth credentials are never refreshed.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> check_auth(
        std::string provider_id);
    /// Live authentication resolution (delegates to `ai::Models`).
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> get_auth(
        std::string provider_id,
        std::optional<std::string> explicit_api_key = std::nullopt);
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> get_auth(
        ai::Model model,
        std::optional<std::string> explicit_api_key = std::nullopt);

    /// True when the provider currently resolves as configured (snapshot).
    [[nodiscard]] bool has_configured_auth(std::string_view provider_id) const;
    /// True when the provider authenticates through an OAuth credential.
    [[nodiscard]] bool is_using_oauth(std::string_view provider_id) const;
    /// Source/type-only auth status for status UI (never triggers network).
    [[nodiscard]] std::optional<ModelRuntimeAuthStatus> get_provider_auth_status(
        std::string_view provider_id) const;
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> list_credentials();

    // ── Login / logout ─────────────────────────────────────────────────────

    /// `Models::login` + `refresh()`. A post-login refresh failure is recorded
    /// in the composition-errors map and never fails the login call.
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::Credential>> login(
        std::string provider_id,
        ai::AuthType type,
        ai::AuthInteraction interaction);

    /// `Models::logout` + `recomposeProvider` + `refresh()` (order preserved).
    /// A post-logout refresh failure is recorded and never fails the logout.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> logout(
        std::string provider_id);

    // ── Streaming (delegation) ─────────────────────────────────────────────

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink);
    /// The borrowed request must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override;

    // ── Provider registration ──────────────────────────────────────────────

    /// Register a native provider used exactly as given (pi
    /// `registerNativeProvider`). Replaces any built-in or composed provider
    /// with the same id. Empty provider ids are rejected.
    [[nodiscard]] util::ExpectedVoid register_native_provider(
        std::shared_ptr<ai::Provider> provider);

    /// Env var names referenced by configured models.json `apiKey` templates
    /// (pi `getConfigValueEnvVarNames`). Used for execution environment secret
    /// filtering.
    [[nodiscard]] std::vector<std::string> configured_api_key_env_names() const;

    /// Frozen default-model table for the supported provider subset.
    [[nodiscard]] static std::optional<std::string> default_model_for_provider(
        std::string_view provider_id);

    struct Impl;

private:
    explicit ModelRuntime(std::unique_ptr<Impl> impl);
    /// Private test-support assembly: build a runtime around an externally
    /// constructed `ai::Models` without re-composing providers. The Models
    /// stays privately held; the runtime delegates everything to it. Used by
    /// the private session test seam only.
    static util::Expected<std::shared_ptr<ModelRuntime>> create_from_models_for_testing(
        std::shared_ptr<ai::Models> models,
        ModelRuntimeOptions options = {});

    friend class runtime::SessionFactory;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent
