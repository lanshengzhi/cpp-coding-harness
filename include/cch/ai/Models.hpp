#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/ChatClient.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai {

/// Runtime collection of long-lived Providers plus live authentication and
/// request-time stream delegation.
class Models final : public StreamingChatClient {
public:
    Models(
        std::shared_ptr<CredentialStore> credentials,
        std::shared_ptr<AuthContext> auth_context);
    Models(Models&&) noexcept;
    Models& operator=(Models&&) noexcept;
    ~Models() override;
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

    /// Normalize model/provider/auth/request failures to one terminal event and
    /// a final AssistantMessage value. Only sink/infrastructure failure uses
    /// Expected's error alternative.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> stream_simple(
        Model model,
        AiContext context,
        SimpleStreamOptions options,
        AssistantEventSink sink);
    /// The borrowed request must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const StreamChatRequest& request,
        AssistantEventSink sink) override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::ai
