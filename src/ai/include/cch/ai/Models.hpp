#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/ModelStream.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

/// Complete pre-runtime description of one Provider. The authentication hooks
/// make this value move-only; transports and stream capabilities are assembled
/// privately by Models.
struct ProviderDefinition {
    std::string id{};
    std::string name{};
    std::vector<Model> models{};
    ProviderAuth auth{};
};

/// One Provider installation or removal. A present definition installs or
/// replaces the provider identified by `provider_id`; an absent definition
/// removes that provider. The id is repeated for installation so a change has
/// one explicit target in either form.
struct ProviderChange {
    std::string provider_id{};
    std::optional<ProviderDefinition> definition{std::nullopt};
};

/// Passive summary of one authentication method exposed by Provider Info.
struct AuthMethodInfo {
    AuthType type{AuthType::ApiKey};
    std::string name{};
    bool has_login{false};
};

/// Passive projection of one installed Provider for hosts and frontends.
struct ProviderInfo {
    std::string id{};
    std::string name{};
    std::vector<AuthMethodInfo> auth_methods{};
};

/// Runtime collection of long-lived Providers plus live authentication and
/// request-time stream delegation.
///
/// Concurrency contract: `Models` and the Providers it owns are not internally
/// synchronized; Providers hold long-lived state (e.g. the Codex WebSocket
/// session cache) and adapters are driven by the calling executor. All
/// operations on one `Models`/`Provider` graph must be driven by a
/// single-threaded executor or otherwise serialized; do not drive the same
/// graph from two threads. One runtime is shared across sessions (ADR 0029),
/// so callers must serialize every session's calls onto one executor.
class Models final : public std::enable_shared_from_this<Models> {
public:
    Models(
        std::shared_ptr<CredentialStore> credentials,
        std::shared_ptr<AuthContext> auth_context);
    Models(Models&&) noexcept;
    Models& operator=(Models&&) noexcept;
    ~Models();
    Models(const Models&) = delete;
    Models& operator=(const Models&) = delete;

    /// Install or remove one provider while keeping construction private to
    /// the AI Owner. A missing provider removal succeeds as a no-op.
    [[nodiscard]] cch::support::ExpectedVoid apply_provider(ProviderChange change);

    // Transitional Provider-pointer surface; removed once downstream callers
    // migrate to ProviderDefinition and ProviderInfo (ADR 0047).
    [[nodiscard]] cch::support::ExpectedVoid set_provider(std::shared_ptr<Provider> provider);
    void delete_provider(std::string_view provider_id);
    void clear_providers();

    [[nodiscard]] std::vector<ProviderInfo> provider_info() const;
    [[nodiscard]] std::vector<std::shared_ptr<Provider>> providers() const;
    [[nodiscard]] std::shared_ptr<Provider> provider(std::string_view provider_id) const;
    [[nodiscard]] std::vector<Model> models(std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<Model> model(
        std::string_view provider_id,
        std::string_view model_id) const;

    /// Side-effect-free authentication status. OAuth credentials are never
    /// refreshed by this operation.
    [[nodiscard]] cch::support::AsyncResult<std::optional<AuthCheck>> check_auth(
        std::string provider_id);

    /// Live authentication resolution. OAuth refresh is double-checked and
    /// persisted through CredentialStore::modify immediately before use.
    [[nodiscard]] cch::support::AsyncResult<std::optional<AuthResult>> get_auth(
        std::string provider_id,
        std::optional<std::string> explicit_api_key = std::nullopt);
    [[nodiscard]] cch::support::AsyncResult<std::optional<AuthResult>> get_auth(
        Model model,
        std::optional<std::string> explicit_api_key = std::nullopt);

    /// Remove a locally stored credential. No remote revocation is attempted.
    [[nodiscard]] cch::support::AsyncResult<void> logout(
        std::string provider_id);

    /// Run a provider-owned login flow and persist its returned credential via
    /// CredentialStore::modify, the only write path. Login-flow failures
    /// propagate unwrapped to the host; only CredentialStore failures wrap as
    /// the `auth` category. Login errors never enter the stream error channel.
    [[nodiscard]] cch::support::AsyncResult<Credential> login(
        std::string provider_id,
        AuthType type,
        AuthInteraction interaction);

    /// Produce one AI-owned move-only `ModelStream` for a single turn
    /// (ADR 0040 / #455). The returned value is consumed exactly once through
    /// `run`/`start`/`consume`; its closure shares this Models Runtime's
    /// lifetime and captures the consuming executor privately. The sink is
    /// supplied at consumption, keeping `ModelStream` composable and free of
    /// third-party execution types. Model/provider/auth/request failures
    /// normalize to one terminal event and a final AssistantMessage value;
    /// only sink or infrastructure failure uses the Expected error alternative.
    [[nodiscard]] ModelStream stream(
        Model model,
        AiContext context,
        SimpleStreamOptions options);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

/// Produces one move-only `ModelStream` per call. The closure deliberately
/// shares the Models Runtime lifetime (ADR 0040). The executor capture stays
/// private to `ai::Models`; the Agent consumes the resulting `ModelStream`
/// value without naming a third-party execution type.
using ModelStreamFactory = std::move_only_function<
    ModelStream(Model, AiContext, SimpleStreamOptions)>;

} // namespace cch::ai
