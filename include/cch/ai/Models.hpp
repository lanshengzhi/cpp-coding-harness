#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/ModelStream.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

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

    [[nodiscard]] util::ExpectedVoid set_provider(std::shared_ptr<Provider> provider);
    void delete_provider(std::string_view provider_id);
    void clear_providers();

    [[nodiscard]] std::vector<std::shared_ptr<Provider>> providers() const;
    [[nodiscard]] std::shared_ptr<Provider> provider(std::string_view provider_id) const;
    [[nodiscard]] std::vector<Model> models(std::optional<std::string_view> provider_id = std::nullopt) const;
    [[nodiscard]] std::optional<Model> model(
        std::string_view provider_id,
        std::string_view model_id) const;

    /// Side-effect-free authentication status. OAuth credentials are never
    /// refreshed by this operation.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthCheck>>> check_auth(
        std::string provider_id);

    /// Live authentication resolution. OAuth refresh is double-checked and
    /// persisted through CredentialStore::modify immediately before use.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> get_auth(
        std::string provider_id,
        std::optional<std::string> explicit_api_key = std::nullopt);
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> get_auth(
        Model model,
        std::optional<std::string> explicit_api_key = std::nullopt);

    /// Remove a locally stored credential. No remote revocation is attempted.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> logout(
        std::string provider_id);

    /// Run a provider-owned login flow and persist its returned credential via
    /// CredentialStore::modify, the only write path. Login-flow failures
    /// propagate unwrapped to the host; only CredentialStore failures wrap as
    /// the `auth` category. Login errors never enter the stream error channel.
    [[nodiscard]] boost::asio::awaitable<util::Expected<Credential>> login(
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

/// Boost.Asio bridge for consuming a `ModelStream` from within an asio
/// coroutine (the Agent's serialized domain). Events flow through `sink` in
/// order before exactly one terminal `std::expected<AssistantMessage, Error>`.
/// The `ModelStream` value itself never names a third-party execution type;
/// this bridge is the one place the private executor association lives.
[[nodiscard]] boost::asio::awaitable<std::expected<AssistantMessage, cch::support::Error>>
consume(ModelStream stream, AssistantEventSink sink);

/// Produces one move-only `ModelStream` per call. The closure deliberately
/// shares the Models Runtime lifetime (ADR 0040). The executor capture stays
/// private to `ai::Models`; the Agent consumes the resulting `ModelStream`
/// value without naming a third-party execution type.
using ModelStreamFactory = std::move_only_function<
    ModelStream(Model, AiContext, SimpleStreamOptions)>;

} // namespace cch::ai
