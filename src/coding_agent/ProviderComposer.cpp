#include "ProviderComposer.hpp"

#include "ai/providers/ComposedProvider.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/auth/KimiCodingOAuth.hpp"
#include "ai/auth/OpenAICodexOAuth.hpp"
#include "ai/providers/CodexCatalog.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"
#include "ai/providers/KimiCatalog.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent {
namespace {

using ai::AuthCheck;
using ai::AuthResult;
using ai::AuthType;
using ai::ModelAuth;

// ─────────────────────────────────────────────────────────────────────────────
// Config-value resolution (pi `resolve-config-value.ts` subset): literal,
// `$VAR`/`${VAR}` environment templates, and `!command` shell execution with a
// process-lifetime result cache. models.json is an executable configuration
// surface; the "not a sandbox" boundary applies unchanged.
// ─────────────────────────────────────────────────────────────────────────────

namespace config_value {

struct TemplatePart {
    enum class Kind { Literal, Env };
    Kind kind{Kind::Literal};
    std::string value{};
};

[[nodiscard]] bool is_env_var_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    for (const char character : name) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) {
            return false;
        }
    }
    return true;
}

void append_literal(std::vector<TemplatePart>& parts, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (!parts.empty() && parts.back().kind == TemplatePart::Kind::Literal) {
        parts.back().value.append(value);
        return;
    }
    parts.push_back(TemplatePart{
        .kind = TemplatePart::Kind::Literal,
        .value = std::string{value},
    });
}

[[nodiscard]] std::vector<TemplatePart> parse_template(std::string_view config) {
    std::vector<TemplatePart> parts;
    std::size_t index = 0;
    while (index < config.size()) {
        const std::size_t dollar_index = config.find('$', index);
        if (dollar_index == std::string_view::npos) {
            append_literal(parts, config.substr(index));
            break;
        }
        append_literal(parts, config.substr(index, dollar_index - index));
        const std::size_t next = dollar_index + 1;
        if (next >= config.size()) {
            append_literal(parts, "$");
            break;
        }
        const char next_character = config[next];
        if (next_character == '$' || next_character == '!') {
            append_literal(parts, std::string_view{&next_character, 1});
            index = next + 1;
            continue;
        }
        if (next_character == '{') {
            const std::size_t end_index = config.find('}', next + 1);
            if (end_index == std::string_view::npos) {
                append_literal(parts, "$");
                index = next;
                continue;
            }
            const std::string_view name = config.substr(next + 1, end_index - next - 1);
            if (is_env_var_name(name)) {
                parts.push_back(TemplatePart{
                    .kind = TemplatePart::Kind::Env,
                    .value = std::string{name},
                });
            } else {
                append_literal(parts, config.substr(dollar_index, end_index - dollar_index + 1));
            }
            index = end_index + 1;
            continue;
        }
        // Bare `$NAME` prefix match.
        std::size_t name_length = 0;
        while (next + name_length < config.size()) {
            const char character = config[next + name_length];
            if (std::isalnum(static_cast<unsigned char>(character)) || character == '_') {
                ++name_length;
            } else {
                break;
            }
        }
        if (name_length > 0 && is_env_var_name(config.substr(next, 1))) {
            parts.push_back(TemplatePart{
                .kind = TemplatePart::Kind::Env,
                .value = std::string{config.substr(next, name_length)},
            });
            index = next + name_length;
        } else {
            append_literal(parts, "$");
            index = next;
        }
    }
    return parts;
}

[[nodiscard]] bool is_command(std::string_view config) {
    return !config.empty() && config.front() == '!';
}

[[nodiscard]] std::vector<std::string> env_var_names(std::string_view config) {
    if (is_command(config)) {
        return {};
    }
    std::vector<std::string> names;
    std::set<std::string, std::less<>> seen;
    for (const auto& part : parse_template(config)) {
        if (part.kind == TemplatePart::Kind::Env && seen.insert(part.value).second) {
            names.push_back(part.value);
        }
    }
    return names;
}

