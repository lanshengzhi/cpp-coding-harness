#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/util/Error.hpp>
#include "util/ExpectedMacros.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// One recorded `streamSimple` call at the Agent seam.
struct RecordedStreamSimpleCall {
    ai::Model model;
    ai::AiContext context;
    ai::SimpleStreamOptions options;
};

/// Scripted, recording fake ModelRuntime — the primary Agent test seam (ADR
/// 0034 / #349 Testing Decisions: "the fake runtime records every streamSimple
/// call and scripts terminal/success outcomes through the surface pi-ai
/// shipped"). Records every `stream_simple` call and returns queued responses;
/// a configured `failure` completes through the single `util::Expected` error
/// alternative (infrastructure failure, never a domain terminal). Only the
/// virtual stream surface is callable; the base runtime holds no impl.
class FakeModelRuntime final : public coding_agent::ModelRuntime {
public:
    /// Session-seam overrides (the §7.2 recorded exception): the impl-less
    /// fake serves model resolution and auth preflight so a session can run a
    /// scripted turn end to end. The served model uses the scripted-fake API
    /// identity like the Models-seam fake provider.
    [[nodiscard]] std::optional<ai::Model> model(
        std::string_view provider_id,
        std::string_view model_id) const override {
        ai::Model model;
        model.id = std::string{model_id};
        model.name = model.id;
        model.api = "scripted-fake";
        model.provider = std::string{provider_id};
        model.reasoning = false;
        model.input = {ai::ModelInput::Text};
        model.context_window = 128000;
        model.max_tokens = 16384;
        return model;
    }

    boost::asio::awaitable<util::Expected<std::vector<ai::Model>>> get_available(
        std::optional<std::string_view> provider_id = std::nullopt) override {
        std::vector<ai::Model> models;
        if (auto resolved = model(
                provider_id.value_or("fake"), "fake-model")) {
            models.push_back(std::move(*resolved));
        }
        co_return models;
    }

    boost::asio::awaitable<util::Expected<std::optional<ai::AuthCheck>>> check_auth(
        std::string provider_id) override {
        (void)provider_id;
        co_return ai::AuthCheck{
            .source = "fake",
            .type = ai::AuthType::ApiKey,
        };
    }

    bool has_configured_auth(std::string_view provider_id) const override {
        (void)provider_id;
        return true;
    }

    std::vector<std::string> configured_api_key_env_names() const override {
        return {};
    }

    boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream_simple(
        ai::Model model,
        ai::AiContext context,
        ai::SimpleStreamOptions options,
        ai::AssistantEventSink sink) override {
        calls.push_back(RecordedStreamSimpleCall{
            std::move(model),
            std::move(context),
            std::move(options),
        });
        if (failure) {
            co_return std::unexpected(*failure);
        }

        // Accepted-call cancellation completes through exactly one aborted
        // terminal event plus the agreeing final AssistantMessage (ADR 0020).
        if (calls.back().options.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message("");
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            ++terminal_events;
            last_terminal_failure = util::make_error(
                util::ErrorCode::Cancelled, *terminal.error_message);
            if (sink) {
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = terminal.stop_reason,
                    .error = terminal,
                    .failure = last_terminal_failure,
                }));
            }
            co_return terminal;
        }

        if (responses.empty()) {
            co_return ai::assistant_text_message("default fake response");
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        if (sink) {
            if (response.stop_reason == ai::AssistantStopReason::Error ||
                response.stop_reason == ai::AssistantStopReason::Aborted) {
                // Exactly one terminal event plus the agreeing final message
                // (the #326 terminal-error-event contract); no start event is
                // emitted so the loop's synthesize-start path stays exercised.
                ++terminal_events;
                std::optional<util::Error> terminal_failure = std::nullopt;
                if (response.error_message) {
                    const auto code =
                        response.stop_reason == ai::AssistantStopReason::Aborted
                            ? util::ErrorCode::Cancelled
                            : terminal_failure_code.value_or(
                                  util::ErrorCode::Stream);
                    terminal_failure = util::make_error(
                        code, *response.error_message);
                }
                last_terminal_failure = terminal_failure;
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = response.stop_reason,
                    .error = response,
                    .failure = std::move(terminal_failure),
                }));
                co_return response;
            }
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        }
        for (std::size_t index = 0; index < response.content.size(); ++index) {
            const auto& block = response.content[index];
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                CCH_TRY_VOID(sink(ai::TextDeltaEvent{index, text->text, response}));
            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ThinkingDeltaEvent{index, thinking->thinking, response}));
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                CCH_TRY_VOID(sink(ai::ToolCallStartEvent{index, response}));
                CCH_TRY_VOID(sink(ai::ToolCallDeltaEvent{index, call->raw_arguments, response}));
                CCH_TRY_VOID(sink(ai::ToolCallEndEvent{index, *call, response}));
            }
        }
        co_return response;
    }

    /// Recorded per-turn calls in issue order.
    std::vector<RecordedStreamSimpleCall> calls;
    /// Count of terminal (error/aborted) events delivered to the sink.
    int terminal_events{0};
    /// Scripted terminal failure category for the #326 six-category channel.
    /// When set, scripted error-terminal responses carry a failure of this
    /// category instead of the derived Stream code; aborted terminals keep
    /// Cancelled.
    std::optional<util::ErrorCode> terminal_failure_code{std::nullopt};
    /// The most recent terminal failure emitted on the sink, recorded so tests
    /// can assert the six-category channel flowing through the single
    /// `util::Expected` error value.
    std::optional<util::Error> last_terminal_failure{std::nullopt};
    /// Scripted assistant responses consumed in FIFO order.
    std::deque<ai::AssistantMessage> responses;
    /// Scripted infrastructure failure returned through the Expected channel.
    std::optional<util::Error> failure{std::nullopt};
};

} // namespace cch::tests
