#include <cch/ai/Models.hpp>

#include "ai/ModelStreamBridge.hpp"
#include "ai/ModelsTestAccess.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] ai::Model make_test_model() {
    auto model = tests::make_model("test-model", "test-provider", "openai-completions");
    return model;
}

[[nodiscard]] ai::AiContext make_test_context() {
    ai::AiContext context;
    context.messages.push_back(ai::user_text_message("hello", 1));
    return context;
}

[[nodiscard]] ai::SimpleStreamOptions make_test_options() {
    ai::SimpleStreamOptions options;
    return options;
}

struct InjectedTerminalRun {
    support::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

[[nodiscard]] InjectedTerminalRun run_injected_terminal(ai::AssistantStreamEvent terminal,
        bool emit_partial = false,
        bool emit_terminal = true,
        std::optional<ai::AssistantMessage> returned_message = std::nullopt,
        ai::Model model = make_test_model()) {
    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "secret-key"});
    ai::detail::ModelsTestAccess::set_stream(*models,
            [terminal = std::move(terminal),
                    emit_partial,
                    emit_terminal,
                    returned_message = std::move(returned_message)](
                    ai::Model model, ai::AiContext, ai::SimpleStreamOptions) mutable {
                return ai::detail::make_model_stream([terminal = std::move(terminal),
                                                             emit_partial,
                                                             emit_terminal,
                                                             model = std::move(model),
                                                             returned_message = std::move(returned_message)](
                                                             ai::AssistantEventSink sink) mutable
                                                             -> boost::asio::awaitable<
                                                                     support::Expected<ai::AssistantMessage>> {
                    if (emit_terminal) {
                        ai::AssistantMessage initial;
                        initial.api = model.api;
                        initial.provider = model.provider;
                        initial.model = model.id;
                        if (auto started = sink(ai::AssistantStartEvent{.partial = std::move(initial)}); !started) {
                            co_return std::unexpected(started.error());
                        }
                    }
                    if (emit_partial) {
                        ai::AssistantMessage partial = ai::assistant_text_message("partial");
                        partial.api = model.api;
                        partial.provider = model.provider;
                        partial.model = model.id;
                        if (auto emitted = sink(ai::TextDeltaEvent{
                                    .content_index = 0,
                                    .delta = "partial",
                                    .partial = partial,
                            });
                                !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    auto message =
                            returned_message
                                    ? std::move(*returned_message)
                                    : std::visit(
                                              [](const auto& event) -> ai::AssistantMessage {
                                                  using Event = std::decay_t<decltype(event)>;
                                                  if constexpr (std::is_same_v<Event, ai::AssistantErrorEvent>) {
                                                      return event.error;
                                                  } else if constexpr (std::is_same_v<Event, ai::AssistantDoneEvent>) {
                                                      return event.message;
                                                  } else {
                                                      return {};
                                                  }
                                              },
                                              terminal);
                    if (emit_terminal) {
                        if (auto emitted = sink(terminal); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return message;
                });
            });

    InjectedTerminalRun run;
    auto captured_events = std::make_shared<std::vector<ai::AssistantStreamEvent>>();
    run.result = tests::run_async_result(models->stream(std::move(model), {}, {})
                    .run([captured_events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                        captured_events->push_back(event);
                        return {};
                    }));
    run.events = std::move(*captured_events);
    return run;
}

} // namespace

TEST_CASE("Completions stream execution processes SSE stream and emits start/done events",
        "[ai][api][completions][engine][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks =
                    {
                            "data: {\"choices\":[{\"delta\":{\"content\":\"hello \"}}]}\n\n",
                            "data: {\"choices\":[{\"delta\":{\"content\":\"world\"},\"finish_reason\":\"stop\"}]}\n\n",
                            "data: [DONE]\n\n",
                    },
    });

    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(std::move(sink)));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(result->content[0]).text == "hello world");
    CHECK(transport->requests.size() == 1);

    REQUIRE(events.size() >= 3);
    CHECK(std::holds_alternative<ai::AssistantStartEvent>(events.front()));
    CHECK(std::holds_alternative<ai::AssistantDoneEvent>(events.back()));
}

TEST_CASE("Completions stream execution retries on 429 and resets state", "[ai][api][completions][engine][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    // Attempt 0: 429 Rate Limit
    transport->attempts.push_back(tests::TransportAttempt{
            .head =
                    {
                            .status_code = 429,
                            .headers = {{"retry-after-ms", "1"}},
                    },
            .chunks = {"{\"error\":{\"message\":\"rate limited\",\"type\":\"rate_limit_error\"}}"},
    });
    // Attempt 1: 200 OK
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks =
                    {
                            "data: "
                            "{\"choices\":[{\"delta\":{\"content\":\"success\"},\"finish_reason\":\"stop\"}]}\n\n",
                            "data: [DONE]\n\n",
                    },
    });

    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();
    options.max_retries = 2;
    options.max_retry_delay_ms = 1000;

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(std::move(sink)));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(result->content[0]).text == "success");
    CHECK(transport->requests.size() == 2);
}