[[nodiscard]] std::vector<std::string> missing_env_var_names(
    std::string_view config,
    const ai::ProviderEnv& env) {
    std::vector<std::string> missing;
    for (const auto& name : env_var_names(config)) {
        const auto found = env.find(name);
        const bool present = found != env.end() && !found->second.empty();
        if (!present && std::getenv(name.c_str()) == nullptr) {
            missing.push_back(name);
        }
    }
    return missing;
}

[[nodiscard]] std::optional<std::string> resolve_template(
    std::string_view config,
    const ai::ProviderEnv& env) {
    std::string resolved;
    for (const auto& part : parse_template(config)) {
        if (part.kind == TemplatePart::Kind::Literal) {
            resolved += part.value;
            continue;
        }
        const auto found = env.find(part.value);
        std::optional<std::string> value;
        if (found != env.end()) {
            value = found->second;
        } else if (const char* process_value = std::getenv(part.value.c_str())) {
            value = std::string{process_value};
        }
        if (!value || value->empty()) {
            return std::nullopt;
        }
        resolved += *value;
    }
    return resolved;
}

[[nodiscard]] std::string trim(std::string value) {
    const auto not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

[[nodiscard]] std::filesystem::path shell_executable() {
    return "/bin/sh";
}

[[nodiscard]] std::vector<std::string> shell_command_arguments(
    const std::string& command) {
    return {"-c", command};
}

[[nodiscard]] boost::asio::awaitable<std::optional<std::string>> execute_command(
    std::string_view config,
    std::shared_ptr<harness::AsyncProcessRunner> runner) {
    // Process-lifetime result cache (pi `commandResultCache`).
    static std::map<std::string, std::optional<std::string>> cache;
    const std::string key{config};
    if (const auto found = cache.find(key); found != cache.end()) {
        co_return found->second;
    }

    std::optional<std::string> value;
    if (runner) {
        harness::ProcessRequest request;
        request.executable = shell_executable();
        request.arguments = shell_command_arguments(std::string{config.substr(1)});
        request.timeout = std::chrono::seconds{10};
        auto result = co_await runner->run(std::move(request));
        if (result && result->exit_code == 0) {
            value = trim(result->stdout_output);
            if (value && value->empty()) {
                value.reset();
            }
        }
    }
    cache.emplace(key, value);
    co_return value;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<std::string>>> resolve_config_value(
    std::string_view config,
    const ai::ProviderEnv& env,
    std::shared_ptr<harness::AsyncProcessRunner> runner) {
    if (is_command(config)) {
        co_return co_await execute_command(config, std::move(runner));
    }
    co_return resolve_template(config, env);
}

[[nodiscard]] std::string join_strings(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            result += ", ";
        }
        result += values[index];
    }
    return result;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> resolve_config_value_required(
    std::string_view config,
    std::string description,
    const ai::ProviderEnv& env,
    std::shared_ptr<harness::AsyncProcessRunner> runner) {
    auto resolved = co_await resolve_config_value(config, env, std::move(runner));
    if (resolved && *resolved) {
        co_return **resolved;
    }
    if (is_command(config)) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
            "Failed to resolve " + description + " from shell command: " +
                std::string{config.substr(1)}));
    }
    const auto missing = missing_env_var_names(config, env);
    if (missing.size() == 1) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
            "Failed to resolve " + description + " from environment variable: " +
                missing.front()));
    }
    if (missing.size() > 1) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
            "Failed to resolve " + description + " from environment variables: " +
                join_strings(missing)));
    }
    co_return std::unexpected(support::make_error(
        support::ErrorCode::Auth,
        "Failed to resolve " + description));
}

