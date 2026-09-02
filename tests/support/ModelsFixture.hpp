#pragma once

#include <cch/ai/Models.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

class ScriptedProvider;

[[nodiscard]] std::vector<coding_agent::ModelRuntimeTestProvider> make_scripted_fake_provider_definitions();
[[nodiscard]] std::shared_ptr<ScriptedProvider> make_scripted_fake_provider(std::string provider_id = "fake");
[[nodiscard]] std::shared_ptr<ai::Models> make_scripted_fake_models();

namespace detail {

class FixtureCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override {
        return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
            std::expected<std::vector<ai::CredentialInfo>, cch::support::Error>{
                std::vector<ai::CredentialInfo>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string,
        ai::CredentialModifyHook) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<void> remove(std::string) override {
        return cch::support::AsyncResult<void>(
            std::expected<void, cch::support::Error>{});
    }
};

class FixtureAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> environment(
        std::string) const override {
        return cch::support::AsyncResult<std::optional<std::string>>(
            std::expected<std::optional<std::string>, cch::support::Error>{
                std::optional<std::string>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<bool> file_exists(
        std::string) const override {
        return cch::support::AsyncResult<bool>(
            std::expected<bool, cch::support::Error>{false});
    }
};

[[nodiscard]] inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> scripted_fake_runtime() {
    return coding_agent::create_model_runtime_for_testing(
            coding_agent::ModelRuntimeOptions{
                    .models_path = std::filesystem::path{},
                    .credentials = std::make_shared<FixtureCredentialStore>(),
            },
            coding_agent::ModelRuntimeTestOptions{
                    .providers = make_scripted_fake_provider_definitions(),
            });
}

inline ai::ProviderAuth fixture_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "test fixture";
    api_key.check = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthCheck>> {
        return cch::support::AsyncResult<std::optional<ai::AuthCheck>>(
            std::expected<std::optional<ai::AuthCheck>, cch::support::Error>{
                ai::AuthCheck{.source = "test fixture", .type = ai::AuthType::ApiKey}});
    };
    api_key.resolve = [](const ai::AuthContext&, std::optional<ai::ApiKeyCredential>)
        -> cch::support::AsyncResult<std::optional<ai::AuthResult>> {
        return cch::support::AsyncResult<std::optional<ai::AuthResult>>(
            std::expected<std::optional<ai::AuthResult>, cch::support::Error>{
                ai::AuthResult{.source = "test fixture"}});
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

} // namespace detail

[[nodiscard]] inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> scripted_fake_runtime() {
    return detail::scripted_fake_runtime();
}

/// One recorded scripted request at the public coding-agent test seam.
struct RecordedProviderRequest {
    ai::Model model;
    ai::AiContext context;
    coding_agent::ModelRuntimeTestStreamOptions options;
};

/// Test-only scripted stream provider. It carries no cch_ai Provider
/// capability; ModelsFixture converts it to a Provider Definition through the
/// coding-agent test seam.
class ScriptedProvider {
public:
    explicit ScriptedProvider(std::string provider_id, ai::ProviderAuth auth = detail::fixture_auth())
        : provider_id_(std::move(provider_id)), auth_(std::move(auth)) {}
    ScriptedProvider(ScriptedProvider&&) noexcept = default;
    ScriptedProvider& operator=(ScriptedProvider&&) noexcept = default;
    virtual ~ScriptedProvider() = default;
    ScriptedProvider(const ScriptedProvider&) = delete;
    ScriptedProvider& operator=(const ScriptedProvider&) = delete;

    [[nodiscard]] virtual std::string_view id() const noexcept { return provider_id_; }
    [[nodiscard]] virtual std::string_view name() const noexcept { return provider_id_; }
    [[nodiscard]] virtual ai::ProviderAuth& auth() noexcept { return auth_; }
    [[nodiscard]] virtual std::vector<ai::Model> models() const { return {}; }

    [[nodiscard]] virtual ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) = 0;

protected:
    const std::string& provider_id() const noexcept { return provider_id_; }
    ai::ProviderAuth& provider_auth() noexcept { return auth_; }

private:
    std::string provider_id_;
    ai::ProviderAuth auth_;
};

[[nodiscard]] inline coding_agent::ModelRuntimeTestProvider make_test_provider_definition(
        std::shared_ptr<ScriptedProvider> provider,
        std::string id,
        std::vector<ai::Model> models,
        ai::ProviderAuth auth = detail::fixture_auth()) {
    const std::string provider_name{provider->name()};
    return coding_agent::ModelRuntimeTestProvider{
            .definition =
                    ai::ProviderDefinition{
                            .id = std::move(id),
                            .name = provider_name,
                            .models = std::move(models),
                            .auth = std::move(auth),
                    },
            .stream =
                    [provider = std::move(provider)](ai::Model model,
                            ai::AiContext context,
                            coding_agent::ModelRuntimeTestStreamOptions options) {
                        return provider->stream(std::move(model), std::move(context), std::move(options));
                    },
    };
}

[[nodiscard]] inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> runtime_from_provider(
        std::shared_ptr<ScriptedProvider> provider, coding_agent::ModelRuntimeOptions options = {}) {
    const std::string provider_id{provider->id()};
    const auto models = provider->models();
    auto provider_state = std::move(provider);

    std::vector<coding_agent::ModelRuntimeTestProvider> definitions;
    const auto definition_for = [&provider_state, &models](std::string id) {
        return make_test_provider_definition(provider_state, std::move(id), models);
    };
    definitions.push_back(definition_for(provider_id));
    if (provider_id != "sdk-host") {
        definitions.push_back(definition_for("sdk-host"));
    }
    if (provider_id != "fake") {
        definitions.push_back(definition_for("fake"));
    }
    return coding_agent::create_model_runtime_for_testing(std::move(options),
            coding_agent::ModelRuntimeTestOptions{
                    .providers = std::move(definitions),
            });
}

[[nodiscard]] inline std::shared_ptr<ai::Models> models_from_provider(std::shared_ptr<ScriptedProvider> provider) {
    auto models = std::make_shared<ai::Models>(
            std::make_shared<detail::FixtureCredentialStore>(), std::make_shared<detail::FixtureAuthContext>());
    const std::string provider_id{provider->id()};
    const auto provider_models = provider->models();
    auto provider_state = std::move(provider);

    const auto apply_definition = [&](std::string id) {
        return coding_agent::apply_model_runtime_test_provider(
                *models, make_test_provider_definition(provider_state, std::move(id), provider_models));
    };

    // The stream callback preserves the scripted provider state; fixture auth
    // is enough for the model-only test seam and keeps aliases independent.
    if (auto applied = apply_definition(provider_id); !applied) {
        return nullptr;
    }
    for (const auto& id : {std::string{"sdk-host"}, std::string{"fake"}}) {
        if (id == provider_id) {
            continue;
        }
        if (auto applied = apply_definition(id); !applied) {
            return nullptr;
        }
    }
    return models;
}

/// Test-only carrier for SessionFactory's private Models assembly seam. The
/// base is the internal CLI creation request: focused session tests reuse its
/// session-target/workspace/model-selection surface, and `models` injects the
/// deterministic scripted provider catalog.
struct ModelsSessionOptions : coding_agent::runtime::AgentSessionCreationRequest {
    std::shared_ptr<ai::Models> models;
    std::shared_ptr<coding_agent::ModelRuntime> model_runtime;
};

/// Request Model for focused session tests that no longer set `provider_config`:
/// a complete, credential-free shape carried through the scripted fake seam.
inline ai::Model scripted_request_model(
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

} // namespace cch::tests
