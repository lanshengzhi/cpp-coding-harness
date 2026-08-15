#include "AuthGuidanceStream.hpp"

#include <cch/ai/StreamEvent.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace cch::coding_agent::runtime {
namespace {

/// pi `_getRequiredRequestAuth` branch selection from a terminal failure:
/// an `oauth`-category failure (refresh/derivation — dead credentials) and an
/// `auth`-category failure on an OAuth provider both produce the re-auth
/// message; every other `auth`-category failure produces the no-key message.
/// Non-auth failures pass through unmapped (pi rethrows non-no-key `getAuth`
/// errors rather than rewriting them).
[[nodiscard]] std::optional<support::Error> guidance_terminal(
    std::string_view provider,
    const ai::AssistantErrorEvent& error,
    OAuthProviderPredicate& is_using_oauth,
    const std::filesystem::path& docs_path) {
    if (!error.failure) {
        return std::nullopt;
    }
    if (error.failure->code == support::ErrorCode::OAuth) {
        return support::make_error(
            support::ErrorCode::OAuth,
            format_oauth_reauthenticate_message(provider));
    }
    if (error.failure->code == support::ErrorCode::Auth) {
        if (is_using_oauth(provider)) {
            return support::make_error(
                support::ErrorCode::Auth,
                format_oauth_reauthenticate_message(provider));
        }
        return support::make_error(
            support::ErrorCode::Auth,
            format_no_api_key_found_message(provider, docs_path));
    }
    return std::nullopt;
}

[[nodiscard]] ai::AssistantMessage rewrite_message(
    ai::AssistantMessage message,
    const support::Error& guidance) {
    message.error_message = guidance.message;
    return message;
}

} // namespace

ai::ModelStream apply_auth_guidance(
    ai::ModelStream inner,
    std::string provider,
    OAuthProviderPredicate is_using_oauth,
    std::filesystem::path docs_path) {
    return ai::ModelStream{ai::ModelStreamProducer{
        [inner = std::move(inner), provider = std::move(provider),
         is_using_oauth = std::move(is_using_oauth),
         docs_path = std::move(docs_path)](
            ai::AssistantEventSink sink,
            ai::ModelStreamCompletion completion) mutable noexcept {
            auto rewrote = std::make_shared<std::optional<support::Error>>();
            auto predicate = std::make_shared<OAuthProviderPredicate>(
                std::move(is_using_oauth));

            ai::AssistantEventSink wrapped_sink =
                [sink = std::move(sink), rewrote, provider, predicate, docs_path](
                    const ai::AssistantStreamEvent& event) mutable -> support::ExpectedVoid {
                    if (const auto* error =
                            std::get_if<ai::AssistantErrorEvent>(&event)) {
                        if (auto guidance = guidance_terminal(
                                provider, *error, *predicate, docs_path)) {
                            *rewrote = *guidance;
                            ai::AssistantErrorEvent rewritten = *error;
                            rewritten.error = rewrite_message(
                                std::move(rewritten.error), *guidance);
                            rewritten.failure = *guidance;
                            if (sink) {
                                return sink(ai::AssistantStreamEvent{
                                    std::move(rewritten)});
                            }
                            return {};
                        }
                    }
                    if (sink) {
                        return sink(event);
                    }
                    return {};
                };

            ai::ModelStreamCompletion wrapped_completion =
                [completion = std::move(completion), rewrote](
                    std::expected<ai::AssistantMessage, support::Error> result) mutable noexcept {
                    if (rewrote->has_value() && result.has_value()) {
                        // The returned terminal message must agree with the
                        // forwarded terminal event (the #326 contract).
                        result->error_message = rewrote->value().message;
                    }
                    completion(std::move(result));
                };

            std::move(inner).start(
                std::move(wrapped_sink), std::move(wrapped_completion));
        }}};
}

} // namespace cch::coding_agent::runtime
