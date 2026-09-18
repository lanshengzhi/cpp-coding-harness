#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/Headers.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/Timestamps.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/ProviderTestAccess.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "SimpleOptions.hpp"
#include "support/BoundedText.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#include <exception>
#endif
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ModelsImpl.hpp"

namespace cch::ai {
namespace {

[[nodiscard]] std::vector<Model> safe_provider_models(const Provider& provider_value) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
        return provider_value.models();
    } catch (...) {
        return {};
    }
#else
    return provider_value.models();
#endif
}

[[nodiscard]] std::shared_ptr<Provider> make_default_provider(ProviderDefinition definition) {
    auto http_transport = std::make_shared<providers::BoostBeastStreamTransport>();
    auto websocket_transport = std::make_shared<providers::BoostBeastWebSocketTransport>();
    return providers::make_composed_provider(std::move(definition.id),
            std::move(definition.name),
            std::move(definition.models),
            std::move(definition.auth),
            std::move(http_transport),
            std::move(websocket_transport));
}

} // namespace

support::ExpectedVoid providers::ProviderTestAccess::install(Models& models, std::shared_ptr<Provider> provider) {
    return models.impl_->install_provider(std::move(provider));
}

support::ExpectedVoid providers::ProviderTestAccess::replace_transports(Models& models,
        std::shared_ptr<StreamTransport> http_transport,
        std::shared_ptr<WebSocketTransport> ws_transport,
        CodexWebSocketCacheConfig cache_config) {
    for (const auto& [provider_id, provider] : models.impl_->providers) {
        auto replacement = providers::make_composed_provider(provider_id,
                std::string{provider->name()},
                provider->models(),
                std::move(provider->auth()),
                http_transport,
                ws_transport,
                cache_config);
        if (auto installed = models.impl_->install_provider(std::move(replacement)); !installed) {
            return std::unexpected(installed.error());
        }
    }
    return {};
}

Models::Models(
    std::shared_ptr<CredentialStore> credentials,
    std::shared_ptr<AuthContext> auth_context)
    : impl_(std::make_unique<Impl>(Impl{
          .credentials = std::move(credentials),
          .auth_context = std::move(auth_context),
          .providers = {},
      })) {}

Models::Models(Models&&) noexcept = default;
Models& Models::operator=(Models&&) noexcept = default;
Models::~Models() = default;

support::ExpectedVoid Models::apply_provider(ProviderChange change) {
    if (change.definition) {
        auto definition = std::move(*change.definition);
        if (definition.id.empty()) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
        }
        if (!change.provider_id.empty() && change.provider_id != definition.id) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider,
                    "provider change id does not match provider definition",
                    definition.id));
        }
        return impl_->install_provider(make_default_provider(std::move(definition)));
    }
    if (change.provider_id.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
    }
    impl_->remove_provider(change.provider_id);
    return {};
}

void Models::clear_providers() {
    impl_->providers.clear();
}

std::vector<ProviderInfo> Models::provider_info() const {
    std::vector<ProviderInfo> result;
    result.reserve(impl_->providers.size());
    for (const auto& [_, provider_value] : impl_->providers) {
        ProviderInfo info{
                .id = std::string{provider_value->id()},
                .name = std::string{provider_value->name()},
                .auth_methods = {},
        };
        const auto& auth = provider_value->auth();
        // Keep the existing login presentation order: OAuth before API key.
        if (auth.oauth) {
            info.auth_methods.push_back(AuthMethodInfo{
                    .type = AuthType::OAuth,
                    .name = auth.oauth->name,
                    .has_login = static_cast<bool>(auth.oauth->login),
            });
        }
        if (auth.api_key) {
            info.auth_methods.push_back(AuthMethodInfo{
                    .type = AuthType::ApiKey,
                    .name = auth.api_key->name,
                    .has_login = static_cast<bool>(auth.api_key->login),
            });
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<Model> Models::models(std::optional<std::string_view> provider_id) const {
    if (provider_id) {
        const auto selected = impl_->provider(*provider_id);
        return selected ? safe_provider_models(*selected) : std::vector<Model>{};
    }
    std::vector<Model> result;
    for (const auto& [_, provider_value] : impl_->providers) {
        auto current = safe_provider_models(*provider_value);
        result.insert(
            result.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    return result;
}

std::optional<Model> Models::model(
    std::string_view provider_id,
    std::string_view model_id) const {
    auto available = models(provider_id);
    const auto found = std::find_if(
        available.begin(), available.end(), [&model_id](const Model& value) {
            return value.id == model_id;
        });
    if (found == available.end()) {
        return std::nullopt;
    }
    return *found;
}

} // namespace cch::ai