TEST_CASE("Completions stream execution isolates sink failure and halts immediately",
        "[ai][api][completions][engine][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"payload\"}}]}\n\n"},
    });

    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();

    // Sink fails on AssistantStartEvent
    auto failing_sink = [](const ai::AssistantStreamEvent&) -> support::ExpectedVoid {
        return std::unexpected(support::make_error(support::ErrorCode::Unknown, "consumer disconnected"));
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(failing_sink));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Unknown);
    CHECK(result.error().message == "consumer disconnected");
}

TEST_CASE("Models stream normalizes every typed cancellation signal over conflicting failures",
        "[ai][api][completions][engine][cancellation][issue1042]") {
    auto make_terminal = [](ai::AssistantStopReason reason,
                                 ai::AssistantStopReason message_reason,
                                 support::ErrorCode error_code,
                                 ai::InferenceFailureKind failure_kind) {
        ai::AssistantMessage message;
        message.timestamp = 1234;
        message.stop_reason = message_reason;
        message.error_message = "custom cancellation detail";
        return ai::AssistantErrorEvent{
                .reason = reason,
                .error = std::move(message),
                .failure =
                        support::make_error(error_code, "transport detail", "secret-key detail", "secret-key context"),
                .inference_failure =
                        ai::InferenceFailure{
                                .kind = failure_kind,
                                .output_started = false,
                                .suggested_backoff_ms = 73,
                                .provider_code = "provider-code",
                        },
        };
    };

    const std::array terminals{
            make_terminal(ai::AssistantStopReason::Aborted,
                    ai::AssistantStopReason::Error,
                    support::ErrorCode::Auth,
                    ai::InferenceFailureKind::Unauthorized),
            make_terminal(ai::AssistantStopReason::Error,
                    ai::AssistantStopReason::Aborted,
                    support::ErrorCode::Auth,
                    ai::InferenceFailureKind::Unauthorized),
            make_terminal(ai::AssistantStopReason::Error,
                    ai::AssistantStopReason::Error,
                    support::ErrorCode::Cancelled,
                    ai::InferenceFailureKind::Unauthorized),
            make_terminal(ai::AssistantStopReason::Error,
                    ai::AssistantStopReason::Error,
                    support::ErrorCode::Auth,
                    ai::InferenceFailureKind::Cancelled),
    };

    for (auto terminal : terminals) {
        auto run = run_injected_terminal(std::move(terminal), true);
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
        CHECK(run.result->timestamp == 1234);
        REQUIRE(run.result->error_message);
        CHECK(*run.result->error_message == "custom cancellation detail");
        REQUIRE(run.result->content.size() == 1);
        CHECK(std::get<ai::TextContent>(run.result->content.front()).text == "partial");
        CHECK(run.result->api == "openai-completions");
        CHECK(run.result->provider == "test-provider");
        CHECK(run.result->model == "test-model");

        REQUIRE_FALSE(run.events.empty());
        REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
        const auto& normalized = std::get<ai::AssistantErrorEvent>(run.events.back());
        CHECK(normalized.reason == ai::AssistantStopReason::Aborted);
        CHECK(normalized.error.stop_reason == ai::AssistantStopReason::Aborted);
        REQUIRE(normalized.failure);
        CHECK(normalized.failure->code == support::ErrorCode::Cancelled);
        CHECK(normalized.failure->message == "custom cancellation detail");
        CHECK(normalized.failure->detail == "[REDACTED] detail");
        CHECK(normalized.failure->context == std::optional<std::string>{"[REDACTED] context"});
        REQUIRE(normalized.inference_failure);
        CHECK(normalized.inference_failure->kind == ai::InferenceFailureKind::Cancelled);
        CHECK(normalized.inference_failure->output_started);
        CHECK(normalized.inference_failure->suggested_backoff_ms == 73);
        CHECK(normalized.inference_failure->provider_code == "provider-code");
    }

    ai::AssistantMessage returned_message = ai::assistant_text_message("provider returned partial", 4321);
    returned_message.content.clear();
    returned_message.stop_reason = ai::AssistantStopReason::Aborted;
    returned_message.error_message = "custom return cancellation";
    auto returned_value = run_injected_terminal(
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Stop,
                    .message = ai::assistant_text_message("ignored"),
            },
            false,
            false,
            std::move(returned_message));
    REQUIRE(returned_value.result);
    CHECK(returned_value.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(returned_value.result->timestamp == 4321);
    CHECK(returned_value.result->error_message == "custom return cancellation");
    REQUIRE_FALSE(returned_value.events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(returned_value.events.back()));
    const auto& returned_abort = std::get<ai::AssistantErrorEvent>(returned_value.events.back());
    REQUIRE(returned_abort.failure);
    CHECK(returned_abort.failure->code == support::ErrorCode::Cancelled);

    auto returned_error_message = ai::assistant_text_message("provider returned error", 5432);
    returned_error_message.content.clear();
    returned_error_message.stop_reason = ai::AssistantStopReason::Error;
    returned_error_message.error_message = "custom returned error detail";
    auto returned_error = run_injected_terminal(
            ai::AssistantErrorEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .error = ai::AssistantMessage{},
            },
            false,
            false,
            std::move(returned_error_message));
    REQUIRE(returned_error.result);
    CHECK(returned_error.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(returned_error.result->timestamp == 5432);
    CHECK(returned_error.result->error_message == "custom returned error detail");
    REQUIRE_FALSE(returned_error.events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(returned_error.events.back()));
    const auto& returned_failure = std::get<ai::AssistantErrorEvent>(returned_error.events.back());
    REQUIRE(returned_failure.failure);
    CHECK(returned_failure.failure->code == support::ErrorCode::Stream);
}

