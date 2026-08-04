#pragma once

#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "coding_agent/runtime/SessionFactory.hpp"
#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {
namespace detail {

class FixtureCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> read(
        std::string) override {
        co_return std::optional<ai::Credential>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> list() override {
        co_return std::vector<ai::CredentialInfo>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> modify(
        std::string,
        ai::CredentialModifyHook) override {
        co_return std::optional<ai::Credential>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> remove(std::string) override {
        co_return util::ExpectedVoid{};
    }
};

class FixtureAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string) const override {
        co_return std::optional<std::string>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string) const override {
        co_return false;
    }
};

inline ai::ProviderAuth fixture_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "test fixture";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> {
        co_return ai::AuthCheck{.source = "test fixture", .type = ai::AuthType::ApiKey};
    };
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{.source = "test fixture"};
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

class FixtureProvider final : public ai::Provider {
public:
    FixtureProvider(
        std::string provider_id,
        std::shared_ptr<ai::StreamingChatClient> stream)
        : provider_id_(std::move(provider_id)),
          stream_(std::move(stream)),
          auth_(fixture_auth()) {}

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return provider_id_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        ai::StreamChatRequest request;
        request.context = context;
        request.stop_token = options.stop_token;
        request.model = model;
        CCH_TRY(message, co_await stream_->stream(request, std::move(sink)));
        co_return message;
    }

private:
    std::string provider_id_;
    std::shared_ptr<ai::StreamingChatClient> stream_;
    ai::ProviderAuth auth_;
};

} // namespace detail

inline std::shared_ptr<ai::Models> models_from_stream(
    std::unique_ptr<ai::StreamingChatClient> stream,
    std::string provider_id = "sdk-host") {
    auto models = std::make_shared<ai::Models>(
        std::make_shared<detail::FixtureCredentialStore>(),
        std::make_shared<detail::FixtureAuthContext>());
    auto shared_stream = std::shared_ptr<ai::StreamingChatClient>{std::move(stream)};
    std::vector<std::string> provider_ids{
        std::move(provider_id),
        "sdk-host",
        "fake",
    };
    for (const auto& id : provider_ids) {
        if (models->provider(id)) {
            continue;
        }
        if (auto added = models->set_provider(
                std::make_shared<detail::FixtureProvider>(id, shared_stream)); !added) {
            return nullptr;
        }
    }
    return models;
}

/// Test-only carrier for SessionFactory's private Models assembly seam.
struct ModelsSessionOptions : coding_agent::CreateAgentSessionOptions {
    std::shared_ptr<ai::Models> models;
};

/// Request Model for SDK session tests that no longer set `provider_config`:
/// a complete, credential-free shape carried through the scripted fake seam.
inline ai::Model sdk_request_model(
    std::string provider,
    std::string model_id,
    std::optional<std::string> base_url = std::nullopt) {
    ai::Model model;
    model.id = std::move(model_id);
    model.name = model.id;
    model.api = "scripted-fake";
    model.provider = std::move(provider);
    model.base_url = base_url.value_or("");
    model.reasoning = false;
    model.thinking_level_map = std::nullopt;
    model.input = {ai::ModelInput::Text};
    model.cost = {};
    model.context_window = 128000;
    model.max_tokens = 16384;
    model.headers = std::nullopt;
    model.compat = std::nullopt;
    return model;
}

inline util::Expected<coding_agent::CreateAgentSessionResult> create_agent_session(
    ModelsSessionOptions options) {
    auto models = std::move(options.models);
    coding_agent::CreateAgentSessionOptions public_options = std::move(options);
    if (models) {
        return coding_agent::create_agent_session_for_testing(
            std::move(public_options), std::move(models));
    }
    return coding_agent::create_agent_session(std::move(public_options));
}

inline util::Expected<coding_agent::CreateAgentSessionResult> create_agent_session(
    ModelsSessionOptions options,
    std::unique_ptr<coding_agent::runtime::AsyncUserShell> user_shell) {
    auto models = std::move(options.models);
    coding_agent::CreateAgentSessionOptions public_options = std::move(options);
    return coding_agent::create_agent_session_for_testing(
        std::move(public_options), std::move(models), std::move(user_shell));
}

} // namespace cch::tests

namespace cch::coding_agent {

inline util::Expected<CreateAgentSessionResult> create_agent_session(
    tests::ModelsSessionOptions options) {
    return tests::create_agent_session(std::move(options));
}

inline util::Expected<CreateAgentSessionResult> create_agent_session(
    tests::ModelsSessionOptions options,
    std::unique_ptr<runtime::AsyncUserShell> user_shell) {
    return tests::create_agent_session(
        std::move(options), std::move(user_shell));
}

} // namespace cch::coding_agent
