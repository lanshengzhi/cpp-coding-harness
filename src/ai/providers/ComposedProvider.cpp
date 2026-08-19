#include "ComposedProvider.hpp"

#include "ai/ModelStreamBridge.hpp"
#include "ai/api/AnthropicMessagesAdapter.hpp"
#include "ai/api/OpenAICodexResponsesAdapter.hpp"
#include "ai/api/OpenAIResponsesAdapter.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::providers {
namespace {

class ComposedProvider final
    : public ai::Provider,
      public std::enable_shared_from_this<ComposedProvider> {
public:
    ComposedProvider(
        std::string provider_id,
        std::string name,
        std::vector<ai::Model> models,
        ai::ProviderAuth auth,
        std::shared_ptr<StreamTransport> http_transport,
        std::shared_ptr<WebSocketTransport> ws_transport,
        CodexWebSocketCacheConfig cache_config)
        : provider_id_(std::move(provider_id)),
          name_(std::move(name)),
          models_(std::move(models)),
          auth_(std::move(auth)),
          responses_adapter_(http_transport),
          // The codex adapter takes the HTTP transport by value for its SSE
          // fallback; the anthropic adapter then takes the original so every
          // scoped adapter owns a usable transport (the anthropic adapter
          // must never receive the moved-from parameter).
          codex_adapter_(http_transport, std::move(ws_transport), cache_config),
          anthropic_adapter_(std::move(http_transport)) {}
    ComposedProvider(ComposedProvider&&) noexcept = default;
    ComposedProvider& operator=(ComposedProvider&&) noexcept = default;
    ~ComposedProvider() override = default;
    ComposedProvider(const ComposedProvider&) = delete;
    ComposedProvider& operator=(const ComposedProvider&) = delete;

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return models_; }

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override {
        return ai::detail::make_model_stream(
            [self = shared_from_this(),
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                if (model.api == "openai-responses") {
                    CCH_TRY(message, co_await self->responses_adapter_.stream(
                        model, context, std::move(options), std::move(sink)));
                    co_return message;
                }
                if (model.api == "openai-codex-responses") {
                    CCH_TRY(message, co_await self->codex_adapter_.stream(
                        model, context, std::move(options), std::move(sink)));
                    co_return message;
                }
                if (model.api == "anthropic-messages") {
                    CCH_TRY(message, co_await self->anthropic_adapter_.stream(
                        model, context, std::move(options), std::move(sink)));
                    co_return message;
                }
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Stream,
                    "Provider " + self->provider_id_ +
                        " has no API implementation for \"" + model.api + "\""));
            });
    }

private:
    std::string provider_id_;
    std::string name_;
    std::vector<ai::Model> models_;
    ai::ProviderAuth auth_;
    ai::api::OpenAIResponsesAdapter responses_adapter_;
    ai::api::OpenAICodexResponsesAdapter codex_adapter_;
    ai::api::AnthropicMessagesAdapter anthropic_adapter_;
};

} // namespace

std::shared_ptr<ai::Provider> make_composed_provider(
    std::string provider_id,
    std::string name,
    std::vector<ai::Model> models,
    ai::ProviderAuth auth,
    std::shared_ptr<StreamTransport> http_transport,
    std::shared_ptr<WebSocketTransport> ws_transport,
    CodexWebSocketCacheConfig cache_config) {
    return std::make_shared<ComposedProvider>(
        std::move(provider_id),
        std::move(name),
        std::move(models),
        std::move(auth),
        std::move(http_transport),
        std::move(ws_transport),
        cache_config);
}

} // namespace cch::ai::providers
