#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/ProviderTestAccess.hpp"
#include "SimpleOptions.hpp"
#include "support/BoundedText.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#include <exception>
#endif
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::ai {
namespace {

constexpr std::chrono::milliseconds kOAuthMinimumValidity{std::chrono::minutes{5}};
constexpr std::size_t kMaxPublicErrorBytes = 1024;

[[nodiscard]] TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] bool expires_soon(const OAuthCredential& credential) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return credential.expires <= now + kOAuthMinimumValidity.count();
}

[[nodiscard]] support::Error categorized_error(
    support::ErrorCode code,
    std::string message,
    const support::Error& cause) {
    std::string detail = cause.message;
    if (!cause.detail.empty()) {
        detail += ": ";
        detail += cause.detail;
    }
    return support::make_error(code,
            support::bounded_redacted_text(std::move(message), kMaxPublicErrorBytes, "..."),
            support::bounded_redacted_text(std::move(detail), kMaxPublicErrorBytes, "..."));
}

[[nodiscard]] support::Error safe_error(support::Error error) {
    error.message = support::bounded_redacted_text(std::move(error.message), kMaxPublicErrorBytes, "...");
    error.detail = support::bounded_redacted_text(std::move(error.detail), kMaxPublicErrorBytes, "...");
    if (error.context) {
        error.context = support::bounded_redacted_text(std::move(*error.context), kMaxPublicErrorBytes, "...");
    }
    return error;
}

template <typename T>
struct AsyncResultValue;

template <typename T, typename E>
struct AsyncResultValue<cch::support::AsyncResult<T, E>> {
    using type = std::expected<T, E>;
};

/// Await one `AsyncResult`-returning operation from within an asio coroutine.
/// Callback and producer failures already use the operation's explicit
/// `Expected` terminal value; this helper only carries that value across the
/// private Asio bridge.
template <typename Operation>
[[nodiscard]] boost::asio::awaitable<
    typename AsyncResultValue<std::invoke_result_t<Operation&>>::type>
invoke_async_operation(Operation operation) {
    auto result = operation();
    co_return co_await support::detail::await_async_result(std::move(result));
}

[[nodiscard]] support::ExpectedVoid emit_assistant_event(
    AssistantEventSink& sink,
    const AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    return sink(event);
}

[[nodiscard]] std::string public_error_diagnostic(const support::Error& error) {
    std::string diagnostic = error.message;
    if (!error.detail.empty() && diagnostic.find(error.detail) == std::string::npos) {
        if (!diagnostic.empty()) {
            diagnostic += ": ";
        }
        diagnostic += error.detail;
    }
    return support::bounded_redacted_text(std::move(diagnostic), kMaxPublicErrorBytes, "...");
}

[[nodiscard]] AssistantMessage safe_terminal_message(
    AssistantMessage message,
    const std::optional<support::Error>& failure = std::nullopt) {
    if (message.error_message) {
        message.error_message =
                support::bounded_redacted_text(std::move(*message.error_message), kMaxPublicErrorBytes, "...");
    } else if (failure) {
        message.error_message = public_error_diagnostic(*failure);
    }
    return message;
}

/// Frozen pi treats each Provider catalog as best-effort: a throwing callback
/// contributes no models so other Providers remain discoverable.
[[nodiscard]] std::vector<Model> safe_provider_models(const Provider& provider_value) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
        return provider_value.models();
    } catch (...) {
        return {};
    }
#else
    return provider_value.models();
#endif
}

[[nodiscard]] std::shared_ptr<Provider> make_default_provider(ProviderDefinition definition) {
    auto http_transport = std::make_shared<providers::BoostBeastStreamTransport>();
    auto websocket_transport = std::make_shared<providers::BoostBeastWebSocketTransport>();
    return providers::make_composed_provider(std::move(definition.id),
            std::move(definition.name),
            std::move(definition.models),
            std::move(definition.auth),
            std::move(http_transport),
            std::move(websocket_transport));
}

