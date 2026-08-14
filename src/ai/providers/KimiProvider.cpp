#include "KimiProvider.hpp"

#include "KimiCatalog.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/api/AnthropicMessagesAdapter.hpp"
#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::providers {
namespace {

class KimiCodingProvider final : public ai::Provider {
public:
    KimiCodingProvider(
        ProviderAuth auth,
        std::shared_ptr<StreamTransport> transport)
        : models_(kimi_coding_models()),
          adapter_(std::move(transport)),
          auth_(std::move(auth)) {}
    KimiCodingProvider(KimiCodingProvider&&) noexcept = default;
    KimiCodingProvider& operator=(KimiCodingProvider&&) noexcept = default;
    ~KimiCodingProvider() override = default;
    KimiCodingProvider(const KimiCodingProvider&) = delete;
    KimiCodingProvider& operator=(const KimiCodingProvider&) = delete;

    [[nodiscard]] std::string_view id() const noexcept override {
        return "kimi-coding";
    }
    [[nodiscard]] std::string_view name() const noexcept override {
        return "Kimi For Coding";
    }
    [[nodiscard]] ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<Model> models() const override { return models_; }

    /// One move-only model stream backed by the anthropic-messages adapter.
    [[nodiscard]] ModelStream stream(
        Model model,
        AiContext context,
        ProviderStreamOptions options) override {
        return detail::make_model_stream(
            [this,
             model = std::move(model),
             context = std::move(context),
             options = std::move(options)](
                AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<AssistantMessage>> {
                if (model.api != "anthropic-messages") {
                    co_return std::unexpected(util::make_error(
                        util::ErrorCode::Stream,
                        "Provider kimi-coding has no API implementation for \"" +
                            model.api + "\""));
                }
                CCH_TRY(message, co_await adapter_.stream(
                    model, context, std::move(options), std::move(sink)));
                co_return message;
            });
    }

private:
    std::vector<Model> models_;
    api::AnthropicMessagesAdapter adapter_;
    ProviderAuth auth_;
};

} // namespace

std::shared_ptr<ai::Provider> make_kimi_coding_provider(
    ProviderAuth auth,
    std::shared_ptr<StreamTransport> transport) {
    return std::make_shared<KimiCodingProvider>(
        std::move(auth),
        std::move(transport));
}

} // namespace cch::ai::providers
