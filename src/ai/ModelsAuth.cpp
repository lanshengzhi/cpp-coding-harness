#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/Headers.hpp"
#include "ModelsImpl.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::ai {
namespace {

[[nodiscard]] std::string_view auth_type_string(AuthType type) {
    switch (type) {
    case AuthType::ApiKey:
        return "api_key";
    case AuthType::OAuth:
        return "oauth";
    }
    return "oauth";
}

/// Stored-credential read that categorizes store failures as auth failures.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<Credential>>> read_stored(
        Models::Impl& impl, const std::string& provider_id) {
    auto stored = co_await invoke_async_operation([&]() { return impl.credentials->read(provider_id); });
    if (!stored) {
        co_return std::unexpected(categorized_error(
                support::ErrorCode::Auth, "Credential store read failed for " + provider_id, stored.error()));
    }
    co_return std::move(*stored);
}

/// API-key auth check shared by both check_auth branches (with and without a
/// stored credential).
[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> check_api_key(
        Models::Impl& impl, Provider& provider, std::optional<ApiKeyCredential> credential) {
    auto& auth = provider.auth();
    const auto provider_id = std::string{provider.id()};
    if (!auth.api_key) {
        co_return std::optional<AuthCheck>{};
    }
    if (auth.api_key->check) {
        auto checked = co_await invoke_async_operation(
                [&]() { return auth.api_key->check(*impl.auth_context, std::move(credential)); });
        if (!checked) {
            co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                    "API key auth check failed for provider " + provider_id,
                    checked.error()));
        }
        co_return std::move(*checked);
    }
    CCH_TRY(resolved, co_await impl.resolve_api_key(provider, *auth.api_key, std::move(credential)));
    if (!resolved) {
        co_return std::optional<AuthCheck>{};
    }
    co_return AuthCheck{
            .source = resolved->source,
            .type = AuthType::ApiKey,
    };
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> check_auth_impl(
        Models::Impl& impl, std::string provider_id) {
    const auto selected = impl.provider(provider_id);
    if (!selected) {
        co_return std::optional<AuthCheck>{};
    }

    CCH_TRY(stored, co_await read_stored(impl, provider_id));
    if (stored) {
        if (std::holds_alternative<OAuthCredential>(*stored)) {
            if (!selected->auth().oauth) {
                co_return std::optional<AuthCheck>{};
            }
            co_return AuthCheck{.source = "OAuth", .type = AuthType::OAuth};
        }
        co_return co_await check_api_key(impl, *selected, std::get<ApiKeyCredential>(*stored));
    }
    co_return co_await check_api_key(impl, *selected, std::nullopt);
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> get_auth_impl(
        Models::Impl& impl, std::string provider_id, std::optional<std::string> explicit_api_key) {
    const auto selected = impl.provider(provider_id);
    if (!selected) {
        co_return std::optional<AuthResult>{};
    }
    auto& auth = selected->auth();

    if (explicit_api_key && auth.api_key) {
        ApiKeyCredential credential;
        credential.key = std::move(explicit_api_key);
        co_return co_await impl.resolve_api_key(*selected, *auth.api_key, std::move(credential));
    }

    CCH_TRY(stored, co_await read_stored(impl, provider_id));
    if (stored) {
        if (auto* oauth = std::get_if<OAuthCredential>(&*stored)) {
            if (!auth.oauth) {
                co_return std::optional<AuthResult>{};
            }
            co_return co_await impl.resolve_oauth(*selected, *auth.oauth, *oauth);
        }
        if (!auth.api_key) {
            co_return std::optional<AuthResult>{};
        }
        co_return co_await impl.resolve_api_key(*selected, *auth.api_key, std::get<ApiKeyCredential>(*stored));
    }

    if (!auth.api_key) {
        co_return std::optional<AuthResult>{};
    }
    co_return co_await impl.resolve_api_key(*selected, *auth.api_key, std::nullopt);
}

[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> logout_impl(Models::Impl& impl, std::string provider_id) {
    auto removed = co_await invoke_async_operation([&]() { return impl.credentials->remove(provider_id); });
    if (!removed) {
        co_return std::unexpected(categorized_error(
                support::ErrorCode::Auth, "Credential store delete failed for " + provider_id, removed.error()));
    }
    co_return support::ExpectedVoid{};
}

/// CredentialStore::modify callback that unconditionally overwrites the
/// stored credential with the fresh login result.
[[nodiscard]] cch::support::AsyncResult<std::optional<Credential>> overwrite_with(Credential credential) {
    return cch::support::AsyncResult<std::optional<Credential>>(
            std::expected<std::optional<Credential>, cch::support::Error>{
                    std::optional<Credential>{std::move(credential)}});
}

[[nodiscard]] boost::asio::awaitable<support::Expected<Credential>> login_impl(
        Models::Impl& impl, std::string provider_id, AuthType type, AuthInteraction interaction) {
    const auto selected = impl.provider(provider_id);
    if (!selected) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Provider, "Unknown provider: " + provider_id));
    }
    auto& auth = selected->auth();
    const auto type_name = std::string{auth_type_string(type)};

    Credential login_credential;
    if (type == AuthType::OAuth) {
        if (!auth.oauth || !auth.oauth->login) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                    std::string{selected->name()} + " does not support " + type_name + " login"));
        }
        auto credential = co_await invoke_async_operation([&]() { return auth.oauth->login(std::move(interaction)); });
        if (!credential) {
            // Login-flow failures propagate unwrapped to the host.
            co_return std::unexpected(std::move(credential.error()));
        }
        login_credential = std::move(*credential);
    } else {
        if (!auth.api_key || !auth.api_key->login) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                    std::string{selected->name()} + " does not support " + type_name + " login"));
        }
        auto credential =
                co_await invoke_async_operation([&]() { return auth.api_key->login(std::move(interaction)); });
        if (!credential) {
            co_return std::unexpected(std::move(credential.error()));
        }
        login_credential = std::move(*credential);
    }

    // Persist exclusively through CredentialStore::modify, the only write
    // path. Only CredentialStore failures wrap as the `auth` category.
    auto stored = co_await invoke_async_operation([&]() -> cch::support::AsyncResult<std::optional<Credential>> {
        return impl.credentials->modify(
                provider_id, [&](std::optional<Credential>) { return overwrite_with(login_credential); });
    });
    if (!stored) {
        co_return std::unexpected(categorized_error(
                support::ErrorCode::Auth, "Credential store modify failed for " + provider_id, stored.error()));
    }
    co_return std::move(login_credential);
}

} // namespace

cch::support::AsyncResult<std::optional<AuthCheck>> Models::check_auth(std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]() { return check_auth_impl(*impl_, std::move(provider_id)); });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
        std::string provider_id, std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id), explicit_api_key = std::move(explicit_api_key)]() {
                return get_auth_impl(*impl_, std::move(provider_id), std::move(explicit_api_key));
            });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
        Model model_value, std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, model_value = std::move(model_value), explicit_api_key = std::move(explicit_api_key)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                CCH_TRY(resolved,
                        co_await support::detail::await_async_result(
                                get_auth(model_value.provider, std::move(explicit_api_key))));
                if (!resolved || !model_value.headers) {
                    co_return resolved;
                }
                merge_headers(resolved->auth.headers, *model_value.headers);
                co_return resolved;
            });
}

cch::support::AsyncResult<void> Models::logout(std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]() { return logout_impl(*impl_, std::move(provider_id)); });
}

cch::support::AsyncResult<Credential> Models::login(
        std::string provider_id, AuthType type, AuthInteraction interaction) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id), type, interaction = std::move(interaction)]() mutable {
                return login_impl(*impl_, std::move(provider_id), type, std::move(interaction));
            });
}

} // namespace cch::ai