[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<ai::ModelHeaders>>> resolve_headers(
    const ai::ModelHeaders& headers,
    const ai::ProviderEnv& env,
    std::shared_ptr<harness::AsyncProcessRunner> runner) {
    ai::ModelHeaders resolved;
    for (const auto& [name, value] : headers) {
        auto resolved_value = co_await resolve_config_value(value, env, runner);
        if (resolved_value && *resolved_value) {
            resolved.emplace(name, **resolved_value);
        }
    }
    co_return resolved.empty()
        ? std::optional<ai::ModelHeaders>{}
        : std::optional<ai::ModelHeaders>{std::move(resolved)};
}

} // namespace config_value

[[nodiscard]] boost::asio::awaitable<support::Expected<ai::ProviderEnv>> config_context_env(
    const std::vector<std::string>& names,
    const ai::AuthContext& context,
    ai::ProviderEnv explicit_env) {
    for (const auto& name : names) {
        if (explicit_env.contains(name)) {
            continue;
        }
        CCH_TRY(value, co_await ai::detail::await_async_result(context.environment(name)));
        if (value) {
            explicit_env[name] = *value;
        }
    }
    co_return explicit_env;
}

// ─────────────────────────────────────────────────────────────────────────────
// Composition (pi `provider-composer.ts` subset)
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] support::Expected<ai::Model> model_from_json(
    std::string_view provider_id,
    const ModelsJsonModel& definition,
    const ModelsJsonProvider& provider_config,
    const ai::Model* defaults) {
    const std::string api = definition.api.value_or(
        provider_config.api.value_or(defaults ? defaults->api : std::string{}));
    if (api.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "Provider " + std::string{provider_id} + ", model " + definition.id +
                ": no \"api\" specified. Set at provider or model level."));
    }
    const std::string base_url = definition.base_url.value_or(
        provider_config.base_url.value_or(defaults ? defaults->base_url : std::string{}));
    if (base_url.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "Provider " + std::string{provider_id} +
                ": \"baseUrl\" is required when defining custom models."));
    }
    if (definition.context_window && *definition.context_window == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "Provider " + std::string{provider_id} + ", model " + definition.id +
                ": invalid contextWindow"));
    }
    if (definition.max_tokens && *definition.max_tokens == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "Provider " + std::string{provider_id} + ", model " + definition.id +
                ": invalid maxTokens"));
    }
    ai::Model model;
    model.id = definition.id;
    model.name = definition.name.value_or(definition.id);
    model.api = api;
    model.provider = std::string{provider_id};
    model.base_url = base_url;
    model.reasoning = definition.reasoning.value_or(false);
    model.thinking_level_map = definition.thinking_level_map;
    model.input = definition.input.value_or(
        std::vector<ai::ModelInput>{ai::ModelInput::Text});
    model.cost = definition.cost.value_or(ai::ModelCost{});
    model.context_window = definition.context_window.value_or(128000);
    model.max_tokens = definition.max_tokens.value_or(16384);
    model.headers = definition.headers;
    model.compat = std::nullopt;
    return model;
}

[[nodiscard]] support::Expected<std::vector<ai::Model>> apply_models_json(
    std::string_view provider_id,
    const std::vector<ai::Model>& base_models,
    const std::optional<ModelsJsonProvider>& config) {
    if (!config) {
        return base_models;
    }
    const bool has_overrides = config->model_overrides && !config->model_overrides->empty();
    const bool has_models = config->models && !config->models->empty();
    if (!has_models && !config->base_url && !config->headers && !has_overrides &&
        !config->api_key) {
        return std::unexpected(support::make_error(
            support::ErrorCode::ModelValidation,
            "Provider " + std::string{provider_id} +
                ": must specify \"baseUrl\", \"headers\", \"modelOverrides\", or \"models\"."));
    }

    std::vector<ai::Model> models;
    models.reserve(base_models.size());
    for (ai::Model base : base_models) {
        if (config->base_url) {
            base.base_url = *config->base_url;
        }
        models.push_back(std::move(base));
    }
    for (const auto& definition : config->models.value_or(std::vector<ModelsJsonModel>{})) {
        const auto existing = std::find_if(
            models.begin(), models.end(), [&definition](const ai::Model& value) {
                return value.id == definition.id;
            });
        const ai::Model* defaults =
            existing != models.end() ? &*existing
            : models.empty()          ? nullptr
                                      : &models.front();
        auto model_result = model_from_json(provider_id, definition, *config, defaults);
        if (!model_result) {
            return std::unexpected(model_result.error());
        }
        if (existing != models.end()) {
            *existing = std::move(*model_result);
        } else {
            models.push_back(std::move(*model_result));
        }
    }
    return models;
}