[[nodiscard]] bool header_name_equal(std::string_view left, std::string_view right) {
    return std::ranges::equal(
        left,
        right,
        [](char left_character, char right_character) {
            const auto lower = [](char character) {
                if (character >= 'A' && character <= 'Z') {
                    return static_cast<char>(character - 'A' + 'a');
                }
                return character;
            };
            return lower(left_character) == lower(right_character);
        });
}

template <typename Headers>
void erase_header(Headers& target, std::string_view name) {
    std::erase_if(target, [&name](const auto& entry) {
        return header_name_equal(entry.first, name);
    });
}

void merge_headers(ProviderHeaders& target, const ModelHeaders& overrides) {
    for (const auto& [name, value] : overrides) {
        erase_header(target, name);
        target.emplace(name, value);
    }
}

void merge_request_headers(RequestHeaders& target, const RequestHeaders& overrides) {
    for (const auto& [name, value] : overrides) {
        erase_header(target, name);
        target.emplace(name, value);
    }
}

void set_request_header(RequestHeaders& target, std::string name, std::string value) {
    erase_header(target, name);
    target.emplace(std::move(name), std::move(value));
}

[[nodiscard]] RequestHeaders request_headers_from_auth(const ModelAuth& auth) {
    RequestHeaders result;
    for (const auto& [name, value] : auth.headers) {
        result.emplace(name, value);
    }
    return result;
}

