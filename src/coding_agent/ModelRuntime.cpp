#include <cch/coding_agent/ModelRuntime.hpp>

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AuthStorage.hpp>
#include "ModelConfig.hpp"
#include "ProcessAuthContext.hpp"
#include "ProviderComposer.hpp"
#include "RuntimeApiKeyOverlay.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Process.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string join_errors(const std::vector<std::string>& errors) {
    std::string result;
    for (const auto& error : errors) {
        if (!result.empty()) {
            result += "\n\n";
        }
        result += error;
    }
    return result;
}

} // namespace

struct ModelRuntime::Impl {
    std::filesystem::path agent_dir;
    std::filesystem::path models_path;
    std::shared_ptr<ai::CredentialStore> credentials;
    std::shared_ptr<ai::AuthContext> auth_context;
    std::shared_ptr<ai::Models> models;
    ModelConfig config;
    std::map<std::string, std::shared_ptr<ai::Provider>, std::less<>> builtins;
    std::map<std::string, std::shared_ptr<ai::Provider>, std::less<>> native_extensions;
    std::map<std::string, std::string, std::less<>> composition_errors;
    ProviderComposerOptions composer_options;
    std::optional<std::string> config_error;

    // Availability snapshot.
    std::vector<ai::Model> all_models;
    std::vector<ai::Model> available_models;
    std::set<std::string, std::less<>> configured_providers;
    std::set<std::string, std::less<>> stored_providers;
    std::set<std::string, std::less<>> runtime_providers;
    std::map<std::string, ai::AuthCheck, std::less<>> auth_snapshot;
    std::optional<std::string> availability_error;

    [[nodiscard]] std::set<std::string, std::less<>> provider_ids() const {
        std::set<std::string, std::less<>> ids;
        for (const auto& [id, _] : builtins) {
            ids.insert(id);
        }
        for (const auto& [id, _] : native_extensions) {
            ids.insert(id);
        }
        for (const auto& id : config.provider_ids()) {
            ids.insert(id);
        }
        return ids;
    }

    [[nodiscard]] std::shared_ptr<ai::Provider> base_provider(
        std::string_view provider_id) const {
        if (const auto found = native_extensions.find(provider_id);
            found != native_extensions.end()) {
            return found->second;
        }
        if (const auto found = builtins.find(provider_id);
            found != builtins.end()) {
            return found->second;
        }
        return nullptr;
    }

    void recompose_provider(std::string_view provider_id) {
        const auto base = base_provider(provider_id);
        std::optional<std::string> error;
        auto composed = compose_provider(
            provider_id, base, config, composer_options, error);
        if (error) {
            composition_errors[std::string{provider_id}] = *error;
        } else {
            composition_errors.erase(std::string{provider_id});
        }
        if (composed) {
            if (auto added = models->set_provider(std::move(composed)); !added) {
                composition_errors[std::string{provider_id}] = added.error().message;
            }
        } else {
            models->delete_provider(provider_id);
        }
    }

    void rebuild_providers() {
        models->clear_providers();
        composition_errors.clear();
        for (const auto& provider_id : provider_ids()) {
            recompose_provider(provider_id);
        }
        update_snapshot();
    }

    void update_snapshot() {
        all_models = models->models();
        configured_providers.clear();
        for (const auto& provider_value : models->providers()) {
            const auto& auth = provider_value->auth();
            if (auth.api_key || auth.oauth) {
                configured_providers.insert(std::string{provider_value->id()});
            }
        }
        recompute_available_models();
    }

    /// Rebuild `available_models` from the structural `configured_providers`
    /// set. Used by the snapshot and by runtime API key changes.
    void recompute_available_models() {
        available_models.clear();
        available_models.reserve(all_models.size());
        for (const auto& model : all_models) {
            if (configured_providers.contains(model.provider)) {
                available_models.push_back(model);
            }
        }
    }
};

ModelRuntime::ModelRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ModelRuntime::ModelRuntime() noexcept = default;

ModelRuntime::ModelRuntime(ModelRuntime&&) noexcept = default;
ModelRuntime& ModelRuntime::operator=(ModelRuntime&&) noexcept = default;
ModelRuntime::~ModelRuntime() = default;