TEST_CASE("Models stream converts error-marked done events into one error terminal",
        "[ai][api][completions][engine][issue1042]") {
    const std::array terminals{
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .message = ai::assistant_text_message("ordinary done", 1234),
            },
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Stop,
                    .message =
                            [] {
                                auto message = ai::assistant_text_message("error done", 2345);
                                message.stop_reason = ai::AssistantStopReason::Error;
                                return message;
                            }(),
            },
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Stop,
                    .message =
                            [] {
                                auto message = ai::assistant_text_message("aborted done", 5678);
                                message.stop_reason = ai::AssistantStopReason::Aborted;
                                return message;
                            }(),
            },
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Error,
                    .message =
                            [] {
                                auto message = ai::assistant_text_message("aborted conflict", 3456);
                                message.stop_reason = ai::AssistantStopReason::Aborted;
                                return message;
                            }(),
            },
            ai::AssistantDoneEvent{
                    .reason = ai::AssistantStopReason::Aborted,
                    .message =
                            [] {
                                auto message = ai::assistant_text_message("conflicting done", 6789);
                                message.stop_reason = ai::AssistantStopReason::Error;
                                return message;
                            }(),
            },
    };

    for (const auto& terminal : terminals) {
        const bool aborted_input = terminal.reason == ai::AssistantStopReason::Aborted ||
                                   terminal.message.stop_reason == ai::AssistantStopReason::Aborted;
        auto run = run_injected_terminal(terminal);
        REQUIRE(run.result);
        CHECK(run.result->stop_reason ==
                (aborted_input ? ai::AssistantStopReason::Aborted : ai::AssistantStopReason::Error));
        CHECK(std::ranges::count_if(run.events, [](const ai::AssistantStreamEvent& event) {
            return std::holds_alternative<ai::AssistantDoneEvent>(event) ||
                   std::holds_alternative<ai::AssistantErrorEvent>(event);
        }) == 1);
        REQUIRE_FALSE(run.events.empty());
        REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
        const auto& error = std::get<ai::AssistantErrorEvent>(run.events.back());
        CHECK(error.reason == run.result->stop_reason);
        CHECK(error.error.stop_reason == run.result->stop_reason);
        REQUIRE(error.failure);
        CHECK(error.failure->code == (aborted_input ? support::ErrorCode::Cancelled : support::ErrorCode::Stream));
    }

    const auto successful = run_injected_terminal(ai::AssistantDoneEvent{
            .reason = ai::AssistantStopReason::Stop,
            .message = ai::assistant_text_message("ordinary success", 9012),
    });
    REQUIRE(successful.result);
    CHECK(successful.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(successful.result->timestamp == 9012);
    REQUIRE_FALSE(successful.events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantDoneEvent>(successful.events.back()));
}

TEST_CASE("Models stream propagates terminal sink failure without retry or re-emission",
        "[ai][api][completions][engine][sink][issue1042]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 429, .headers = {{"retry-after-ms", "1"}}},
            .chunks = {"{\"error\":{\"message\":\"rate limited\"}}"},
    });
    transport->attempts.push_back(tests::TransportAttempt{
            .failure = support::make_error(support::ErrorCode::Network, "terminal transport failure"),
    });
    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);

    auto terminal_calls = std::make_shared<std::size_t>(0);
    auto options = make_test_options();
    options.max_retries = 1;
    options.max_retry_delay_ms = 1;
    auto result = tests::run_async_result(models->stream(make_test_model(), {}, std::move(options))
                    .run([terminal_calls](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
                        if (std::holds_alternative<ai::AssistantErrorEvent>(event) ||
                                std::holds_alternative<ai::AssistantDoneEvent>(event)) {
                            ++*terminal_calls;
                            return std::unexpected(support::make_error(
                                    support::ErrorCode::Unknown, "terminal sink disconnected", "sink detail"));
                        }
                        return {};
                    }));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Unknown);
    CHECK(result.error().message == "terminal sink disconnected");
    CHECK(result.error().detail == "sink detail");
    CHECK(*terminal_calls == 1);
    CHECK(transport->requests.size() == 2);
}