[[nodiscard]] ProviderHeaders concrete_headers(const RequestHeaders& headers) {
    ProviderHeaders result;
    for (const auto& [name, value] : headers) {
        if (value) {
            result.emplace(name, *value);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> deleted_headers(
    const RequestHeaders& headers) {
    std::vector<std::string> result;
    for (const auto& [name, value] : headers) {
        if (!value) {
            result.push_back(name);
        }
    }
    return result;
}

[[nodiscard]] bool has_non_empty_header(
    const ProviderHeaders& headers,
    std::string_view expected_name) {
    return std::ranges::any_of(headers, [&expected_name](const auto& entry) {
        return header_name_equal(entry.first, expected_name) &&
               std::ranges::any_of(entry.second, [](char character) {
                   return character != ' ' && character != '\t' &&
                          character != '\r' && character != '\n';
               });
    });
}

[[nodiscard]] support::ExpectedVoid assert_request_auth(
    const Model& model,
    const ModelAuth& auth) {
    const bool scoped_api = model.api == "openai-codex-responses" ||
                            model.api == "openai-responses" ||
                            model.api == "anthropic-messages";
    if (!scoped_api || (auth.api_key && !auth.api_key->empty()) ||
        has_non_empty_header(auth.headers, "authorization") ||
        has_non_empty_header(auth.headers, "x-api-key") ||
        has_non_empty_header(auth.headers, "cf-aig-authorization")) {
        return {};
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Auth,
        "No API key for provider: " + model.provider));
}

[[nodiscard]] support::Expected<RequestHeaders> transform_request_headers(
    RequestHeaders headers,
    TransformHeadersHook& transform) {
    if (!transform) {
        return headers;
    }
    return transform(std::move(headers));
}

[[nodiscard]] ModelThinkingLevel to_model_thinking_level(ThinkingLevel level) {
    switch (level) {
    case ThinkingLevel::Minimal:
        return ModelThinkingLevel::Minimal;
    case ThinkingLevel::Low:
        return ModelThinkingLevel::Low;
    case ThinkingLevel::Medium:
        return ModelThinkingLevel::Medium;
    case ThinkingLevel::High:
        return ModelThinkingLevel::High;
    case ThinkingLevel::XHigh:
        return ModelThinkingLevel::XHigh;
    case ThinkingLevel::Max:
        return ModelThinkingLevel::Max;
    }
    return ModelThinkingLevel::Medium;
}

struct PreparedProviderRequest {
    Model model{};
    ProviderStreamOptions options{};
};

[[nodiscard]] support::Expected<PreparedProviderRequest> prepare_provider_request(
    Model model,
    const AiContext& context,
    AuthResult auth_result,
    SimpleStreamOptions options) {
    if (auth_result.auth.base_url) {
        model.base_url = *auth_result.auth.base_url;
    }

    auto request_env = std::move(auth_result.env);
    for (const auto& [name, value] : options.env) {
        request_env.insert_or_assign(name, value);
    }
    const auto cache_retention = detail::resolve_cache_retention(
        options.cache_retention, request_env);
    const auto request_session_id = cache_retention == CacheRetention::None
        ? std::optional<std::string>{}
        : options.session_id;

    auto request_headers = request_headers_from_auth(auth_result.auth);
    if (request_session_id) {
        if (model.api == "openai-codex-responses") {
            const auto codex_session_id =
                detail::clamp_openai_prompt_cache_key(*request_session_id);
            set_request_header(request_headers, "session-id", codex_session_id);
            set_request_header(request_headers, "x-client-request-id", codex_session_id);
        } else if (model.api == "openai-responses") {
            set_request_header(request_headers, "session_id", *request_session_id);
            set_request_header(request_headers, "x-client-request-id", *request_session_id);
        }
    }
    merge_request_headers(request_headers, options.headers);
    auto transformed_headers = transform_request_headers(
        std::move(request_headers), options.transform_headers);
    if (!transformed_headers) {
        return std::unexpected(transformed_headers.error());
    }

    auto request_deleted_headers = deleted_headers(*transformed_headers);
    auth_result.auth.headers = concrete_headers(*transformed_headers);
    if (auto asserted = assert_request_auth(model, auth_result.auth); !asserted) {
        return std::unexpected(asserted.error());
    }

    const auto reasoning = options.reasoning
        ? std::optional<ModelThinkingLevel>{clamp_thinking_level(
              model, to_model_thinking_level(*options.reasoning))}
        : std::nullopt;
    const auto requested_max_tokens = options.max_tokens.value_or(model.max_tokens);
    const auto max_tokens = detail::clamp_max_tokens_to_context(
        model, context, requested_max_tokens);
    return PreparedProviderRequest{
        .model = std::move(model),
        .options = ProviderStreamOptions{
            .auth = std::move(auth_result.auth),
            .deleted_headers = std::move(request_deleted_headers),
            .env = std::move(request_env),
            .temperature = options.temperature,
            .max_tokens = max_tokens,
            .reasoning = reasoning,
            .session_id = request_session_id,
            .cache_retention = cache_retention,
            .timeout_ms = options.timeout_ms,
            .max_retries = options.max_retries,
            .max_retry_delay_ms = options.max_retry_delay_ms.value_or(60000),
            .stop_token = options.stop_token,
        },
    };
}

[[nodiscard]] bool is_models_domain_error(support::ErrorCode code) {
    return code == support::ErrorCode::ModelSource ||
           code == support::ErrorCode::ModelValidation ||
           code == support::ErrorCode::Provider ||
           code == support::ErrorCode::Stream ||
           code == support::ErrorCode::Auth ||
           code == support::ErrorCode::OAuth;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> terminal_failure(
    const Model& model,
    support::Error failure,
    AssistantEventSink& sink,
    AssistantStopReason reason = AssistantStopReason::Error) {
    failure = safe_error(std::move(failure));

    AssistantMessage message;
    message.api = model.api;
    message.provider = model.provider;
    message.model = model.id;
    message.stop_reason = reason;
    message.error_message = public_error_diagnostic(failure);
    message.timestamp = current_timestamp_ms();

    CCH_TRY_VOID(emit_assistant_event(
        sink,
        AssistantErrorEvent{
            .reason = reason,
            .error = message,
            .failure = failure,
        }));
    co_return message;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> terminal_message_value(
    AssistantMessage message,
    support::Error failure,
    AssistantEventSink& sink) {
    failure = safe_error(std::move(failure));
    message = safe_terminal_message(std::move(message), failure);
    CCH_TRY_VOID(emit_assistant_event(
        sink,
        AssistantErrorEvent{
            .reason = message.stop_reason,
            .error = message,
            .failure = failure,
        }));
    co_return message;
}

} // namespace

struct Models::Impl {
    std::shared_ptr<CredentialStore> credentials;
    std::shared_ptr<AuthContext> auth_context;
    std::map<std::string, std::shared_ptr<Provider>, std::less<>> providers;

    [[nodiscard]] std::shared_ptr<Provider> provider(std::string_view provider_id) const {
        const auto found = providers.find(provider_id);
        return found == providers.end() ? nullptr : found->second;
    }

    [[nodiscard]] support::ExpectedVoid install_provider(std::shared_ptr<Provider> provider_value) {
        if (!provider_value || provider_value->id().empty()) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
        }
        const auto& auth = provider_value->auth();
        if (!auth.api_key && !auth.oauth) {
            return std::unexpected(support::make_error(support::ErrorCode::Auth,
                    "provider must define authentication",
                    std::string{provider_value->id()}));
        }
        providers.insert_or_assign(std::string{provider_value->id()}, std::move(provider_value));
        return {};
    }

    void remove_provider(std::string_view provider_id) { providers.erase(provider_id); }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> resolve_api_key(
        const Provider& provider,
        ApiKeyAuth& auth,
        std::optional<ApiKeyCredential> credential) {
        auto resolved = co_await invoke_async_operation(
            [&]() {
                return auth.resolve(*auth_context, std::move(credential));
            });
        if (!resolved) {
            co_return std::unexpected(categorized_error(
                support::ErrorCode::Auth,
                "API key auth failed for provider " + std::string{provider.id()},
                resolved.error()));
        }
        co_return std::move(*resolved);
    }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> resolve_oauth(
        const Provider& provider,
        OAuthAuth& auth,
        OAuthCredential credential) {
        const std::string provider_id{provider.id()};
        if (expires_soon(credential)) {
            auto modified = co_await invoke_async_operation([&]() -> cch::support::AsyncResult<
                                                                          std::optional<Credential>> {
                return credentials->modify(provider_id,
                        [&auth, provider_id](std::optional<Credential> current)
                                -> cch::support::AsyncResult<std::optional<Credential>> {
                            return support::detail::make_async_result(
                                    [&auth, provider_id, current = std::move(current)]()
                                            -> boost::asio::awaitable<support::Expected<std::optional<Credential>>> {
                                        auto* current_oauth =
                                                current ? std::get_if<OAuthCredential>(&*current) : nullptr;
                                        if (current_oauth == nullptr || !expires_soon(*current_oauth)) {
                                            co_return std::optional<Credential>{};
                                        }
                                        auto refreshed = co_await invoke_async_operation(
                                                [&]() { return auth.refresh(*current_oauth); });
                                        if (!refreshed) {
                                            co_return std::unexpected(categorized_error(support::ErrorCode::OAuth,
                                                    "OAuth refresh failed for " + provider_id,
                                                    refreshed.error()));
                                        }
                                        co_return std::optional<Credential>{Credential{std::move(*refreshed)}};
                                    });
                        });
            });
            if (!modified) {
                if (modified.error().code == support::ErrorCode::OAuth) {
                    co_return std::unexpected(modified.error());
                }
                co_return std::unexpected(categorized_error(
                    support::ErrorCode::Auth,
                    "Credential store modify failed for " + provider_id,
                    modified.error()));
            }
            if (!*modified) {
                co_return std::optional<AuthResult>{};
            }
            const auto* refreshed = std::get_if<OAuthCredential>(&**modified);
            if (refreshed == nullptr) {
                co_return std::optional<AuthResult>{};
            }
            credential = *refreshed;
        }

        auto request_auth = co_await invoke_async_operation(
            [&]() {
                return auth.to_auth(credential);
            });
        if (!request_auth) {
            co_return std::unexpected(categorized_error(
                support::ErrorCode::OAuth,
                "OAuth auth derivation failed for " + provider_id,
                request_auth.error()));
        }
        co_return AuthResult{
            .auth = std::move(*request_auth),
            .env = {},
            .source = "OAuth",
        };
    }
};

support::ExpectedVoid providers::ProviderTestAccess::install(Models& models, std::shared_ptr<Provider> provider) {
    return models.impl_->install_provider(std::move(provider));
}

support::ExpectedVoid providers::ProviderTestAccess::replace_transports(Models& models,
        std::shared_ptr<StreamTransport> http_transport,
        std::shared_ptr<WebSocketTransport> ws_transport,
        CodexWebSocketCacheConfig cache_config) {
    for (const auto& [provider_id, provider] : models.impl_->providers) {
        auto replacement = providers::make_composed_provider(provider_id,
                std::string{provider->name()},
                provider->models(),
                std::move(provider->auth()),
                http_transport,
                ws_transport,
                cache_config);
        if (auto installed = models.impl_->install_provider(std::move(replacement)); !installed) {
            return std::unexpected(installed.error());
        }
    }
    return {};
}

Models::Models(
    std::shared_ptr<CredentialStore> credentials,
    std::shared_ptr<AuthContext> auth_context)
    : impl_(std::make_unique<Impl>(Impl{
          .credentials = std::move(credentials),
          .auth_context = std::move(auth_context),
          .providers = {},
      })) {}

Models::Models(Models&&) noexcept = default;
Models& Models::operator=(Models&&) noexcept = default;
Models::~Models() = default;

support::ExpectedVoid Models::apply_provider(ProviderChange change) {
    if (change.definition) {
        auto definition = std::move(*change.definition);
        if (definition.id.empty()) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
        }
        if (!change.provider_id.empty() && change.provider_id != definition.id) {
            return std::unexpected(support::make_error(support::ErrorCode::Provider,
                    "provider change id does not match provider definition",
                    definition.id));
        }
        return impl_->install_provider(make_default_provider(std::move(definition)));
    }
    if (change.provider_id.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Provider, "provider id is required"));
    }
    impl_->remove_provider(change.provider_id);
    return {};
}

