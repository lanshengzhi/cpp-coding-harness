#pragma once

#include <cch/ai/Content.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/RequestOptions.hpp>
#include <cch/util/Error.hpp>
#include "ai/AsyncResultBridge.hpp"
#include "ai/ModelStreamBridge.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>

#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// One recorded model-stream request at the Agent seam (the post-`stream`
/// shape: the concrete Model argument, the conversation context, and the
/// per-turn options).
struct RecordedStreamSimpleCall {
    ai::Model model;
    ai::AiContext context;
    ai::SimpleStreamOptions options;
};

/// Scripted, recording narrow fake for the Agent's AI-owned `ModelStream`
/// seam (ADR 0040 / #453). Records every stream request and returns queued
/// responses through the move-only `ModelStream` value; a configured
/// `failure` completes through the single `util::Expected` error alternative
/// (infrastructure failure, never a domain terminal). Not a
/// `coding_agent::ModelRuntime` subclass: it produces passive `ModelStream`
/// values instead of standing in for a virtual Models Runtime surface.
class FakeModelStream final : public std::enable_shared_from_this<FakeModelStream> {
public:
    FakeModelStream() = default;
    ~FakeModelStream() = default;
    FakeModelStream(const FakeModelStream&) = delete;
    FakeModelStream& operator=(const FakeModelStream&) = delete;

    /// The move-only factory the Agent consumes.
    [[nodiscard]] ai::ModelStreamFactory factory() {
        auto self = shared_from_this();
        return ai::ModelStreamFactory{
            [self](ai::Model model, ai::AiContext context, ai::SimpleStreamOptions options)
                -> ai::ModelStream {
                self->calls.push_back(RecordedStreamSimpleCall{
                    std::move(model), std::move(context), std::move(options)});
                return ai::ModelStream{ai::ModelStreamProducer{
                    [self](
                        ai::AssistantEventSink sink,
                        ai::ModelStreamCompletion completion) mutable noexcept {
                        try {
                            boost::asio::post(
                                cch::ai::detail::t_initiating_executor,
                                [self,
                                 sink = std::move(sink),
                                 completion = std::move(completion)]() mutable {
                                    self->script(std::move(sink), std::move(completion));
                                });
                        } catch (...) {
                            completion(std::unexpected(util::make_error(
                                util::ErrorCode::Stream,
                                "scripted stream initiation failed")));
                        }
                    }}};
            }};
    }

    /// Script one accepted request (runs on the consuming executor).
    void script(ai::AssistantEventSink sink, ai::ModelStreamCompletion completion) {
        // The sink is a move-only weak-backpressured channel: a failing sink
        // completes the stream with its error exactly once. `emit` returns
        // false after completing, so the caller stops scripting.
        auto emit = [&](const ai::AssistantStreamEvent& event) -> bool {
            if (!sink) {
                return true;
            }
            auto emitted = sink(event);
            if (!emitted) {
                completion(std::unexpected(emitted.error()));
                return false;
            }
            return true;
        };
        const auto& call = calls.back();
        if (failure) {
            completion(std::unexpected(*failure));
            return;
        }

        // Accepted-call cancellation completes through exactly one aborted
        // terminal event plus the agreeing final AssistantMessage (ADR 0020).
        if (call.options.stop_token.stop_requested()) {
            auto terminal = ai::assistant_text_message("");
            terminal.stop_reason = ai::AssistantStopReason::Aborted;
            terminal.error_message = "Request was aborted";
            ++terminal_events;
            last_terminal_failure = util::make_error(
                util::ErrorCode::Cancelled, *terminal.error_message);
            if (!emit(ai::AssistantErrorEvent{
                    .reason = terminal.stop_reason,
                    .error = terminal,
                    .failure = last_terminal_failure,
                })) {
                return;
            }
            completion(terminal);
            return;
        }

        if (responses.empty()) {
            completion(ai::assistant_text_message("default fake response"));
            return;
        }
        auto response = std::move(responses.front());
        responses.pop_front();
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
                        : terminal_failure_code.value_or(util::ErrorCode::Stream);
                terminal_failure = util::make_error(code, *response.error_message);
            }
            last_terminal_failure = terminal_failure;
            if (!emit(ai::AssistantErrorEvent{
                    .reason = response.stop_reason,
                    .error = response,
                    .failure = std::move(terminal_failure),
                })) {
                return;
            }
            completion(response);
            return;
        }
        if (!emit(ai::AssistantStartEvent{response})) {
            return;
        }
        for (std::size_t index = 0; index < response.content.size(); ++index) {
            const auto& block = response.content[index];
            if (const auto* text = std::get_if<ai::TextContent>(&block)) {
                if (!emit(ai::TextDeltaEvent{index, text->text, response})) {
                    return;
                }
            } else if (const auto* thinking = std::get_if<ai::ThinkingContent>(&block)) {
                if (!emit(ai::ThinkingDeltaEvent{index, thinking->thinking, response})) {
                    return;
                }
            } else if (const auto* call = std::get_if<ai::ToolCallContent>(&block)) {
                if (!emit(ai::ToolCallStartEvent{index, response})) {
                    return;
                }
                if (!emit(ai::ToolCallDeltaEvent{index, call->raw_arguments, response})) {
                    return;
                }
                if (!emit(ai::ToolCallEndEvent{index, *call, response})) {
                    return;
                }
            }
        }
        completion(response);
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

/// Adapt a frozen `streamSimple`-shaped coroutine to the Agent's move-only
/// `ModelStreamFactory` seam (ADR 0040 / #453). Narrow fakes keep their
/// bespoke scripting; the adapter captures the consuming executor and
/// co_spawns the coroutine, forwarding the terminal outcome through the
/// `ModelStream` value.
[[nodiscard]] inline ai::ModelStreamFactory adapt_stream_simple(
    std::move_only_function<
        boost::asio::awaitable<util::Expected<ai::AssistantMessage>>(
            ai::Model,
            ai::AiContext,
            ai::SimpleStreamOptions,
            ai::AssistantEventSink)> stream_fn) {
    // The scripted coroutine is re-invocable (one fresh awaitable per turn),
    // so it is shared rather than moved out of the factory's coroutine frame.
    auto shared_fn = std::make_shared<
        std::move_only_function<
            boost::asio::awaitable<util::Expected<ai::AssistantMessage>>(
                ai::Model,
                ai::AiContext,
                ai::SimpleStreamOptions,
                ai::AssistantEventSink)>>(std::move(stream_fn));
    return ai::ModelStreamFactory{
        [shared_fn](
            ai::Model model,
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> ai::ModelStream {
            return ai::detail::make_model_stream(
                [shared_fn,
                 model = std::move(model),
                 context = std::move(context),
                 options = std::move(options)](
                    ai::AssistantEventSink sink) mutable
                    -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
                    co_return co_await (*shared_fn)(
                        std::move(model),
                        std::move(context),
                        std::move(options),
                        std::move(sink));
                });
        }};
}

} // namespace cch::tests
