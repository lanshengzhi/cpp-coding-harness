#pragma once

#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "ai/ModelStreamBridge.hpp"
#include "ai/api/OpenAICodexResponsesAdapter.hpp"
#include "ai/api/OpenAIResponsesAdapter.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"
#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

/// Test-only Provider composition around a private wire adapter: the same
/// api-gated delegation the removed production factories provided, kept local
/// to the adapter test surface.
template <typename Adapter>
class AdapterProvider final : public ai::Provider {
public:
    template <typename... Args>
    AdapterProvider(
        std::string provider_id,
        std::string api,
        std::vector<ai::Model> models,
        std::vector<std::string> api_key_env,
        Args&&... adapter_args)
        : provider_id_(std::move(provider_id)),
          api_(std::move(api)),
          models_(std::move(models)),
          adapter_(std::forward<Args>(adapter_args)...),
          auth_(ai::providers::make_env_api_key_auth("API key", std::move(api_key_env))) {}

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return provider_id_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return models_; }

    /// One move-only model stream delegated to the private wire adapter.
    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override {
        return ai::detail::make_model_stream(
            [this,
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                if (model.api != api_) {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Stream,
                        "Provider " + provider_id_ +
                            " has no API implementation for \"" + model.api + "\""));
                }
                CCH_TRY(message, co_await adapter_.stream(
                    model, context, std::move(options), std::move(sink)));
                co_return message;
            });
    }

private:
    std::string provider_id_;
    std::string api_;
    std::vector<ai::Model> models_;
    Adapter adapter_;
    ai::ProviderAuth auth_;
};

[[nodiscard]] inline std::shared_ptr<ai::Provider> make_openai_responses_test_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<ai::providers::StreamTransport> transport) {
    return std::make_shared<AdapterProvider<ai::api::OpenAIResponsesAdapter>>(
        std::move(provider_id),
        "openai-responses",
        std::move(models),
        std::move(api_key_env),
        std::move(transport));
}

[[nodiscard]] inline std::shared_ptr<ai::Provider> make_openai_codex_responses_test_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<ai::providers::StreamTransport> http_transport,
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport,
    ai::providers::CodexWebSocketCacheConfig cache_config = {}) {
    return std::make_shared<AdapterProvider<ai::api::OpenAICodexResponsesAdapter>>(
        std::move(provider_id),
        "openai-codex-responses",
        std::move(models),
        std::move(api_key_env),
        std::move(http_transport),
        std::move(ws_transport),
        cache_config);
}

} // namespace cch::tests
