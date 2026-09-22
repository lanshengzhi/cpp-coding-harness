#include "AnthropicMessagesAdapter.hpp"

#include "AnthropicEvents.hpp"
#include "AnthropicShared.hpp"
#include "ai/Headers.hpp"
#include "ai/Timestamps.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cch::ai::api {

AnthropicMessagesAdapter::AnthropicMessagesAdapter(
    std::shared_ptr<providers::StreamTransport> transport)
    : transport_(std::move(transport)) {}

AnthropicMessagesAdapter::AnthropicMessagesAdapter(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter& AnthropicMessagesAdapter::operator=(AnthropicMessagesAdapter&&) noexcept = default;
AnthropicMessagesAdapter::~AnthropicMessagesAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> AnthropicMessagesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!transport_) {
        co_return std::unexpected(
                providers::make_stream_error("Anthropic Messages adapter requires a stream transport"));
    }
    if (model.api != "anthropic-messages") {
        co_return std::unexpected(
                providers::make_stream_error("Anthropic Messages adapter received the wrong Model API"));
    }
    if (model.base_url.empty()) {
        co_return std::unexpected(providers::make_stream_error("Anthropic Messages Model base URL is required"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if ((!options.auth.api_key || options.auth.api_key->empty()) &&
        !has_header(options.auth.headers, "authorization") &&
        !has_header(options.auth.headers, "x-api-key") &&
        !has_header(options.auth.headers, "cf-aig-authorization")) {
        co_return std::unexpected(providers::make_stream_error("No API key for provider: " + model.provider));
    }

    CCH_TRY(request, build_anthropic_stream_request(model, context, options));

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    auto attempt_state = std::make_shared<AnthropicEventState>();

    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        *attempt_state = AnthropicEventState{};
        return [attempt_state, &model](const providers::SseEvent& event,
                       AssistantMessage& assistant,
                       AssistantEventSink& sink,
                       std::optional<InferenceFailure>& inference_failure) -> support::ExpectedVoid {
            return process_anthropic_sse_event(event, model, assistant, *attempt_state, sink, inference_failure);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant, AssistantEventSink&) -> support::ExpectedVoid {
        return finalize_anthropic_stream(*attempt_state, assistant);
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
        .protocol_name = "Anthropic Messages",
        .request = request,
        .transport = *transport_,
        .options = options,
        .initial_assistant = std::move(assistant),
        .sink = std::move(sink),
        .attempt_hook = std::move(attempt_hook),
        .finalize_hook = std::move(finalize_hook),
    });
}

} // namespace cch::ai::api