void Models::clear_providers() {
    impl_->providers.clear();
}

std::vector<ProviderInfo> Models::provider_info() const {
    std::vector<ProviderInfo> result;
    result.reserve(impl_->providers.size());
    for (const auto& [_, provider_value] : impl_->providers) {
        ProviderInfo info{
                .id = std::string{provider_value->id()},
                .name = std::string{provider_value->name()},
                .auth_methods = {},
        };
        const auto& auth = provider_value->auth();
        // Keep the existing login presentation order: OAuth before API key.
        if (auth.oauth) {
            info.auth_methods.push_back(AuthMethodInfo{
                    .type = AuthType::OAuth,
                    .name = auth.oauth->name,
                    .has_login = static_cast<bool>(auth.oauth->login),
            });
        }
        if (auth.api_key) {
            info.auth_methods.push_back(AuthMethodInfo{
                    .type = AuthType::ApiKey,
                    .name = auth.api_key->name,
                    .has_login = static_cast<bool>(auth.api_key->login),
            });
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<Model> Models::models(std::optional<std::string_view> provider_id) const {
    if (provider_id) {
        const auto selected = impl_->provider(*provider_id);
        return selected ? safe_provider_models(*selected) : std::vector<Model>{};
    }
    std::vector<Model> result;
    for (const auto& [_, provider_value] : impl_->providers) {
        auto current = safe_provider_models(*provider_value);
        result.insert(
            result.end(),
            std::make_move_iterator(current.begin()),
            std::make_move_iterator(current.end()));
    }
    return result;
}

std::optional<Model> Models::model(
    std::string_view provider_id,
    std::string_view model_id) const {
    auto available = models(provider_id);
    const auto found = std::find_if(
        available.begin(), available.end(), [&model_id](const Model& value) {
            return value.id == model_id;
        });
    if (found == available.end()) {
        return std::nullopt;
    }
    return *found;
}

cch::support::AsyncResult<std::optional<AuthCheck>> Models::check_auth(
    std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthCheck>>> {
                const auto selected = impl_->provider(provider_id);
                if (!selected) {
                    co_return std::optional<AuthCheck>{};
                }

                auto stored = co_await invoke_async_operation([&]() { return impl_->credentials->read(provider_id); });
                if (!stored) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store read failed for " + provider_id,
                            stored.error()));
                }

                auto& auth = selected->auth();
                if (*stored) {
                    if (std::holds_alternative<OAuthCredential>(**stored)) {
                        if (!auth.oauth) {
                            co_return std::optional<AuthCheck>{};
                        }
                        co_return AuthCheck{.source = "OAuth", .type = AuthType::OAuth};
                    }
                    if (!auth.api_key) {
                        co_return std::optional<AuthCheck>{};
                    }
                    const auto credential = std::get<ApiKeyCredential>(**stored);
                    if (auth.api_key->check) {
                        auto checked = co_await invoke_async_operation(
                                [&]() { return auth.api_key->check(*impl_->auth_context, credential); });
                        if (!checked) {
                            co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                                    "API key auth check failed for provider " + provider_id,
                                    checked.error()));
                        }
                        co_return std::move(*checked);
                    }
                    CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, credential));
                    if (!resolved) {
                        co_return std::optional<AuthCheck>{};
                    }
                    co_return AuthCheck{
                            .source = resolved->source,
                            .type = AuthType::ApiKey,
                    };
                }

                if (!auth.api_key) {
                    co_return std::optional<AuthCheck>{};
                }
                if (auth.api_key->check) {
                    auto checked = co_await invoke_async_operation(
                            [&]() { return auth.api_key->check(*impl_->auth_context, std::nullopt); });
                    if (!checked) {
                        co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                                "API key auth check failed for provider " + provider_id,
                                checked.error()));
                    }
                    co_return std::move(*checked);
                }
                CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::nullopt));
                if (!resolved) {
                    co_return std::optional<AuthCheck>{};
                }
                co_return AuthCheck{
                        .source = resolved->source,
                        .type = AuthType::ApiKey,
                };
            });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
    std::string provider_id,
    std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id), explicit_api_key = std::move(explicit_api_key)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                const auto selected = impl_->provider(provider_id);
                if (!selected) {
                    co_return std::optional<AuthResult>{};
                }
                auto& auth = selected->auth();

                if (explicit_api_key && auth.api_key) {
                    ApiKeyCredential credential;
                    credential.key = std::move(explicit_api_key);
                    CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::move(credential)));
                    co_return resolved;
                }

                auto stored = co_await invoke_async_operation([&]() { return impl_->credentials->read(provider_id); });
                if (!stored) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store read failed for " + provider_id,
                            stored.error()));
                }
                if (*stored) {
                    if (auto* oauth = std::get_if<OAuthCredential>(&**stored)) {
                        if (!auth.oauth) {
                            co_return std::optional<AuthResult>{};
                        }
                        CCH_TRY(resolved, co_await impl_->resolve_oauth(*selected, *auth.oauth, *oauth));
                        co_return resolved;
                    }
                    if (!auth.api_key) {
                        co_return std::optional<AuthResult>{};
                    }
                    CCH_TRY(resolved,
                            co_await impl_->resolve_api_key(
                                    *selected, *auth.api_key, std::get<ApiKeyCredential>(**stored)));
                    co_return resolved;
                }

                if (!auth.api_key) {
                    co_return std::optional<AuthResult>{};
                }
                CCH_TRY(resolved, co_await impl_->resolve_api_key(*selected, *auth.api_key, std::nullopt));
                co_return resolved;
            });
}