TEST_CASE("Completions stream execution locks out retry once output has started",
        "[ai][api][completions][engine][spec]") {
    class MidStreamFailingTransport final : public support::StreamTransport {
    public:
        [[nodiscard]] boost::asio::awaitable<support::Expected<support::StreamResponse>> async_stream(
                const support::StreamRequest&, support::BodyChunkHandler on_body_chunk) override {
            ++calls;
            // Deliver a chunk so output_started transitions to true
            CCH_TRY_VOID(on_body_chunk("data: {\"choices\":[{\"delta\":{\"content\":\"first chunk\"}}]}\n\n"));
            // Then fail the connection mid-stream
            co_return std::unexpected(support::make_error(support::ErrorCode::Network, "connection dropped"));
        }
        int calls{0};
    };

    auto transport = std::make_shared<MidStreamFailingTransport>();
    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();
    options.max_retries = 3;
    options.max_retry_delay_ms = 1000;

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(std::move(sink)));

    // Output started, so retry was strictly locked out
    CHECK(transport->calls == 1);
    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Error);

    // Terminal error event was emitted exactly once
    REQUIRE_FALSE(events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(events.back()));
    const auto& error_event = std::get<ai::AssistantErrorEvent>(events.back());
    CHECK(error_event.reason == ai::AssistantStopReason::Error);
    REQUIRE(error_event.inference_failure.has_value());
    CHECK(error_event.inference_failure->output_started == true);
}

TEST_CASE("Completions stream execution normalizes cancellation into aborted terminal message",
        "[ai][api][completions][engine][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"chunk\"}}]}\n\n"},
    });

    std::stop_source stop;
    stop.request_stop();

    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();
    options.stop_token = stop.get_token();

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(std::move(sink)));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(result->error_message == "Request was aborted");
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(events[0]));
    const auto& error_event = std::get<ai::AssistantErrorEvent>(events[0]);
    CHECK(error_event.reason == ai::AssistantStopReason::Aborted);
    REQUIRE(error_event.failure);
    CHECK(error_event.failure->code == support::ErrorCode::Cancelled);
    CHECK(error_event.failure->message == "Request was aborted");
    REQUIRE(error_event.inference_failure.has_value());
    CHECK(error_event.inference_failure->kind == ai::InferenceFailureKind::Cancelled);
    CHECK(error_event.inference_failure->output_started == false);
}

TEST_CASE("Completions stream execution emits terminal error when cancelled during backoff sleep",
        "[ai][api][completions][engine][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    // Attempt 0: 429 Rate Limit with delay
    transport->attempts.push_back(tests::TransportAttempt{
            .head =
                    {
                            .status_code = 429,
                            .headers = {{"retry-after-ms", "5000"}},
                    },
            .chunks = {"{\"error\":{\"message\":\"rate limited\"}}"},
    });

    std::stop_source stop;
    auto model = make_test_model();
    auto context = make_test_context();
    auto options = make_test_options();
    options.max_retries = 2;
    options.max_retry_delay_ms = 5000;
    options.stop_token = stop.get_token();

    // Cancel while in backoff sleep
    transport->on_request = [&stop]() { stop.request_stop(); };

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    auto models = std::make_shared<ai::Models>(ai::ModelsOptions{.api_key = "test-api-key"});
    ai::detail::ModelsTestAccess::set_transport(*models, transport);
    auto result =
            tests::run_async_result(models->stream(model, std::move(context), std::move(options)).run(std::move(sink)));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(result->error_message == "Request was aborted");
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(events[0]));
    const auto& error_event = std::get<ai::AssistantErrorEvent>(events[0]);
    CHECK(error_event.reason == ai::AssistantStopReason::Aborted);
    REQUIRE(error_event.failure);
    CHECK(error_event.failure->code == support::ErrorCode::Cancelled);
    CHECK(error_event.failure->message == "Request was aborted");
    REQUIRE(error_event.inference_failure.has_value());
    CHECK(error_event.inference_failure->kind == ai::InferenceFailureKind::Cancelled);
    CHECK(error_event.inference_failure->output_started == false);
}
