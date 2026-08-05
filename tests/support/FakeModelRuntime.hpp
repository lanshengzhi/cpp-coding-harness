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
            if (sink) {
                CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                    .reason = terminal.stop_reason,
                    .error = terminal,
                    .failure = util::make_error(
                        util::ErrorCode::Cancelled, *terminal.error_message),
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
                    terminal_failure = util::make_error(
                        response.stop_reason == ai::AssistantStopReason::Aborted
                            ? util::ErrorCode::Cancelled
                            : util::ErrorCode::Stream,
                        *response.error_message);
                }
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
    /// Scripted assistant responses consumed in FIFO order.
    std::deque<ai::AssistantMessage> responses;
    /// Scripted infrastructure failure returned through the Expected channel.
    std::optional<util::Error> failure{std::nullopt};
};

} // namespace cch::tests