cch::support::AsyncResult<std::optional<AuthResult>> Models::get_auth(
    Model model_value,
    std::optional<std::string> explicit_api_key) {
    return support::detail::make_async_result(
            [this, model_value = std::move(model_value), explicit_api_key = std::move(explicit_api_key)]()
                    -> boost::asio::awaitable<support::Expected<std::optional<AuthResult>>> {
                CCH_TRY(resolved,
                        co_await support::detail::await_async_result(
                                get_auth(model_value.provider, std::move(explicit_api_key))));
                if (!resolved || !model_value.headers) {
                    co_return resolved;
                }
                merge_headers(resolved->auth.headers, *model_value.headers);
                co_return resolved;
            });
}

[[nodiscard]] std::string_view auth_type_string(AuthType type) {
    switch (type) {
    case AuthType::ApiKey:
        return "api_key";
    case AuthType::OAuth:
        return "oauth";
    }
    return "oauth";
}

cch::support::AsyncResult<void> Models::logout(
    std::string provider_id) {
    return support::detail::make_async_result(
            [this, provider_id = std::move(provider_id)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                auto removed =
                        co_await invoke_async_operation([&]() { return impl_->credentials->remove(provider_id); });
                if (!removed) {
                    co_return std::unexpected(categorized_error(support::ErrorCode::Auth,
                            "Credential store delete failed for " + provider_id,
                            removed.error()));
                }
                co_return support::ExpectedVoid{};
            });
}

