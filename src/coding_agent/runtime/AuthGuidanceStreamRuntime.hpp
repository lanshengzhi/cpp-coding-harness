#pragma once

#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::runtime {

/// Whether a provider authenticates through an OAuth credential (pi
/// `ModelRuntime.isUsingOAuth`). Injected so the request-time branch is
/// decidable through the fake-`ModelRuntime` seam; the production default
/// queries the wrapped runtime.
using OAuthProviderPredicate =
    std::move_only_function<bool(std::string_view)>;

/// Session-layer stream decorator for the request-time re-auth guidance (pi
/// `agent-session.ts` `_getRequiredRequestAuth`). Wraps the session's
/// `ModelRuntime` so every `streamSimple` request maps a pi-ai auth-category
/// (`auth`/`oauth`) terminal failure to pi's two verbatim re-auth guidance
/// branches: no usable key → `formatNoApiKeyFoundMessage`; OAuth credential
/// missing/expired → "Credentials may have expired … Run '/login X'". The
/// terminal-error-event contract is preserved: exactly one `error` terminal
/// event plus an agreeing final `AssistantMessage`, with the category flowing
/// through the single `util::Expected` channel (#326) — no second exception
/// hierarchy. The wrapped runtime stays the canonical seam for every other
/// call (the pi-ai `ModelRuntime`/`getAuth` surface is consumed unchanged);
/// only the stream surface is decorated.
class AuthGuidanceStreamRuntime final : public coding_agent::ModelRuntime {
public:
    explicit AuthGuidanceStreamRuntime(
        std::shared_ptr<coding_agent::ModelRuntime> wrapped,
        OAuthProviderPredicate is_using_oauth = {},
        std::filesystem::path docs_path =
            std::filesystem::path{kDefaultAuthGuidanceDocsPath})
        : wrapped_(std::move(wrapped)),
          is_using_oauth_(std::move(is_using_oauth)),
          docs_path_(std::move(docs_path)) {
        if (!is_using_oauth_) {
            is_using_oauth_ = [wrapped = wrapped_](
                                  std::string_view provider_id) {
                return wrapped != nullptr &&
                       wrapped->is_using_oauth(provider_id);
            };
        }
    }

    AuthGuidanceStreamRuntime(const AuthGuidanceStreamRuntime&) = delete;
    AuthGuidanceStreamRuntime& operator=(const AuthGuidanceStreamRuntime&) =
        delete;

    /// Stream one assistant response, rewriting an auth/oauth-category
    /// terminal failure to the verbatim guidance (category preserved).
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>>
    stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        // The provider identity must survive the move into the wrapped call:
        // the forwarding sink below may fire while the model argument is
        // already moved-from.
        const std::string provider{model.provider};
        std::optional<util::Error> rewritten_terminal;
        ai::AssistantEventSink forwarding_sink =
            [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            if (const auto* error =
                    std::get_if<ai::AssistantErrorEvent>(&event)) {
                if (auto rewrite = guidance_terminal(provider, *error)) {
                    rewritten_terminal = std::move(*rewrite);
                    ai::AssistantErrorEvent rewritten = *error;
                    rewritten.error = rewrite_message(
                        std::move(rewritten.error), *rewritten_terminal);
                    rewritten.failure = *rewritten_terminal;
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

        auto result = co_await wrapped_->stream_simple(
            std::move(model),
            std::move(context),
            std::move(options),
            std::move(forwarding_sink));
        if (!result) {
            co_return std::unexpected(std::move(result.error()));
        }
        if (rewritten_terminal) {
            // The returned terminal message must agree with the forwarded
            // terminal event (the #326 contract).
            co_return rewrite_message(
                std::move(*result), *rewritten_terminal);
        }
        co_return std::move(*result);
    }

    /// Non-simple streaming is delegated unchanged (no caller in the session
    /// layer uses it; kept so the decorator remains a complete runtime).
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>>
    stream(const ai::StreamChatRequest& request,
           ai::AssistantEventSink sink) override {
        co_return co_await wrapped_->stream(request, std::move(sink));
    }

private:
    /// pi `_getRequiredRequestAuth` branch selection from a terminal failure:
    /// an `oauth`-category failure (refresh/derivation — dead credentials)
    /// and an `auth`-category failure on an OAuth provider both produce the
    /// re-auth message; every other `auth`-category failure produces the
    /// no-key message. Non-auth failures pass through unmapped (pi rethrows
    /// non-no-key `getAuth` errors rather than rewriting them).
    [[nodiscard]] std::optional<util::Error> guidance_terminal(
        std::string_view provider,
        const ai::AssistantErrorEvent& error) {
        if (!error.failure) {
            return std::nullopt;
        }
        if (error.failure->code == util::ErrorCode::OAuth) {
            return util::make_error(
                util::ErrorCode::OAuth,
                format_oauth_reauthenticate_message(provider));
        }
        if (error.failure->code == util::ErrorCode::Auth) {
            if (is_using_oauth_(provider)) {
                return util::make_error(
                    util::ErrorCode::Auth,
                    format_oauth_reauthenticate_message(provider));
            }
            return util::make_error(
                util::ErrorCode::Auth,
                format_no_api_key_found_message(provider, docs_path_));
        }
        return std::nullopt;
    }

    [[nodiscard]] ai::AssistantMessage rewrite_message(
        ai::AssistantMessage message,
        const util::Error& guidance) {
        message.error_message = guidance.message;
        return message;
    }

    std::shared_ptr<coding_agent::ModelRuntime> wrapped_;
    OAuthProviderPredicate is_using_oauth_;
    std::filesystem::path docs_path_;
};

} // namespace cch::coding_agent::runtime
