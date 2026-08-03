#include <cch/ai/Models.hpp>

#include "util/BoundedText.hpp"
#include "util/ExpectedMacros.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
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

[[nodiscard]] util::Error categorized_error(
    util::ErrorCode code,
    std::string message,
    const util::Error& cause) {
    std::string detail = cause.message;
    if (!cause.detail.empty()) {
        detail += ": ";
        detail += cause.detail;
    }
    return util::make_error(
        code,
        util::bounded_redacted_text(std::move(message), kMaxPublicErrorBytes, "..."),
        util::bounded_redacted_text(std::move(detail), kMaxPublicErrorBytes, "..."));
}

[[nodiscard]] util::Error safe_error(util::Error error) {
    error.message = util::bounded_redacted_text(
        std::move(error.message), kMaxPublicErrorBytes, "...");
    error.detail = util::bounded_redacted_text(
        std::move(error.detail), kMaxPublicErrorBytes, "...");
    if (error.context) {
        error.context = util::bounded_redacted_text(
            std::move(*error.context), kMaxPublicErrorBytes, "...");
    }
    return error;
}

[[nodiscard]] util::Error callback_exception_error(
    util::ErrorCode code,
    std::string message,
    std::string detail = "unknown exception") {
    return safe_error(util::make_error(
        code,
        std::move(message),
        std::move(detail)));
}

template <typename T>
struct AwaitableResult;

template <typename T, typename Executor>
struct AwaitableResult<boost::asio::awaitable<T, Executor>> {
    using type = T;
};

template <typename Operation>
[[nodiscard]] boost::asio::awaitable<
    typename AwaitableResult<std::invoke_result_t<Operation&>>::type>
invoke_models_callback(
    util::ErrorCode code,
    std::string message,
    Operation operation) {
    using Result = typename AwaitableResult<std::invoke_result_t<Operation&>>::type;
    try {
        if constexpr (std::is_void_v<typename Result::value_type>) {
            CCH_TRY_VOID(co_await operation());
            co_return Result{};
        } else {
            CCH_TRY(value, co_await operation());
            co_return value;
        }
    } catch (const std::exception& error) {
        co_return Result{std::unexpected(callback_exception_error(
            code,
            std::move(message),
            error.what()))};
    } catch (...) {
        co_return Result{std::unexpected(callback_exception_error(
            code,
            std::move(message)))};
    }
}

[[nodiscard]] util::ExpectedVoid emit_assistant_event(
    AssistantEventSink& sink,
    const AssistantStreamEvent& event) {
    if (!sink) {
        return {};
    }
    try {
        return sink(event);
    } catch (const std::exception& error) {
        return std::unexpected(callback_exception_error(
            util::ErrorCode::Unknown,
            "Assistant event sink failed",
            error.what()));
    } catch (...) {
        return std::unexpected(callback_exception_error(
            util::ErrorCode::Unknown,
            "Assistant event sink failed"));
    }
}

[[nodiscard]] std::string public_error_diagnostic(const util::Error& error) {
    std::string diagnostic = error.message;
    if (!error.detail.empty() && diagnostic.find(error.detail) == std::string::npos) {
        if (!diagnostic.empty()) {
            diagnostic += ": ";
        }
        diagnostic += error.detail;
    }
    return util::bounded_redacted_text(
        std::move(diagnostic), kMaxPublicErrorBytes, "...");
}

[[nodiscard]] AssistantMessage safe_terminal_message(
    AssistantMessage message,
    const std::optional<util::Error>& failure = std::nullopt) {
    if (message.error_message) {
        message.error_message = util::bounded_redacted_text(
            std::move(*message.error_message), kMaxPublicErrorBytes, "...");
    } else if (failure) {
        message.error_message = public_error_diagnostic(*failure);
    }
    return message;
}

/// Frozen pi treats each Provider catalog as best-effort: a throwing callback
/// contributes no models so other Providers remain discoverable.
[[nodiscard]] std::vector<Model> safe_provider_models(const Provider& provider_value) {
    try {
        return provider_value.models();
    } catch (...) {
        return {};
    }
}