util::Expected<std::shared_ptr<ModelRuntime>> ModelRuntime::create(
    ModelRuntimeOptions options) {
    std::filesystem::path agent_dir = options.agent_dir.empty()
        ? agent_config_dir()
        : options.agent_dir;

    std::optional<std::filesystem::path> configured_models_path = options.models_path;
    std::filesystem::path models_path;
    if (configured_models_path.has_value()) {
        models_path = *configured_models_path;
    } else if (!agent_dir.empty()) {
        models_path = agent_dir / "models.json";
    }

    std::shared_ptr<ai::CredentialStore> credentials = std::move(options.credentials);
    if (!credentials) {
        credentials = std::make_shared<AuthStorage>(agent_dir / "auth.json");
    }
    auto credentials_with_overlay = std::make_shared<RuntimeApiKeyOverlay>(credentials);
    auto auth_context = std::make_shared<ProcessAuthContext>();
    auto models = std::make_shared<ai::Models>(credentials_with_overlay, auth_context);

    std::shared_ptr<ai::providers::StreamTransport> http_transport =
        std::move(options.http_transport);
    if (!http_transport) {
        http_transport = std::make_shared<ai::providers::BoostBeastStreamTransport>();
    }
    std::shared_ptr<ai::providers::WebSocketTransport> ws_transport =
        std::move(options.ws_transport);
    if (!ws_transport) {
        ws_transport = std::make_shared<ai::providers::BoostBeastWebSocketTransport>();
    }
    std::shared_ptr<util::AsyncProcessRunner> process_runner =
        std::make_shared<util::DefaultAsyncProcessRunner>();

    auto impl = std::make_unique<Impl>(Impl{
        .agent_dir = std::move(agent_dir),
        .models_path = std::move(models_path),
        .credentials = credentials_with_overlay,
        .auth_context = auth_context,
        .models = models,
        .config = {},
        .builtins = {},
        .native_extensions = {},
        .composition_errors = {},
        .composer_options = ProviderComposerOptions{
            .http_transport = http_transport,
            .ws_transport = ws_transport,
            .codex_cache_config = options.codex_cache_config,
            .process_runner = process_runner,
        },
        .config_error = {},
        .all_models = {},
        .available_models = {},
        .configured_providers = {},
        .stored_providers = {},
        .runtime_providers = {},
        .auth_snapshot = {},
        .availability_error = {},
    });
    impl->builtins = builtin_providers(impl->composer_options);

    auto runtime = std::shared_ptr<ModelRuntime>(new ModelRuntime(std::move(impl)));
    static_cast<void>(runtime->refresh());
    return runtime;
}

util::Expected<std::shared_ptr<ModelRuntime>>
ModelRuntime::create_from_models_for_testing(
    std::shared_ptr<ai::Models> models,
    ModelRuntimeOptions options) {
    std::filesystem::path agent_dir = options.agent_dir.empty()
        ? agent_config_dir()
        : options.agent_dir;
    std::shared_ptr<ai::CredentialStore> credentials = std::move(options.credentials);
    if (!credentials) {
        credentials = std::make_shared<AuthStorage>(agent_dir / "auth.json");
    }
    auto credentials_with_overlay = std::make_shared<RuntimeApiKeyOverlay>(credentials);
    auto auth_context = std::make_shared<ProcessAuthContext>();
    auto impl = std::make_unique<Impl>(Impl{
        .agent_dir = std::move(agent_dir),
        .models_path = {},
        .credentials = credentials_with_overlay,
        .auth_context = auth_context,
        .models = std::move(models),
        .config = {},
        .builtins = {},
        .native_extensions = {},
        .composition_errors = {},
        .composer_options = {},
        .config_error = {},
        .all_models = {},
        .available_models = {},
        .configured_providers = {},
        .stored_providers = {},
        .runtime_providers = {},
        .auth_snapshot = {},
        .availability_error = {},
    });
    auto runtime = std::shared_ptr<ModelRuntime>(new ModelRuntime(std::move(impl)));
    runtime->impl_->update_snapshot();
    return runtime;
}

const std::filesystem::path& ModelRuntime::agent_dir() const noexcept {
    return impl_->agent_dir;
}

const std::filesystem::path& ModelRuntime::models_path() const noexcept {
    return impl_->models_path;
}

util::ExpectedVoid ModelRuntime::refresh() {
    impl_->config = ModelConfig::load(impl_->models_path);
    impl_->config_error = impl_->config.error();
    impl_->rebuild_providers();
    return util::ExpectedVoid{};
}