[[nodiscard]] ai::Model apply_model_override(
    const ai::Model& model,
    const ModelsJsonModelOverride& override_value) {
    ai::Model result = model;
    if (override_value.name) {
        result.name = *override_value.name;
    }
    if (override_value.reasoning) {
        result.reasoning = *override_value.reasoning;
    }
    if (override_value.thinking_level_map) {
        if (!result.thinking_level_map) {
            result.thinking_level_map = ai::ThinkingLevelMap{};
        }
        for (const auto& [level, value] : *override_value.thinking_level_map) {
            (*result.thinking_level_map)[level] = value;
        }
    }
    if (override_value.input) {
        result.input = *override_value.input;
    }
    if (override_value.cost) {
        ai::ModelCost merged = result.cost;
        if (override_value.cost->input != 0) {
            merged.input = override_value.cost->input;
        }
        if (override_value.cost->output != 0) {
            merged.output = override_value.cost->output;
        }
        if (override_value.cost->cache_read != 0) {
            merged.cache_read = override_value.cost->cache_read;
        }
        if (override_value.cost->cache_write != 0) {
            merged.cache_write = override_value.cost->cache_write;
        }
        if (override_value.cost->tiers) {
            merged.tiers = override_value.cost->tiers;
        }
        result.cost = std::move(merged);
    }
    if (override_value.context_window) {
        result.context_window = *override_value.context_window;
    }
    if (override_value.max_tokens) {
        result.max_tokens = *override_value.max_tokens;
    }
    if (override_value.headers) {
        if (!result.headers) {
            result.headers = ai::ModelHeaders{};
        }
        for (const auto& [name, value] : *override_value.headers) {
            (*result.headers)[name] = value;
        }
    }
    return result;
}

