#pragma once

#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cch::tests {
namespace detail {

class FixtureRuntime final {
public:
    FixtureRuntime()
        : loop_(std::make_shared<boost::asio::io_context>()),
          work_guard_(boost::asio::make_work_guard(*loop_)),
          root_(loop_, harness::RuntimeLimits{}),
          loop_thread_([this] { loop_->run(); }) {}

    ~FixtureRuntime() {
        work_guard_.reset();
        loop_->stop();
    }

    [[nodiscard]] std::shared_ptr<harness::RuntimeTarget> make_target() {
        return root_.make_target();
    }

private:
    std::shared_ptr<boost::asio::io_context> loop_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
    harness::RuntimeRoot root_;
    std::jthread loop_thread_;
};

[[nodiscard]] inline std::shared_ptr<harness::RuntimeTarget> fixture_runtime_target() {
    static FixtureRuntime runtime;
    return runtime.make_target();
}

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

inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> scripted_fake_runtime() {
    return coding_agent::create_model_runtime_for_testing(
            coding_agent::ModelRuntimeOptions{
                    .models_path = std::filesystem::path{},
                    .credentials = ai::providers::make_scripted_credential_store(),
            },
            coding_agent::ModelRuntimeTestOptions{
                    .providers = ai::providers::make_scripted_fake_provider_definitions(),
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

inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> scripted_fake_runtime() {
    return detail::scripted_fake_runtime();
}

/// One recorded scripted request — the post-`streamSimple` shape: the concrete
/// Model argument, the conversation context, and the per-turn options. The
/// legacy request aggregate is gone (ADR 0034 / #362); scripted definitions
/// record this instead.
struct RecordedProviderRequest {
    ai::Model model;
    ai::AiContext context;
    ai::ProviderStreamOptions options;
};

/// Transitional base for scripted stream fakes used across the session/TUI/CLI
/// test suites. `models_from_provider` converts these fakes into scripted
/// Provider Definitions before installing them in Models.
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

inline support::Expected<std::shared_ptr<coding_agent::ModelRuntime>> runtime_from_provider(
        std::shared_ptr<ai::Provider> provider, coding_agent::ModelRuntimeOptions options = {}) {
    const std::string provider_id{provider->id()};
    const std::string provider_name{provider->name()};
    const auto models = provider->models();
    auto provider_state = std::move(provider);

    std::vector<ai::providers::ScriptedProviderDefinition> definitions;
    const auto definition_for = [&provider_state, &models, &provider_name](std::string id) {
        ai::providers::ScriptedProviderDefinition definition;
        definition.definition = ai::ProviderDefinition{
                .id = std::move(id),
                .name = provider_name,
                .models = models,
                .auth = detail::fixture_auth(),
        };
        definition.stream = [provider_state](
                                    ai::Model model, ai::AiContext context, ai::ProviderStreamOptions stream_options) {
            return provider_state->stream(std::move(model), std::move(context), std::move(stream_options));
        };
        return definition;
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

inline std::shared_ptr<ai::Models> models_from_provider(
    std::shared_ptr<ai::Provider> provider) {
    auto models = std::make_shared<ai::Models>(
        std::make_shared<detail::FixtureCredentialStore>(),
        std::make_shared<detail::FixtureAuthContext>());
    const std::string provider_id{provider->id()};
    const std::string provider_name{provider->name()};
    const auto provider_models = provider->models();
    auto provider_state = std::move(provider);
    const auto apply_definition = [&](std::string id, ai::ProviderAuth auth) {
        ai::providers::ScriptedProviderDefinition definition;
        definition.definition = ai::ProviderDefinition{
                .id = std::move(id),
                .name = provider_name,
                .models = provider_models,
                .auth = std::move(auth),
        };
        definition.stream = [provider_state](
                                    ai::Model model, ai::AiContext context, ai::ProviderStreamOptions stream_options) {
            return provider_state->stream(std::move(model), std::move(context), std::move(stream_options));
        };
        return ai::providers::apply_scripted_provider(*models, std::move(definition));
    };

    // The stream callback preserves the scripted provider state; fixture auth
    // is enough for the model-only test seam and keeps aliases independent.
    if (auto applied = apply_definition(provider_id, detail::fixture_auth()); !applied) {
        return nullptr;
    }
    for (const auto& id : {std::string{"sdk-host"}, std::string{"fake"}}) {
        if (id == provider_id) {
            continue;
        }
        if (auto applied = apply_definition(id, detail::fixture_auth()); !applied) {
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

inline support::Expected<coding_agent::CreateAgentSessionResult> create_agent_session(
    ModelsSessionOptions options) {
    auto models = std::move(options.models);
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    if (!request.execution_runtime_target) {
        request.execution_runtime_target = detail::fixture_runtime_target();
    }
    if (model_runtime) {
        request.model_runtime = std::move(model_runtime);
        return coding_agent::create_agent_session(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{
                        .model_runtime = nullptr, .cli_fake = false, .models = nullptr, .user_shell = nullptr});
    }
    return coding_agent::create_agent_session(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{
                    .model_runtime = nullptr, .cli_fake = false, .models = std::move(models), .user_shell = nullptr});
}

inline support::Expected<coding_agent::CreateAgentSessionResult> create_agent_session(
    ModelsSessionOptions options,
    std::unique_ptr<coding_agent::runtime::AsyncUserShell> user_shell) {
    auto models = std::move(options.models);
    auto model_runtime = std::move(options.model_runtime);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    if (!request.execution_runtime_target) {
        request.execution_runtime_target = detail::fixture_runtime_target();
    }
    if (model_runtime) {
        request.model_runtime = std::move(model_runtime);
        return coding_agent::create_agent_session(std::move(request),
                std::nullopt,
                coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                        .cli_fake = false,
                        .models = nullptr,
                        .user_shell = std::move(user_shell)});
    }
    return coding_agent::create_agent_session(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{.model_runtime = nullptr,
                    .cli_fake = false,
                    .models = std::move(models),
                    .user_shell = std::move(user_shell)});
}

} // namespace cch::tests

namespace cch::coding_agent {

inline support::Expected<CreateAgentSessionResult> create_agent_session(
    tests::ModelsSessionOptions options) {
    return tests::create_agent_session(std::move(options));
}

inline support::Expected<CreateAgentSessionResult> create_agent_session(
    tests::ModelsSessionOptions options,
    std::unique_ptr<runtime::AsyncUserShell> user_shell) {
    return tests::create_agent_session(
        std::move(options), std::move(user_shell));
}

} // namespace cch::coding_agent