std::optional<std::string> ModelRuntime::get_error() const {
    std::vector<std::string> errors;
    if (impl_->config_error) {
        errors.push_back(*impl_->config_error);
    }
    for (const auto& [provider_id, error] : impl_->composition_errors) {
        errors.push_back("Provider \"" + provider_id + "\": " + error);
    }
    if (impl_->availability_error) {
        errors.push_back("Availability refresh: " + *impl_->availability_error);
    }
    if (errors.empty()) {
        return std::nullopt;
    }
    return join_errors(errors);
}

std::vector<std::shared_ptr<ai::Provider>> ModelRuntime::providers() const {
    return impl_->models->providers();
}

std::shared_ptr<ai::Provider> ModelRuntime::provider(
    std::string_view provider_id) const {
    return impl_->models->provider(provider_id);
}

std::vector<ai::Model> ModelRuntime::models(
    std::optional<std::string_view> provider_id) const {
    return impl_->models->models(provider_id);
}

std::optional<ai::Model> ModelRuntime::model(
    std::string_view provider_id,
    std::string_view model_id) const {
    return impl_->models->model(provider_id, model_id);
}

boost::asio::awaitable<util::Expected<std::vector<ai::Model>>>
ModelRuntime::get_available(std::optional<std::string_view> provider_id) {
    if (provider_id) {
        const auto selected = provider(*provider_id);
        if (!selected) {
            co_return std::vector<ai::Model>{};
        }
        CCH_TRY(check, co_await check_auth(std::string{*provider_id}));
        if (!check) {
            co_return std::vector<ai::Model>{};
        }
        co_return models(*provider_id);
    }

    std::set<std::string, std::less<>> configured;
    std::map<std::string, ai::AuthCheck, std::less<>> auth;
    std::optional<std::string> failure;
    for (const auto& provider_value : impl_->models->providers()) {
        const std::string provider_id{provider_value->id()};
        auto checked = co_await impl_->models->check_auth(provider_id);
        if (!checked) {
            failure = checked.error().message;
            continue;
        }
        if (*checked) {
            configured.insert(provider_id);
            auth.emplace(provider_id, **checked);
        }
    }
    CCH_TRY(stored, co_await impl_->credentials->list());

    impl_->configured_providers = std::move(configured);
    impl_->auth_snapshot = std::move(auth);
    impl_->stored_providers.clear();
    for (const auto& entry : stored) {
        impl_->stored_providers.insert(entry.provider_id);
    }
    impl_->recompute_available_models();
    impl_->availability_error = std::move(failure);
    co_return impl_->available_models;
}

std::vector<ai::Model> ModelRuntime::get_available_snapshot() const {
    return impl_->available_models;
}

boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>>
ModelRuntime::check_auth(std::string provider_id) {
    co_return co_await impl_->models->check_auth(std::move(provider_id));
}

boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>>
ModelRuntime::get_auth(
    std::string provider_id,
    std::optional<std::string> explicit_api_key) {
    co_return co_await impl_->models->get_auth(
        std::move(provider_id), std::move(explicit_api_key));
}

boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>>
ModelRuntime::get_auth(
    ai::Model model,
    std::optional<std::string> explicit_api_key) {
    co_return co_await impl_->models->get_auth(
        std::move(model), std::move(explicit_api_key));
}

bool ModelRuntime::has_configured_auth(std::string_view provider_id) const {
    return impl_->configured_providers.contains(std::string{provider_id});
}

bool ModelRuntime::is_using_oauth(std::string_view provider_id) const {
    const auto selected = provider(provider_id);
    return selected != nullptr && selected->auth().oauth.has_value();
}

util::ExpectedVoid ModelRuntime::set_runtime_api_key(
    std::string provider_id,
    std::string api_key) {
    auto* overlay = dynamic_cast<RuntimeApiKeyOverlay*>(impl_->credentials.get());
    if (overlay == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Auth,
            "runtime API key override unavailable for provider " + provider_id));
    }
    overlay->set_runtime_api_key(provider_id, std::move(api_key));
    // Refresh the availability snapshot so the provider resolves as configured
    // (pi `setRuntimeApiKey` refresh). Runtime keys are the highest-precedence
    // source in Request Authentication via the credential overlay; they are
    // never stored, so `stored_providers` is intentionally not touched.
    impl_->runtime_providers.insert(provider_id);
    impl_->configured_providers.insert(provider_id);
    impl_->auth_snapshot[provider_id] = ai::AuthCheck{
        .source = "runtime API key",
        .type = ai::AuthType::ApiKey,
    };
    impl_->recompute_available_models();
    return util::ExpectedVoid{};
}