cch::support::AsyncResult<Credential> Models::login(
    std::string provider_id,
    AuthType type,
    AuthInteraction interaction) {
    return support::detail::make_async_result([this,
                                                      provider_id = std::move(provider_id),
                                                      type,
                                                      interaction = std::move(interaction)]() mutable
                                                      -> boost::asio::awaitable<support::Expected<Credential>> {
        const auto selected = impl_->provider(provider_id);
        if (!selected) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Provider, "Unknown provider: " + provider_id));
        }
        auto& auth = selected->auth();
        const auto type_name = std::string{auth_type_string(type)};

        support::Expected<Credential> flow_result;
        if (type == AuthType::OAuth) {
            if (!auth.oauth || !auth.oauth->login) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                        std::string{selected->name()} + " does not support " + type_name + " login"));
            }
            auto credential =
                    co_await invoke_async_operation([&]() { return auth.oauth->login(std::move(interaction)); });
            if (!credential) {
                // Login-flow failures propagate unwrapped to the host.
                co_return std::unexpected(std::move(credential.error()));
            }
            flow_result = std::move(*credential);
        } else {
            if (!auth.api_key || !auth.api_key->login) {
                co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
                        std::string{selected->name()} + " does not support " + type_name + " login"));
            }
            auto credential =
                    co_await invoke_async_operation([&]() { return auth.api_key->login(std::move(interaction)); });
            if (!credential) {
                co_return std::unexpected(std::move(credential.error()));
            }
            flow_result = std::move(*credential);
        }

        // Persist exclusively through CredentialStore::modify, the only write
        // path. Only CredentialStore failures wrap as the `auth` category.
        support::Expected<Credential> login_credential = std::move(flow_result);
        auto stored =
                co_await invoke_async_operation([&]() -> cch::support::AsyncResult<std::optional<Credential>> {
                    return impl_->credentials->modify(
                        provider_id,
                        [credential = login_credential](std::optional<Credential>)
                            -> cch::support::AsyncResult<std::optional<Credential>> {
                            return cch::support::AsyncResult<std::optional<Credential>>(
                                std::expected<std::optional<Credential>, cch::support::Error>{
                                    std::optional<Credential>{*credential}});
                        });
                });
        if (!stored) {
            co_return std::unexpected(categorized_error(
                    support::ErrorCode::Auth, "Credential store modify failed for " + provider_id, stored.error()));
        }
        co_return std::move(login_credential);
    });
}

