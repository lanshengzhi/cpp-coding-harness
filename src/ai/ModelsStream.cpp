#include <cch/ai/Models.hpp>

#include "support/AsyncResultBridge.hpp"
#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Timestamps.hpp>
#include "support/StreamTransport.hpp"
#include "ai/api/CompletionsPipeline.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/StreamEmitter.hpp"
#include <cch/support/BoundedText.hpp>
#include "support/ExpectedMacros.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
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

/// Private one-turn stream implementation (ADR 0040 / ADR 0069). Runs on the
/// consuming executor, resolves authentication, validates the model, and
/// coordinates stream lifecycle and terminal outcomes via StreamEmitter.
/// test_stream and request_started must outlive the awaitable; the outer stream retains Models.
[[nodiscard]] boost::asio::awaitable<support::Expected<AssistantMessage>> stream_impl(std::string api_key,
        std::string base_url,
        bool& request_started,
        ModelStreamFactory* test_stream,
        std::shared_ptr<support::StreamTransport> transport,
        Model model_value,
        AiContext context,
        SimpleStreamOptions options,
        AssistantEventSink sink) {
    AssistantMessage initial_assistant;
    initial_assistant.api = model_value.api;
    initial_assistant.provider = model_value.provider;
    initial_assistant.model = model_value.id;
    initial_assistant.stop_reason = AssistantStopReason::Pending;
    initial_assistant.timestamp = current_timestamp_ms();

    providers::StreamEmitter emitter(std::move(initial_assistant), std::move(sink), api_key);

    if (auto valid = validate_model(model_value); !valid) {
        co_return emitter.finalize_failure(
                categorized_error(support::ErrorCode::ModelValidation, valid.error().message, valid.error()));
    }

    auto stream_sink = emitter.as_sink();
    auto result = co_await [&]() -> boost::asio::awaitable<support::Expected<AssistantMessage>> {
        if (*test_stream) {
            if (!options.stop_token.stop_requested()) request_started = true;
            auto provider_stream = (*test_stream)(std::move(model_value), std::move(context), std::move(options));
            co_return co_await support::detail::await_async_result(
                    std::move(provider_stream).run(std::move(stream_sink)));
        }
        if (model_value.api == kOpenAICompletionsApi) {
            co_return co_await api::execute_completions_stream(*transport,
                    std::move(base_url),
                    model_value,
                    context,
                    api::RequestAuth{.api_key = api_key},
                    std::move(options),
                    emitter,
                    request_started);
        }
        co_return std::unexpected(support::make_error(support::ErrorCode::Stream,
                "Provider " + model_value.provider + " has no API implementation for \"" + model_value.api + "\""));
    }();

    if (emitter.sink_broken()) {
        co_return std::unexpected(*emitter.sink_failure());
    }
    if (result) {
        if (emitter.is_terminated()) {
            co_return emitter.assistant();
        }
        const auto reason = result->stop_reason;
        if (reason == AssistantStopReason::Error || reason == AssistantStopReason::Aborted) {
            co_return emitter.finalize_error_event(AssistantErrorEvent{
                    .reason = reason,
                    .error = std::move(*result),
                    .failure =
                            support::make_error(reason == AssistantStopReason::Aborted ? support::ErrorCode::Cancelled
                                                                                       : support::ErrorCode::Stream,
                                    std::string{}),
            });
        }
        emitter.assistant() = std::move(*result);
        co_return emitter.finalize_success(reason);
    }

    if (emitter.is_terminated()) {
        co_return emitter.assistant();
    }
    if (result.error().code == support::ErrorCode::Cancelled) {
        co_return emitter.finalize_failure(
                support::make_error(support::ErrorCode::Cancelled, std::string{kDefaultAbortDiagnostic}),
                std::nullopt,
                AssistantStopReason::Aborted);
    }
    if (providers::is_models_domain_error(result.error().code)) {
        co_return emitter.finalize_failure(result.error());
    }
    co_return std::unexpected(safe_error(result.error(), api_key));
}

} // namespace

ModelStream Models::stream(Model model, AiContext context, SimpleStreamOptions options) {
    auto self = shared_from_this();
    auto transport = impl_->transport;
    return detail::make_model_stream(
            [self,
                    transport = std::move(transport),
                    model = std::move(model),
                    context = std::move(context),
                    options = std::move(options)](
                    AssistantEventSink sink) mutable -> boost::asio::awaitable<support::Expected<AssistantMessage>> {
                // self retains the private hook and request state for the whole borrowed call.
                co_return co_await stream_impl(self->impl_->api_key,
                        self->impl_->gateway_url + "/v1",
                        self->impl_->request_started,
                        &self->impl_->test_stream,
                        transport,
                        std::move(model),
                        std::move(context),
                        std::move(options),
                        std::move(sink));
            });
}

} // namespace cch::ai
