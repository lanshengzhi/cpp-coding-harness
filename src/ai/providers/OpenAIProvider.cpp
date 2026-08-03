#include "OpenAIProvider.hpp"

#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::ai::providers {
namespace {

/// Borrowed context and environment_names must outlive the returned awaitable.
[[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> resolve_api_key(
    const AuthContext& context,
    std::optional<ApiKeyCredential> credential,
    const std::vector<std::string>& environment_names) {
    if (credential && credential->key && !credential->key->empty()) {
        ProviderEnv env;
        env.insert(credential->env.begin(), credential->env.end());
        co_return AuthResult{
            .auth = ModelAuth{.api_key = *credential->key},
            .env = std::move(env),
            .source = "stored credential",
        };
    }
    for (const auto& name : environment_names) {
        CCH_TRY(value, co_await context.environment(name));
        if (value && !value->empty()) {
            co_return AuthResult{
                .auth = ModelAuth{.api_key = *value},
                .env = {},
                .source = name,
            };
        }
    }
    co_return std::optional<AuthResult>{};
}

class OpenAICompatibleProvider final : public ai::Provider {
public:
    OpenAICompatibleProvider(
        std::string provider_id,
        std::vector<ai::Model> models,
        std::vector<std::string> api_key_env,
        std::shared_ptr<StreamTransport> transport,
        OpenAIStreamConfig config)
        : provider_id_(std::move(provider_id)),
          models_(std::move(models)),
          stream_(std::move(transport), std::move(config)) {
        ApiKeyAuth api_key;
        api_key.name = "API key";
        api_key.check = [environment_names = api_key_env](
                            const AuthContext& context,
                            std::optional<ApiKeyCredential> credential)
            -> boost::asio::awaitable<util::Expected<std::optional<AuthCheck>>> {
            CCH_TRY(resolved, co_await resolve_api_key(
                context, std::move(credential), environment_names));
            if (!resolved) {
                co_return std::optional<AuthCheck>{};
            }
            co_return AuthCheck{
                .source = resolved->source,
                .type = AuthType::ApiKey,
            };
        };
        api_key.resolve = [environment_names = std::move(api_key_env)](
                              const AuthContext& context,
                              std::optional<ApiKeyCredential> credential)
            -> boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> {
            CCH_TRY(resolved, co_await resolve_api_key(
                context, std::move(credential), environment_names));
            co_return resolved;
        };
        auth_.api_key = std::move(api_key);
    }

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return provider_id_; }
    [[nodiscard]] ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<Model> models() const override { return models_; }

    /// Borrowed model and context must outlive the returned awaitable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> stream(
        const Model& model,
        const AiContext& context,
        ProviderStreamOptions options,
        AssistantEventSink sink) override {
        if (model.api != "openai-completions") {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Stream,
                "Provider " + provider_id_ +
                    " has no API implementation for \"" + model.api + "\""));
        }
        StreamChatRequest request;
        request.context = context;
        request.stop_token = options.stop_token;
        request.model = model;
        auto result = co_await stream_.stream(
            request, std::move(options.auth), std::move(sink));
        if (!result &&
            (result.error().code == util::ErrorCode::JsonSerialize ||
             result.error().code == util::ErrorCode::Validation)) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Stream,
                result.error().message,
                result.error().detail,
                result.error().context));
        }
        CCH_TRY(message, std::move(result));
        co_return message;
    }

private:
    std::string provider_id_;
    std::vector<Model> models_;
    StreamingOpenAIChatClient stream_;
    ProviderAuth auth_;
};

} // namespace

std::shared_ptr<ai::Provider> make_openai_compatible_provider(
    std::string provider_id,
    std::vector<ai::Model> models,
    std::vector<std::string> api_key_env,
    std::shared_ptr<StreamTransport> transport,
    OpenAIStreamConfig config) {
    return std::make_shared<OpenAICompatibleProvider>(
        std::move(provider_id),
        std::move(models),
        std::move(api_key_env),
        std::move(transport),
        std::move(config));
}

} // namespace cch::ai::providers