namespace {

/// Private one-turn stream implementation (the former public `stream_simple`
/// surface, now private; #455). Runs on the consuming executor, resolves
/// authentication, normalizes model/auth/request failures to one terminal
/// event, delegates to the Provider's move-only `ModelStream`, and reaches
/// exactly one terminal outcome. `self` keeps the Models Runtime alive for the
/// whole stream.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> stream_impl(std::shared_ptr<Models> self,
        std::shared_ptr<Provider> selected,
        Model model_value,
        AiContext context,
        SimpleStreamOptions options,
        AssistantEventSink sink) {
    if (!selected) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            support::make_error(
                support::ErrorCode::Provider,
                "Unknown provider: " + model_value.provider),
            sink));
        co_return terminal;
    }

    auto auth = co_await invoke_async_operation(
        [&]() {
            return self->get_auth(model_value, std::move(options.api_key));
        });
    if (!auth) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value, auth.error(), sink));
        co_return terminal;
    }
    if (!*auth) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            support::make_error(
                support::ErrorCode::Auth,
                "Provider is not configured: " + model_value.provider),
            sink));
        co_return terminal;
    }

    if (auto valid = validate_model(model_value); !valid) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            categorized_error(
                support::ErrorCode::ModelValidation,
                valid.error().message,
                valid.error()),
            sink));
        co_return terminal;
    }

    auto prepared = prepare_provider_request(
        model_value, context, std::move(**auth), std::move(options));
    if (!prepared) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value, prepared.error(), sink));
        co_return terminal;
    }

    std::optional<AssistantMessage> terminal_message;
    std::optional<support::Error> sink_failure;
    AssistantEventSink forwarding_sink =
        [&sink, &terminal_message, &sink_failure](
            const AssistantStreamEvent& event) -> support::ExpectedVoid {
            if (sink_failure) {
                return std::unexpected(*sink_failure);
            }
            std::optional<AssistantStreamEvent> safe_terminal;
            if (const auto* done = std::get_if<AssistantDoneEvent>(&event)) {
                if (terminal_message) {
                    return {};
                }
                terminal_message = safe_terminal_message(done->message);
                safe_terminal = AssistantDoneEvent{
                    .reason = done->reason,
                    .message = *terminal_message,
                };
            } else if (const auto* error = std::get_if<AssistantErrorEvent>(&event)) {
                if (terminal_message) {
                    return {};
                }
                auto failure = error->failure;
                if (failure) {
                    failure = safe_error(std::move(*failure));
                }
                terminal_message = safe_terminal_message(error->error, failure);
                safe_terminal = AssistantErrorEvent{
                    .reason = error->reason,
                    .error = *terminal_message,
                    .failure = std::move(failure),
                };
            }
            const auto& forwarded = safe_terminal ? *safe_terminal : event;
            if (auto emitted = emit_assistant_event(sink, forwarded); !emitted) {
                sink_failure = emitted.error();
                return std::unexpected(*sink_failure);
            }
            return {};
        };
    auto result = co_await [&]() -> boost::asio::awaitable<support::Expected<AssistantMessage>> {
        auto provider_stream = selected->stream(
            std::move(prepared->model),
            std::move(context),
            std::move(prepared->options));
        co_return co_await support::detail::await_async_result(
                std::move(provider_stream).run(std::move(forwarding_sink)));
    }();

    if (sink_failure) {
        co_return std::unexpected(*sink_failure);
    }
    if (result) {
        if (terminal_message) {
            co_return std::move(*terminal_message);
        }
        const auto reason = result->stop_reason;
        if (reason == AssistantStopReason::Error ||
            reason == AssistantStopReason::Aborted) {
            const auto code = reason == AssistantStopReason::Aborted
                ? support::ErrorCode::Cancelled
                : support::ErrorCode::Stream;
            const auto diagnostic = result->error_message.value_or(
                reason == AssistantStopReason::Aborted
                    ? "Request was aborted"
                    : "Provider stream failed");
            auto message = std::move(*result);
            CCH_TRY(terminal, co_await terminal_message_value(
                std::move(message),
                support::make_error(code, diagnostic),
                sink));
            co_return terminal;
        }
        CCH_TRY_VOID(emit_assistant_event(
            sink,
            AssistantDoneEvent{
                .reason = reason,
                .message = *result,
            }));
        co_return std::move(*result);
    }

    if (terminal_message &&
        (result.error().code == support::ErrorCode::Cancelled ||
         is_models_domain_error(result.error().code))) {
        co_return std::move(*terminal_message);
    }
    if (result.error().code == support::ErrorCode::Cancelled) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
            sink,
            AssistantStopReason::Aborted));
        co_return terminal;
    }
    if (is_models_domain_error(result.error().code)) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value, result.error(), sink));
        co_return terminal;
    }
    CCH_TRY(message, std::move(result));
    co_return message;
}

} // namespace

ModelStream Models::stream(
    Model model,
    AiContext context,
    SimpleStreamOptions options) {
    auto self = shared_from_this();
    return detail::make_model_stream(
            [self, model = std::move(model), context = std::move(context), options = std::move(options)](
                    AssistantEventSink sink) mutable -> boost::asio::awaitable<support::Expected<AssistantMessage>> {
                auto selected = self->impl_->provider(model.provider);
                co_return co_await stream_impl(self,
                        std::move(selected),
                        std::move(model),
                        std::move(context),
                        std::move(options),
                        std::move(sink));
            });
}

} // namespace cch::ai