void merge_headers(ProviderHeaders& target, const ModelHeaders& overrides) {
    for (const auto& [name, value] : overrides) {
        std::erase_if(target, [&name](const auto& entry) {
            return std::ranges::equal(
                entry.first,
                name,
                [](char left, char right) {
                    const auto lower = [](char character) {
                        if (character >= 'A' && character <= 'Z') {
                            return static_cast<char>(character - 'A' + 'a');
                        }
                        return character;
                    };
                    return lower(left) == lower(right);
                });
        });
        target.emplace(name, value);
    }
}

[[nodiscard]] bool is_models_domain_error(util::ErrorCode code) {
    return code == util::ErrorCode::ModelSource ||
           code == util::ErrorCode::ModelValidation ||
           code == util::ErrorCode::Provider ||
           code == util::ErrorCode::Stream ||
           code == util::ErrorCode::Auth ||
           code == util::ErrorCode::OAuth;
}

[[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> terminal_failure(
    const Model& model,
    util::Error failure,
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

[[nodiscard]] boost::asio::awaitable<util::Expected<AssistantMessage>> terminal_message_value(
    AssistantMessage message,
    util::Error failure,
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

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> resolve_api_key(
        const Provider& provider,
        ApiKeyAuth& auth,
        std::optional<ApiKeyCredential> credential) {
        auto resolved = co_await invoke_models_callback(
            util::ErrorCode::Auth,
            "API key auth callback failed for provider " + std::string{provider.id()},
            [&]() {
                return auth.resolve(*auth_context, std::move(credential));
            });
        if (!resolved) {
            co_return std::unexpected(categorized_error(
                util::ErrorCode::Auth,
                "API key auth failed for provider " + std::string{provider.id()},
                resolved.error()));
        }
        co_return std::move(*resolved);
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> resolve_oauth(
        const Provider& provider,
        OAuthAuth& auth,
        OAuthCredential credential) {
        const std::string provider_id{provider.id()};
        if (expires_soon(credential)) {
            auto modified = co_await invoke_models_callback(
                util::ErrorCode::Auth,
                "Credential store modify callback failed for " + provider_id,
                [&]() {
                    return credentials->modify(
                        provider_id,
                        [&auth, provider_id](std::optional<Credential> current)
                            -> boost::asio::awaitable<util::Expected<std::optional<Credential>>> {
                            auto* current_oauth = current
                                ? std::get_if<OAuthCredential>(&*current)
                                : nullptr;
                            if (current_oauth == nullptr || !expires_soon(*current_oauth)) {
                                co_return std::optional<Credential>{};
                            }
                            auto refreshed = co_await invoke_models_callback(
                                util::ErrorCode::OAuth,
                                "OAuth refresh callback failed for " + provider_id,
                                [&]() {
                                    return auth.refresh(*current_oauth);
                                });
                            if (!refreshed) {
                                co_return std::unexpected(categorized_error(
                                    util::ErrorCode::OAuth,
                                    "OAuth refresh failed for " + provider_id,
                                    refreshed.error()));
                            }
                            co_return std::optional<Credential>{Credential{std::move(*refreshed)}};
                        });
                });
            if (!modified) {
                if (modified.error().code == util::ErrorCode::OAuth) {
                    co_return std::unexpected(modified.error());
                }
                co_return std::unexpected(categorized_error(
                    util::ErrorCode::Auth,
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

        auto request_auth = co_await invoke_models_callback(
            util::ErrorCode::OAuth,
            "OAuth auth derivation callback failed for " + provider_id,
            [&]() {
                return auth.to_auth(credential);
            });
        if (!request_auth) {
            co_return std::unexpected(categorized_error(
                util::ErrorCode::OAuth,
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

util::ExpectedVoid Models::set_provider(std::shared_ptr<Provider> provider_value) {
    if (!provider_value || provider_value->id().empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "provider id is required"));
    }
    const auto& auth = provider_value->auth();
    if (!auth.api_key && !auth.oauth) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Auth,
            "provider must define authentication",
            std::string{provider_value->id()}));
    }
    impl_->providers.insert_or_assign(
        std::string{provider_value->id()}, std::move(provider_value));
    return {};
}

void Models::delete_provider(std::string_view provider_id) {
    if (const auto found = impl_->providers.find(provider_id);
        found != impl_->providers.end()) {
        impl_->providers.erase(found);
    }
}

void Models::clear_providers() {
    impl_->providers.clear();
}

std::vector<std::shared_ptr<Provider>> Models::providers() const {
    std::vector<std::shared_ptr<Provider>> result;
    result.reserve(impl_->providers.size());
    for (const auto& [_, provider_value] : impl_->providers) {
        result.push_back(provider_value);
    }
    return result;
}

std::shared_ptr<Provider> Models::provider(std::string_view provider_id) const {
    const auto found = impl_->providers.find(provider_id);
    return found == impl_->providers.end() ? nullptr : found->second;
}

std::vector<Model> Models::models(std::optional<std::string_view> provider_id) const {
    if (provider_id) {
        const auto selected = provider(*provider_id);
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

boost::asio::awaitable<util::Expected<std::optional<AuthCheck>>> Models::check_auth(
    std::string provider_id) {
    const auto selected = provider(provider_id);
    if (!selected) {
        co_return std::optional<AuthCheck>{};
    }

    auto stored = co_await invoke_models_callback(
        util::ErrorCode::Auth,
        "Credential store read callback failed for " + provider_id,
        [&]() {
            return impl_->credentials->read(provider_id);
        });
    if (!stored) {
        co_return std::unexpected(categorized_error(
            util::ErrorCode::Auth,
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
            auto checked = co_await invoke_models_callback(
                util::ErrorCode::Auth,
                "API key auth check callback failed for provider " + provider_id,
                [&]() {
                    return auth.api_key->check(*impl_->auth_context, credential);
                });
            if (!checked) {
                co_return std::unexpected(categorized_error(
                    util::ErrorCode::Auth,
                    "API key auth check failed for provider " + provider_id,
                    checked.error()));
            }
            co_return std::move(*checked);
        }
        CCH_TRY(resolved, co_await impl_->resolve_api_key(
            *selected, *auth.api_key, credential));
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
        auto checked = co_await invoke_models_callback(
            util::ErrorCode::Auth,
            "API key auth check callback failed for provider " + provider_id,
            [&]() {
                return auth.api_key->check(*impl_->auth_context, std::nullopt);
            });
        if (!checked) {
            co_return std::unexpected(categorized_error(
                util::ErrorCode::Auth,
                "API key auth check failed for provider " + provider_id,
                checked.error()));
        }
        co_return std::move(*checked);
    }
    CCH_TRY(resolved, co_await impl_->resolve_api_key(
        *selected, *auth.api_key, std::nullopt));
    if (!resolved) {
        co_return std::optional<AuthCheck>{};
    }
    co_return AuthCheck{
        .source = resolved->source,
        .type = AuthType::ApiKey,
    };
}

boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> Models::get_auth(
    std::string provider_id,
    std::optional<std::string> explicit_api_key) {
    const auto selected = provider(provider_id);
    if (!selected) {
        co_return std::optional<AuthResult>{};
    }
    auto& auth = selected->auth();

    if (explicit_api_key && auth.api_key) {
        ApiKeyCredential credential;
        credential.key = std::move(explicit_api_key);
        CCH_TRY(resolved, co_await impl_->resolve_api_key(
            *selected, *auth.api_key, std::move(credential)));
        co_return resolved;
    }

    auto stored = co_await invoke_models_callback(
        util::ErrorCode::Auth,
        "Credential store read callback failed for " + provider_id,
        [&]() {
            return impl_->credentials->read(provider_id);
        });
    if (!stored) {
        co_return std::unexpected(categorized_error(
            util::ErrorCode::Auth,
            "Credential store read failed for " + provider_id,
            stored.error()));
    }
    if (*stored) {
        if (auto* oauth = std::get_if<OAuthCredential>(&**stored)) {
            if (!auth.oauth) {
                co_return std::optional<AuthResult>{};
            }
            CCH_TRY(resolved, co_await impl_->resolve_oauth(
                *selected, *auth.oauth, *oauth));
            co_return resolved;
        }
        if (!auth.api_key) {
            co_return std::optional<AuthResult>{};
        }
        CCH_TRY(resolved, co_await impl_->resolve_api_key(
            *selected,
            *auth.api_key,
            std::get<ApiKeyCredential>(**stored)));
        co_return resolved;
    }

    if (!auth.api_key) {
        co_return std::optional<AuthResult>{};
    }
    CCH_TRY(resolved, co_await impl_->resolve_api_key(
        *selected, *auth.api_key, std::nullopt));
    co_return resolved;
}

boost::asio::awaitable<util::Expected<std::optional<AuthResult>>> Models::get_auth(
    Model model_value,
    std::optional<std::string> explicit_api_key) {
    CCH_TRY(resolved, co_await get_auth(
        model_value.provider, std::move(explicit_api_key)));
    if (!resolved || !model_value.headers) {
        co_return resolved;
    }
    merge_headers(resolved->auth.headers, *model_value.headers);
    co_return resolved;
}

boost::asio::awaitable<util::ExpectedVoid> Models::logout(
    std::string provider_id) {
    auto removed = co_await invoke_models_callback(
        util::ErrorCode::Auth,
        "Credential store delete callback failed for " + provider_id,
        [&]() {
            return impl_->credentials->remove(provider_id);
        });
    if (!removed) {
        co_return std::unexpected(categorized_error(
            util::ErrorCode::Auth,
            "Credential store delete failed for " + provider_id,
            removed.error()));
    }
    co_return util::ExpectedVoid{};
}

boost::asio::awaitable<util::Expected<AssistantMessage>> Models::stream(
    Model model_value,
    AiContext context,
    ModelsStreamOptions options,
    AssistantEventSink sink) {
    const auto selected = provider(model_value.provider);
    if (!selected) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            util::make_error(
                util::ErrorCode::Provider,
                "Unknown provider: " + model_value.provider),
            sink));
        co_return terminal;
    }

    auto auth = co_await invoke_models_callback(
        util::ErrorCode::Auth,
        "Authentication resolution failed for provider " + model_value.provider,
        [&]() {
            return get_auth(model_value, std::move(options.api_key));
        });
    if (!auth) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value, auth.error(), sink));
        co_return terminal;
    }
    if (!*auth) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            util::make_error(
                util::ErrorCode::Auth,
                "Provider is not configured: " + model_value.provider),
            sink));
        co_return terminal;
    }

    if (auto valid = validate_model(model_value); !valid) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            categorized_error(
                util::ErrorCode::ModelValidation,
                valid.error().message,
                valid.error()),
            sink));
        co_return terminal;
    }

    auto request_model = model_value;
    if ((**auth).auth.base_url) {
        request_model.base_url = *(**auth).auth.base_url;
    }

    std::optional<AssistantMessage> terminal_message;
    std::optional<util::Error> sink_failure;
    AssistantEventSink forwarding_sink =
        [&sink, &terminal_message, &sink_failure](
            const AssistantStreamEvent& event) -> util::ExpectedVoid {
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
    auto result = co_await invoke_models_callback(
        util::ErrorCode::Stream,
        "Provider stream callback failed for " + model_value.provider,
        [&]() {
            return selected->stream(
                request_model,
                context,
                ProviderStreamOptions{
                    .auth = (**auth).auth,
                    .env = (**auth).env,
                    .stop_token = options.stop_token,
                },
                std::move(forwarding_sink));
        });

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
                ? util::ErrorCode::Cancelled
                : util::ErrorCode::Stream;
            const auto diagnostic = result->error_message.value_or(
                reason == AssistantStopReason::Aborted
                    ? "Request was aborted"
                    : "Provider stream failed");
            auto message = std::move(*result);
            CCH_TRY(terminal, co_await terminal_message_value(
                std::move(message),
                util::make_error(code, diagnostic),
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
        (result.error().code == util::ErrorCode::Cancelled ||
         is_models_domain_error(result.error().code))) {
        co_return std::move(*terminal_message);
    }
    if (result.error().code == util::ErrorCode::Cancelled) {
        CCH_TRY(terminal, co_await terminal_failure(
            model_value,
            util::make_error(util::ErrorCode::Cancelled, "Request was aborted"),
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

boost::asio::awaitable<util::Expected<AssistantMessage>> Models::stream(
    const StreamChatRequest& request,
    AssistantEventSink sink) {
    CCH_TRY(message, co_await stream(
        request.model,
        request.context,
        ModelsStreamOptions{.stop_token = request.stop_token},
        std::move(sink)));
    co_return message;
}

} // namespace cch::ai
