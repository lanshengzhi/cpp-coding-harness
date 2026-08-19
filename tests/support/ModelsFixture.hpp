#pragma once

#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

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

/// One recorded provider request — the post-`streamSimple` shape: the concrete
/// Model argument, the conversation context, and the per-turn options. The
/// legacy request aggregate is gone (ADR 0034 / #362); scripted provider fakes
/// record this instead.
struct RecordedProviderRequest {
    ai::Model model;
    ai::AiContext context;
    ai::ProviderStreamOptions options;
};

/// Base for scripted `ai::Provider` fakes used across the session/TUI/CLI
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

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions options) override {
        return inner_->stream(
            std::move(model), std::move(context), std::move(options));
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

/// Test-only carrier for SessionFactory's private Models assembly seam. The
/// base is the internal CLI creation request: focused session tests reuse its
/// session-target/workspace/model-selection surface, and `models` injects the
/// deterministic scripted provider catalog.
struct ModelsSessionOptions : coding_agent::runtime::AgentSessionCreationRequest {
    std::shared_ptr<ai::Models> models;
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
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    if (!request.execution_runtime_target) {
        request.execution_runtime_target = detail::fixture_runtime_target();
    }
    if (models) {
        return coding_agent::create_agent_session_for_testing(
            std::move(request), std::move(models));
    }
    return coding_agent::create_agent_session(std::move(request));
}

inline support::Expected<coding_agent::CreateAgentSessionResult> create_agent_session(
    ModelsSessionOptions options,
    std::unique_ptr<coding_agent::runtime::AsyncUserShell> user_shell) {
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    if (!request.execution_runtime_target) {
        request.execution_runtime_target = detail::fixture_runtime_target();
    }
    return coding_agent::create_agent_session_for_testing(
        std::move(request), std::move(models), std::move(user_shell));
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
