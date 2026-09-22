#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/Headers.hpp"
#include "ai/ModelStreamBridge.hpp"
#include "ai/Timestamps.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "ai/providers/BoostBeastWebSocketTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/ProviderTestAccess.hpp"
#include "ai/providers/RetryPolicy.hpp"
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
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ModelsImpl.hpp"

namespace cch::ai {
namespace {

[[nodiscard]] support::ExpectedVoid emit_assistant_event(AssistantEventSink& sink, const AssistantStreamEvent& event) {
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

[[nodiscard]] InferenceFailure inference_failure_for(
        const support::Error& failure, AssistantStopReason reason = AssistantStopReason::Error) {
    return InferenceFailure{
            .kind = reason == AssistantStopReason::Aborted
                            ? InferenceFailureKind::Cancelled
                            : providers::inference_failure_kind_from_transport(failure.code),
            .output_started = false,
            .suggested_backoff_ms = std::nullopt,
            .provider_code = std::nullopt,
    };
}

[[nodiscard]] AssistantMessage safe_terminal_message(
        AssistantMessage message, const std::optional<support::Error>& failure = std::nullopt) {
    if (message.error_message) {
        message.error_message =
                support::bounded_redacted_text(std::move(*message.error_message), kMaxPublicErrorBytes, "...");
    } else if (failure) {
        message.error_message = public_error_diagnostic(*failure);
    }
    return message;
}

/// Frozen pi treats each Provider catalog as best-effort: a throwing callback

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

[[nodiscard]] std::vector<std::string> deleted_headers(const RequestHeaders& headers) {
    std::vector<std::string> result;
    for (const auto& [name, value] : headers) {
        if (!value) {
            result.push_back(name);
        }
    }
    return result;
}

[[nodiscard]] support::ExpectedVoid assert_request_auth(const Model& model, const ModelAuth& auth) {
    const bool scoped_api = model.api == "openai-codex-responses" ||
                            model.api == "openai-responses" ||
                            model.api == "openai-completions" ||
                            model.api == "anthropic-messages";
    if (!scoped_api || (auth.api_key && !auth.api_key->empty()) ||
            has_non_empty_header(auth.headers, "authorization") || has_non_empty_header(auth.headers, "x-api-key") ||
            has_non_empty_header(auth.headers, "cf-aig-authorization")) {
        return {};
    }
    return std::unexpected(support::make_error(support::ErrorCode::Auth, "No API key for provider: " + model.provider));
}

[[nodiscard]] support::Expected<RequestHeaders> transform_request_headers(
        RequestHeaders headers, TransformHeadersHook& transform) {
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

/// Per-API session headers for cache reuse: Codex clamps the prompt cache
/// key and aliases it as session-id; openai-responses passes it through.
void apply_session_headers(RequestHeaders& headers, std::string_view api, const std::string& session_id) {
    if (api == "openai-codex-responses") {
        const auto codex_session_id = detail::clamp_openai_prompt_cache_key(session_id);
        set_header(headers, "session-id", codex_session_id);
        set_header(headers, "x-client-request-id", codex_session_id);
    } else if (api == "openai-responses") {
        set_header(headers, "session_id", session_id);
        set_header(headers, "x-client-request-id", session_id);
    }
}

[[nodiscard]] support::Expected<PreparedProviderRequest> prepare_provider_request(
        Model model, const AiContext& context, AuthResult auth_result, SimpleStreamOptions options) {
    if (auth_result.auth.base_url) {
        model.base_url = *auth_result.auth.base_url;
    }

    auto request_env = std::move(auth_result.env);
    for (const auto& [name, value] : options.env) {
        request_env.insert_or_assign(name, value);
    }
    const auto cache_retention = detail::resolve_cache_retention(options.cache_retention, request_env);
    const auto request_session_id =
            cache_retention == CacheRetention::None ? std::optional<std::string>{} : options.session_id;

    auto request_headers = request_headers_from_auth(auth_result.auth);
    if (request_session_id) {
        apply_session_headers(request_headers, model.api, *request_session_id);
    }
    merge_headers(request_headers, options.headers);
    auto transformed_headers = transform_request_headers(std::move(request_headers), options.transform_headers);
    if (!transformed_headers) {
        return std::unexpected(transformed_headers.error());
    }

    auto request_deleted_headers = deleted_headers(*transformed_headers);
    auth_result.auth.headers = concrete_headers(*transformed_headers);
    if (auto asserted = assert_request_auth(model, auth_result.auth); !asserted) {
        return std::unexpected(asserted.error());
    }

    const auto reasoning = options.reasoning ? std::optional<ModelThinkingLevel>{clamp_thinking_level(
                                                       model, to_model_thinking_level(*options.reasoning))}
                                             : std::nullopt;
    const auto requested_max_tokens = options.max_tokens.value_or(model.max_tokens);
    const auto max_tokens = detail::clamp_max_tokens_to_context(model, context, requested_max_tokens);
    return PreparedProviderRequest{
            .model = std::move(model),
            .options =
                    ProviderStreamOptions{
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
    return code == support::ErrorCode::ModelSource || code == support::ErrorCode::ModelValidation ||
           code == support::ErrorCode::Provider || code == support::ErrorCode::Stream ||
           code == support::ErrorCode::Auth || code == support::ErrorCode::OAuth;
}

/// Sanitizes the failure, derives the public terminal message, and emits
/// exactly one AssistantErrorEvent for it.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> emit_terminal_error(
        AssistantMessage message, support::Error failure, AssistantEventSink& sink) {
    failure = safe_error(std::move(failure));
    message = safe_terminal_message(std::move(message), failure);
    CCH_TRY_VOID(emit_assistant_event(sink,
            AssistantErrorEvent{
                    .reason = message.stop_reason,
                    .error = message,
                    .failure = failure,
                    .inference_failure = inference_failure_for(failure, message.stop_reason),
            }));
    co_return message;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> terminal_failure(const Model& model,
        support::Error failure,
        AssistantEventSink& sink,
        AssistantStopReason reason = AssistantStopReason::Error) {
    AssistantMessage message;
    message.api = model.api;
    message.provider = model.provider;
    message.model = model.id;
    message.stop_reason = reason;
    message.timestamp = current_timestamp_ms();
    co_return co_await emit_terminal_error(std::move(message), std::move(failure), sink);
}

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
        CCH_TRY(terminal,
                co_await terminal_failure(model_value,
                        support::make_error(support::ErrorCode::Provider, "Unknown provider: " + model_value.provider),
                        sink));
        co_return terminal;
    }

    auto auth =
            co_await invoke_async_operation([&]() { return self->get_auth(model_value, std::move(options.api_key)); });
    if (!auth) {
        CCH_TRY(terminal, co_await terminal_failure(model_value, auth.error(), sink));
        co_return terminal;
    }
    if (!*auth) {
        CCH_TRY(terminal,
                co_await terminal_failure(model_value,
                        support::make_error(
                                support::ErrorCode::Auth, "Provider is not configured: " + model_value.provider),
                        sink));
        co_return terminal;
    }

    if (auto valid = validate_model(model_value); !valid) {
        CCH_TRY(terminal,
                co_await terminal_failure(model_value,
                        categorized_error(support::ErrorCode::ModelValidation, valid.error().message, valid.error()),
                        sink));
        co_return terminal;
    }

    auto prepared = prepare_provider_request(model_value, context, std::move(**auth), std::move(options));
    if (!prepared) {
        CCH_TRY(terminal, co_await terminal_failure(model_value, prepared.error(), sink));
        co_return terminal;
    }

    std::optional<AssistantMessage> terminal_message;
    std::optional<support::Error> sink_failure;
    AssistantEventSink forwarding_sink = [&sink, &terminal_message, &sink_failure](
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
                    .inference_failure = error->inference_failure,
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
        auto provider_stream =
                selected->stream(std::move(prepared->model), std::move(context), std::move(prepared->options));
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
        if (reason == AssistantStopReason::Error || reason == AssistantStopReason::Aborted) {
            const auto code =
                    reason == AssistantStopReason::Aborted ? support::ErrorCode::Cancelled : support::ErrorCode::Stream;
            const auto diagnostic = result->error_message.value_or(
                    reason == AssistantStopReason::Aborted ? "Request was aborted" : "Provider stream failed");
            auto message = std::move(*result);
            CCH_TRY(terminal,
                    co_await emit_terminal_error(std::move(message), support::make_error(code, diagnostic), sink));
            co_return terminal;
        }
        CCH_TRY_VOID(emit_assistant_event(sink,
                AssistantDoneEvent{
                        .reason = reason,
                        .message = *result,
                }));
        co_return std::move(*result);
    }

    if (terminal_message &&
            (result.error().code == support::ErrorCode::Cancelled || is_models_domain_error(result.error().code))) {
        co_return std::move(*terminal_message);
    }
    if (result.error().code == support::ErrorCode::Cancelled) {
        CCH_TRY(terminal,
                co_await terminal_failure(model_value,
                        support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
                        sink,
                        AssistantStopReason::Aborted));
        co_return terminal;
    }
    if (is_models_domain_error(result.error().code)) {
        CCH_TRY(terminal, co_await terminal_failure(model_value, result.error(), sink));
        co_return terminal;
    }
    CCH_TRY(message, std::move(result));
    co_return message;
}

} // namespace

ModelStream Models::stream(Model model, AiContext context, SimpleStreamOptions options) {
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