[[nodiscard]] std::optional<ai::ApiKeyAuth> compose_api_key_auth(
    // Owning: the check/resolve closures below are stored on the composed
    // Provider and invoked asynchronously, after the caller's provider-id
    // string (e.g. the recompose loop's id set) is gone (ASan, issue #473).
    std::string provider_id,
    const std::shared_ptr<ai::Provider>& base,
    const std::optional<ModelsJsonProvider>& config,
    const std::shared_ptr<harness::AsyncProcessRunner>& runner) {
    ai::ApiKeyAuth* inherited =
        base && base->auth().api_key ? &*base->auth().api_key : nullptr;
    const std::optional<std::string> raw_key = config ? config->api_key : std::nullopt;
    const bool has_oauth = base && base->auth().oauth.has_value();
    if (inherited == nullptr && !raw_key && has_oauth) {
        // OAuth-only providers get no fabricated API-key login method.
        return std::nullopt;
    }

    ai::ApiKeyAuth result;
    result.name = inherited ? inherited->name : "API key";
    result.login = [base](ai::AuthInteraction interaction)
        -> cch::support::AsyncResult<ai::ApiKeyCredential> {
        return ai::detail::make_async_result(
            [base, interaction = std::move(interaction)]() mutable
                -> boost::asio::awaitable<support::Expected<ai::ApiKeyCredential>> {
                ai::ApiKeyAuth* inherited =
                    base && base->auth().api_key ? &*base->auth().api_key : nullptr;
                if (inherited && inherited->login) {
                    co_return co_await ai::detail::await_async_result(
                        inherited->login(std::move(interaction)));
                }
                CCH_TRY(key, co_await ai::detail::await_async_result(
                    interaction.prompt(ai::AuthPrompt{
                        .kind = ai::AuthPromptSecret{.message = "Enter API key"},
                    })));
                ai::ApiKeyCredential credential;
                credential.key = std::move(key);
                co_return credential;
            });
    };

    const bool is_command = raw_key ? config_value::is_command(*raw_key) : false;
    const std::vector<std::string> raw_key_env_names =
        raw_key ? config_value::env_var_names(*raw_key) : std::vector<std::string>{};
    const std::optional<ai::ModelHeaders> config_headers =
        config ? config->headers : std::nullopt;
    std::vector<std::string> header_env_names;
    if (config_headers) {
        for (const auto& [_, value] : *config_headers) {
            for (const auto& name : config_value::env_var_names(value)) {
                header_env_names.push_back(name);
            }
        }
    }

    result.check = [provider_id, base, raw_key, is_command, raw_key_env_names](
                       const ai::AuthContext& context,
                       std::optional<ai::ApiKeyCredential> credential)
        -> cch::support::AsyncResult<std::optional<AuthCheck>> {
        return ai::detail::make_async_result(
            [&context, credential = std::move(credential), base, raw_key, is_command,
             raw_key_env_names]()
                -> boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> {
                ai::ApiKeyAuth* inherited =
                    base && base->auth().api_key ? &*base->auth().api_key : nullptr;
                if (credential) {
                    if (inherited && inherited->check) {
                        CCH_TRY(checked, co_await ai::detail::await_async_result(
                            inherited->check(context, credential)));
                        co_return checked;
                    }
                    if (credential->key && !credential->key->empty()) {
                        co_return AuthCheck{.source = "stored credential", .type = AuthType::ApiKey};
                    }
                    if (inherited && inherited->resolve) {
                        CCH_TRY(resolved, co_await ai::detail::await_async_result(
                            inherited->resolve(context, credential)));
                        if (resolved) {
                            co_return AuthCheck{.source = resolved->source, .type = AuthType::ApiKey};
                        }
                    }
                    co_return std::optional<AuthCheck>{};
                }
                if (raw_key) {
                    if (is_command) {
                        co_return AuthCheck{.source = "configured API key", .type = AuthType::ApiKey};
                    }
                    for (const auto& name : raw_key_env_names) {
                        CCH_TRY(value, co_await ai::detail::await_async_result(
                            context.environment(name)));
                        if (!value || value->empty()) {
                            co_return std::optional<AuthCheck>{};
                        }
                    }
                    co_return AuthCheck{.source = "configured API key", .type = AuthType::ApiKey};
                }
                if (inherited && inherited->check) {
                    CCH_TRY(checked, co_await ai::detail::await_async_result(
                        inherited->check(context, std::nullopt)));
                    co_return checked;
                }
                if (inherited && inherited->resolve) {
                    CCH_TRY(resolved, co_await ai::detail::await_async_result(
                        inherited->resolve(context, std::nullopt)));
                    if (resolved) {
                        co_return AuthCheck{.source = resolved->source, .type = AuthType::ApiKey};
                    }
                }
                co_return std::optional<AuthCheck>{};
            });
    };

    result.resolve = [provider_id, base, raw_key, raw_key_env_names, config_headers, header_env_names, runner](
                             const ai::AuthContext& context, std::optional<ai::ApiKeyCredential> credential)
            -> cch::support::AsyncResult<std::optional<AuthResult>> {
        return ai::detail::make_async_result(
                [&context,
                        credential = std::move(credential),
                        provider_id,
                        base,
                        raw_key,
                        raw_key_env_names,
                        config_headers,
                        header_env_names,
                        runner]() -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                    ai::ApiKeyAuth* inherited = base && base->auth().api_key ? &*base->auth().api_key : nullptr;
                    std::optional<AuthResult> resolved_result;
                    if (credential) {
                        if (inherited && inherited->resolve) {
                            CCH_TRY(resolved,
                                    co_await ai::detail::await_async_result(inherited->resolve(context, credential)));
                            resolved_result = std::move(resolved);
                        } else if (credential->key && !credential->key->empty()) {
                            ai::ProviderEnv credential_env;
                            credential_env.insert(credential->env.begin(), credential->env.end());
                            resolved_result = AuthResult{
                                    .auth = ModelAuth{.api_key = *credential->key},
                                    .env = std::move(credential_env),
                                    .source = "stored credential",
                            };
                        }
                    } else if (raw_key) {
                        CCH_TRY(env, co_await config_context_env(raw_key_env_names, context, {}));
                        CCH_TRY(key,
                                co_await config_value::resolve_config_value_required(*raw_key,
                                        "API key for provider \"" + std::string{provider_id} + "\"",
                                        env,
                                        runner));
                        if (inherited && inherited->resolve) {
                            ai::ApiKeyCredential configured;
                            configured.key = std::move(key);
                            CCH_TRY(resolved,
                                    co_await ai::detail::await_async_result(
                                            inherited->resolve(context, std::move(configured))));
                            resolved_result = std::move(resolved);
                        } else {
                            resolved_result = AuthResult{
                                    .auth = ModelAuth{.api_key = std::move(key)},
                                    .source = "configured API key",
                            };
                        }
                    } else if (inherited && inherited->resolve) {
                        CCH_TRY(resolved,
                                co_await ai::detail::await_async_result(inherited->resolve(context, std::nullopt)));
                        resolved_result = std::move(resolved);
                    }
                    if (!resolved_result) {
                        co_return std::optional<AuthResult>{};
                    }
                    if (config_headers) {
                        CCH_TRY(header_env,
                                co_await config_context_env(header_env_names, context, resolved_result->env));
                        CCH_TRY(headers, co_await config_value::resolve_headers(*config_headers, header_env, runner));
                        if (headers) {
                            for (const auto& [name, value] : *headers) {
                                resolved_result->auth.headers.insert_or_assign(name, value);
                            }
                        }
                    }
                    co_return resolved_result;
                });
    };

    return result;
}