util::ExpectedVoid ModelRuntime::remove_runtime_api_key(
    std::string provider_id) {
    auto* overlay = dynamic_cast<RuntimeApiKeyOverlay*>(impl_->credentials.get());
    if (overlay == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Auth,
            "runtime API key override unavailable for provider " + provider_id));
    }
    overlay->remove_runtime_api_key(provider_id);
    impl_->runtime_providers.erase(provider_id);
    impl_->auth_snapshot.erase(provider_id);
    // Restore the structural availability snapshot.
    impl_->update_snapshot();
    return util::ExpectedVoid{};
}

bool ModelRuntime::has_runtime_api_key(std::string_view provider_id) const {
    if (auto* overlay = dynamic_cast<RuntimeApiKeyOverlay*>(impl_->credentials.get())) {
        return overlay->has_runtime_api_key(provider_id);
    }
    return false;
}

std::optional<ModelRuntimeAuthStatus> ModelRuntime::get_provider_auth_status(
    std::string_view provider_id) const {
    if (!provider(provider_id)) {
        return std::nullopt;
    }
    if (impl_->runtime_providers.contains(std::string{provider_id})) {
        return ModelRuntimeAuthStatus{.configured = true, .source = "runtime"};
    }
    if (impl_->stored_providers.contains(std::string{provider_id})) {
        return ModelRuntimeAuthStatus{.configured = true, .source = "stored"};
    }
    if (const auto& configured = impl_->config.provider(provider_id)) {
        if (auto status = configured_request_auth_status(*configured)) {
            return status;
        }
    }
    if (const auto found = impl_->auth_snapshot.find(provider_id);
        found != impl_->auth_snapshot.end()) {
        return ModelRuntimeAuthStatus{
            .configured = true,
            .source = "environment",
            .label = found->second.source,
        };
    }
    const auto selected = provider(provider_id);
    if (selected && (selected->auth().api_key || selected->auth().oauth)) {
        return ModelRuntimeAuthStatus{.configured = true};
    }
    return ModelRuntimeAuthStatus{.configured = false};
}

boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>>
ModelRuntime::list_credentials() {
    co_return co_await impl_->credentials->list();
}

boost::asio::awaitable<util::Expected<ai::Credential>> ModelRuntime::login(
    std::string provider_id,
    ai::AuthType type,
    ai::AuthInteraction interaction) {
    auto credential = co_await impl_->models->login(
        provider_id, type, std::move(interaction));
    if (credential) {
        // Post-login refresh failures are recorded in the composition-errors
        // map and never fail the login call (ADR 0032).
        static_cast<void>(refresh());
    }
    co_return credential;
}

boost::asio::awaitable<util::ExpectedVoid> ModelRuntime::logout(
    std::string provider_id) {
    CCH_TRY_VOID(co_await impl_->models->logout(provider_id));
    // Credential-dependent composition is reset before the unconfigured
    // provider is recomposed by refresh (pi: logout → recomposeProvider →
    // refresh; order preserved).
    impl_->recompose_provider(provider_id);
    static_cast<void>(refresh());
    co_return util::ExpectedVoid{};
}

boost::asio::awaitable<util::Expected<ai::AssistantMessage>>
ModelRuntime::stream_simple(
    ai::Model model,
    ai::AiContext context,
    ai::SimpleStreamOptions options,
    ai::AssistantEventSink sink) {
    co_return co_await impl_->models->stream_simple(
        std::move(model),
        std::move(context),
        std::move(options),
        std::move(sink));
}

boost::asio::awaitable<util::Expected<ai::AssistantMessage>>
ModelRuntime::stream(
    const ai::StreamChatRequest& request,
    ai::AssistantEventSink sink) {
    co_return co_await impl_->models->stream(request, std::move(sink));
}

util::ExpectedVoid ModelRuntime::register_native_provider(
    std::shared_ptr<ai::Provider> provider_value) {
    if (!provider_value || provider_value->id().empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "provider id is required"));
    }
    impl_->native_extensions[std::string{provider_value->id()}] = provider_value;
    impl_->recompose_provider(provider_value->id());
    impl_->update_snapshot();
    return util::ExpectedVoid{};
}

std::vector<std::string> ModelRuntime::configured_api_key_env_names() const {
    return ::cch::coding_agent::configured_api_key_env_names(impl_->config);
}

std::optional<std::string> ModelRuntime::default_model_for_provider(
    std::string_view provider_id) {
    return ::cch::coding_agent::default_model_for_provider(provider_id);
}

} // namespace cch::coding_agent
