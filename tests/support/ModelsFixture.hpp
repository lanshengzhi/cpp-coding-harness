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

} // namespace detail

/// One recorded provider request — the post-`streamSimple` shape: the concrete
/// Model argument, the conversation context, and the per-turn options. The
/// legacy request aggregate is gone (ADR 0034 / #362); scripted provider fakes
/// record this instead.
struct RecordedProviderRequest {
    ai::Model model;
    ai::AiContext context;
    ai::ProviderStreamOptions options;
};

/// Base for scripted `ai::Provider` fakes used across the SDK/session/TUI/CLI
/// test suites: fixed identity and an always-configured API-key auth.
/// Subclasses implement the frozen `Provider::stream` surface only.
class ScriptedProvider : public ai::Provider {
public:
    explicit ScriptedProvider(std::string provider_id)
        : provider_id_(std::move(provider_id)), auth_(detail::fixture_auth()) {}

    [[nodiscard]] std::string_view id() const noexcept override { return provider_id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return provider_id_; }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return auth_; }
    [[nodiscard]] std::vector<ai::Model> models() const override { return {}; }

protected:
    const std::string& provider_id() const noexcept { return provider_id_; }
    ai::ProviderAuth& provider_auth() noexcept { return auth_; }

private:
    std::string provider_id_;
    ai::ProviderAuth auth_;
};

namespace detail {

/// Registers one scripted provider under an alias id. `Models::set_provider`
/// keys by `provider->id()`, so the canonical "sdk-host"/"fake" aliases the
/// session layer may query share the same underlying fake provider.
class IdAliasProvider final : public ai::Provider {
public:
    IdAliasProvider(std::string id, std::shared_ptr<ai::Provider> inner)
        : id_(std::move(id)), inner_(std::move(inner)) {}

    [[nodiscard]] std::string_view id() const noexcept override { return id_; }
    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] ai::ProviderAuth& auth() noexcept override { return inner_->auth(); }
    [[nodiscard]] std::vector<ai::Model> models() const override { return inner_->models(); }

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions options,
        ai::AssistantEventSink sink) override {
        co_return co_await inner_->stream(
            model, context, std::move(options), std::move(sink));
    }

private:
    std::string id_;
    std::shared_ptr<ai::Provider> inner_;
};

} // namespace detail

inline std::shared_ptr<ai::Models> models_from_provider(
    std::shared_ptr<ai::Provider> provider) {
    auto models = std::make_shared<ai::Models>(
        std::make_shared<detail::FixtureCredentialStore>(),
        std::make_shared<detail::FixtureAuthContext>());
    // Registered under the canonical ids the session layer may query; aliases
    // share the same underlying fake provider.
    std::vector<std::string> provider_ids{
        std::string{provider->id()},
        "sdk-host",
        "fake",
    };
    for (const auto& id : provider_ids) {
        if (models->provider(id)) {
            continue;
        }
        auto to_register =
            id == provider->id()
                ? provider
                : std::make_shared<detail::IdAliasProvider>(id, provider);
        if (auto added = models->set_provider(std::move(to_register)); !added) {
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