[[nodiscard]] support::Expected<ai::ProviderAuth> compose_auth(
    std::string_view provider_id,
    const std::shared_ptr<ai::Provider>& base,
    const std::optional<ModelsJsonProvider>& config,
    const std::shared_ptr<harness::AsyncProcessRunner>& runner) {
    ai::ProviderAuth auth;
    auto api_key = compose_api_key_auth(std::string{provider_id}, base, config, runner);
    if (api_key) {
        auth.api_key = std::move(*api_key);
    }
    if (base && base->auth().oauth) {
        auth.oauth = std::move(*base->auth().oauth);
    }
    if (!auth.api_key && !auth.oauth) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
            "Provider " + std::string{provider_id} +
                ": no authentication method configured."));
    }
    return auth;
}

} // namespace

std::map<std::string, std::shared_ptr<ai::Provider>, std::less<>>
builtin_providers(const ProviderComposerOptions& options) {
    std::map<std::string, std::shared_ptr<ai::Provider>, std::less<>> providers;

    ai::ProviderAuth codex_auth;
    codex_auth.oauth = ai::auth::make_openai_codex_oauth_auth();
    providers.emplace(
        "openai-codex",
        ai::providers::make_composed_provider(
            "openai-codex",
            "OpenAI Codex",
            ai::providers::codex_models(),
            std::move(codex_auth),
            options.http_transport,
            options.ws_transport,
            options.codex_cache_config));

    ai::ProviderAuth kimi_auth;
    auto kimi_api_key = ai::providers::make_env_api_key_auth(
        "Kimi API key", std::vector<std::string>{"KIMI_API_KEY"});
    kimi_auth.api_key = std::move(*kimi_api_key.api_key);
    kimi_auth.oauth = ai::auth::make_kimi_coding_oauth_auth();
    providers.emplace(
        "kimi-coding",
        ai::providers::make_composed_provider(
            "kimi-coding",
            "Kimi For Coding",
            ai::providers::kimi_coding_models(),
            std::move(kimi_auth),
            options.http_transport,
            options.ws_transport,
            options.codex_cache_config));

    return providers;
}

