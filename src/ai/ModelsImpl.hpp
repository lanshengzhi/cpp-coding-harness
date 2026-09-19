#pragma once

#include <cch/ai/Models.hpp>

#include "ModelsInternal.hpp"

#include "support/ExpectedMacros.hpp"

#include "providers/Provider.hpp"

#include <boost/asio/awaitable.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai {

/// Models Runtime state: the provider registry, credential store handle, and
/// the API-key/OAuth auth resolution pipeline. Shared by the registry, auth,
/// and stream translation units.

struct Models::Impl {
    std::shared_ptr<CredentialStore> credentials;
    std::shared_ptr<AuthContext> auth_context;
    std::map<std::string, std::shared_ptr<Provider>, std::less<>> providers;

    [[nodiscard]] std::shared_ptr<Provider> provider(std::string_view provider_id) const {
        const auto found = providers.find(provider_id);
        return found == providers.end() ? nullptr : found->second;
    }

    [[nodiscard]] support::ExpectedVoid install_provider(std::shared_ptr<Provider> provider_value) {
        if (!provider_value || provider_value->id().empty()) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
        }
        const auto& auth = provider_value->auth();
        if (!auth.api_key && !auth.oauth) {
            return std::unexpected(support::make_error(support::ErrorCode::Auth,
                    "provider must define authentication",
                    std::string{provider_value->id()}));
        }
        providers.insert_or_assign(std::string{provider_value->id()}, std::move(provider_value));
        return {};
    }

    void remove_provider(std::string_view provider_id) { providers.erase(provider_id); }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> resolve_api_key(
            const Provider& provider, ApiKeyAuth& auth, std::optional<ApiKeyCredential> credential) {
        auto resolved =
                co_await invoke_async_operation([&]() { return auth.resolve(*auth_context, std::move(credential)); });
        if (!resolved) {
            co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                    "API key auth failed for provider " + std::string{provider.id()},
                    resolved.error()));
        }
        co_return std::move(*resolved);
    }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> resolve_oauth(
            const Provider& provider, OAuthAuth& auth, OAuthCredential credential) {
        const std::string provider_id{provider.id()};
        if (expires_soon(credential)) {
            CCH_TRY(refreshed, co_await refresh_if_due(auth, provider_id));
            if (!refreshed) {
                // A concurrent writer already refreshed the stored credential;
                // pi resolves to "no auth" rather than racing a second refresh.
                co_return std::optional<AuthResult>{};
            }
            credential = std::move(*refreshed);
        }

        auto request_auth = co_await invoke_async_operation([&]() { return auth.to_auth(credential); });
        if (!request_auth) {
            co_return std::unexpected(categorized_error(support::ErrorCode::OAuth,
                    "OAuth auth derivation failed for " + provider_id,
                    request_auth.error()));
        }
        co_return AuthResult{
                .auth = std::move(*request_auth),
                .env = {},
                .source = "OAuth",
        };
    }

private:
    /// CredentialStore::modify callback for the OAuth refresh: refreshes only
    /// a still-stored, still-expiring OAuth credential and yields an empty
    /// optional when a concurrent writer already handled it.
    [[nodiscard]] cch::support::AsyncResult<std::optional<Credential>> refresh_stored(
            OAuthAuth& auth, std::string provider_id, std::optional<Credential> current) {
        return support::detail::make_async_result(
                [&auth, provider_id = std::move(provider_id), current = std::move(current)]()
                        -> boost::asio::awaitable<support::Expected<std::optional<Credential>>> {
                    auto* current_oauth = current ? std::get_if<OAuthCredential>(&*current) : nullptr;
                    if (current_oauth == nullptr || !expires_soon(*current_oauth)) {
                        co_return std::optional<Credential>{};
                    }
                    auto refreshed = co_await invoke_async_operation([&]() { return auth.refresh(*current_oauth); });
                    if (!refreshed) {
                        co_return std::unexpected(categorized_error(support::ErrorCode::OAuth,
                                "OAuth refresh failed for " + provider_id,
                                refreshed.error()));
                    }
                    co_return std::optional<Credential>{Credential{std::move(*refreshed)}};
                });
    }

    /// The expiring-credential refresh round trip through the credential
    /// store. OAuth-category failures propagate unwrapped; other store
    /// failures are categorized as auth failures.
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<OAuthCredential>>> refresh_if_due(
            OAuthAuth& auth, std::string provider_id) {
        auto modified = co_await invoke_async_operation([&]() {
            return credentials->modify(provider_id, [&](std::optional<Credential> current) {
                return refresh_stored(auth, provider_id, std::move(current));
            });
        });
        if (!modified) {
            if (modified.error().code == support::ErrorCode::OAuth) {
                co_return std::unexpected(modified.error());
            }
            co_return std::unexpected(categorized_error(
                    support::ErrorCode::Auth, "Credential store modify failed for " + provider_id, modified.error()));
        }
        if (!*modified) {
            co_return std::optional<OAuthCredential>{};
        }
        const auto* refreshed = std::get_if<OAuthCredential>(&**modified);
        if (refreshed == nullptr) {
            co_return std::optional<OAuthCredential>{};
        }
        co_return std::optional<OAuthCredential>{*refreshed};
    }
};

} // namespace cch::ai