std::shared_ptr<ai::Provider> compose_provider(
    std::string_view provider_id,
    std::shared_ptr<ai::Provider> base,
    const ModelConfig& config,
    const ProviderComposerOptions& options,
    std::optional<std::string>& error) {
    const auto& config_entry = config.provider(provider_id);
    if (!base && !config_entry) {
        return nullptr;
    }
    if (base && !config_entry) {
        // No overlays: use the built-in untouched so its auth/login/stream
        // behavior is exact (pi recomposeProvider).
        error.reset();
        return base;
    }
    auto models_result = apply_models_json(
        provider_id,
        base ? base->models() : std::vector<ai::Model>{},
        config_entry);
    if (!models_result) {
        error = models_result.error().message;
        return base;
    }
    std::vector<ai::Model> models = std::move(*models_result);
    if (config_entry->model_overrides) {
        for (ai::Model& model : models) {
            if (const auto found = config_entry->model_overrides->find(model.id);
                found != config_entry->model_overrides->end()) {
                model = apply_model_override(model, found->second);
            }
        }
    }
    std::string name = config_entry->name.value_or(
        base ? std::string{base->name()} : std::string{provider_id});
    auto auth_result = compose_auth(
        provider_id, base, config_entry, options.process_runner);
    if (!auth_result) {
        error = auth_result.error().message;
        return base;
    }
    auto provider = ai::providers::make_composed_provider(
        std::string{provider_id},
        std::move(name),
        std::move(models),
        std::move(*auth_result),
        options.http_transport,
        options.ws_transport,
        options.codex_cache_config);
    error.reset();
    return provider;
}

std::vector<std::string> configured_api_key_env_names(const ModelConfig& config) {
    std::vector<std::string> names;
    std::set<std::string, std::less<>> seen;
    for (const auto& provider_id : config.provider_ids()) {
        const auto& entry = config.provider(provider_id);
        if (entry && entry->api_key) {
            for (const auto& name : config_value::env_var_names(*entry->api_key)) {
                if (seen.insert(name).second) {
                    names.push_back(name);
                }
            }
        }
    }
    return names;
}

std::optional<ModelRuntimeAuthStatus> configured_request_auth_status(
    const ModelsJsonProvider& config) {
    if (!config.api_key) {
        return std::nullopt;
    }
    if (config_value::is_command(*config.api_key)) {
        return ModelRuntimeAuthStatus{
            .configured = true,
            .source = "models_json_command",
        };
    }
    const auto names = config_value::env_var_names(*config.api_key);
    if (!names.empty()) {
        const bool configured = config_value::missing_env_var_names(*config.api_key, {}).empty();
        if (!configured) {
            return ModelRuntimeAuthStatus{.configured = false};
        }
        return ModelRuntimeAuthStatus{
            .configured = true,
            .source = "environment",
            .label = config_value::join_strings(names),
        };
    }
    return ModelRuntimeAuthStatus{
        .configured = true,
        .source = "models_json_key",
    };
}

std::optional<std::string> default_model_for_provider(std::string_view provider_id) {
    if (provider_id == "openai-codex") {
        return "gpt-5.5";
    }
    if (provider_id == "kimi-coding") {
        return "kimi-for-coding";
    }
    return std::nullopt;
}

} // namespace cch::coding_agent
