#include "ai/providers/StreamExecutionEngine.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

TEST_CASE("StreamExecutionEngine processes SSE stream and emits start/done events", "[ai][provider][engine]") {
    tests::ScriptedTransport transport;
    transport.attempts.push_back(tests::TransportAttempt{
        .head = {.status_code = 200, .headers = {}},
        .chunks = {
            "event: delta\ndata: hello ",
            "world\n\n",
        },
    });

    ai::providers::StreamRequest request{.url = "https://api.test/v1/stream"};
    ai::ProviderStreamOptions options;
    ai::AssistantMessage initial_assistant;
    initial_assistant.api = "test-api";
    initial_assistant.provider = "test-provider";
    initial_assistant.model = "test-model";
    initial_assistant.stop_reason = ai::AssistantStopReason::Pending;

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    int hook_instantiations = 0;
    auto hook = [&hook_instantiations]() -> support::Expected<ai::providers::SseEventHook> {
        ++hook_instantiations;
        return [](
            const ai::providers::SseEvent& event,
            ai::AssistantMessage& assistant,
            ai::AssistantEventSink&) -> support::ExpectedVoid {
            if (event.event == "delta") {
                assistant.content.emplace_back(ai::TextContent{.text = event.data});
            }
            return {};
        };
    };

    auto finalize = [](ai::AssistantMessage& assistant) -> support::ExpectedVoid {
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        return {};
    };

    auto result = tests::run_awaitable(ai::providers::execute_sse_stream(
        ai::providers::SseStreamExecutionOptions{
            .protocol_name = "Test",
            .request = request,
            .transport = transport,
            .options = options,
            .initial_assistant = std::move(initial_assistant),
            .sink = std::move(sink),
            .attempt_hook = std::move(hook),
            .finalize_hook = std::move(finalize),
        }));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(result->content[0]).text == "hello world");
    CHECK(hook_instantiations == 1);

    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<ai::AssistantStartEvent>(events[0]));
    CHECK(std::holds_alternative<ai::AssistantDoneEvent>(events[1]));
}

TEST_CASE("StreamExecutionEngine retries on 429 and resets state via factory", "[ai][provider][engine]") {
    tests::ScriptedTransport transport;
    // Attempt 0: 429 Rate Limit
    transport.attempts.push_back(tests::TransportAttempt{
        .head = {
            .status_code = 429,
            .headers = {{"retry-after-ms", "1"}},
        },
        .chunks = {"rate limited"},
    });
    // Attempt 1: 200 OK
    transport.attempts.push_back(tests::TransportAttempt{
        .head = {.status_code = 200, .headers = {}},
        .chunks = {"event: data\ndata: success\n\n"},
    });

    ai::providers::StreamRequest request{.url = "https://api.test/v1/stream"};
    ai::ProviderStreamOptions options;
    options.max_retries = 2;
    options.max_retry_delay_ms = 1000;

    ai::AssistantMessage initial_assistant;
    initial_assistant.api = "test-api";
    initial_assistant.provider = "test-provider";
    initial_assistant.model = "test-model";
    initial_assistant.stop_reason = ai::AssistantStopReason::Pending;

    std::vector<ai::AssistantStreamEvent> events;
    auto sink = [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
        events.push_back(event);
        return {};
    };

    int hook_instantiations = 0;
    auto hook = [&hook_instantiations]() -> support::Expected<ai::providers::SseEventHook> {
        ++hook_instantiations;
        return [](
            const ai::providers::SseEvent& event,
            ai::AssistantMessage& assistant,
            ai::AssistantEventSink&) -> support::ExpectedVoid {
            if (event.event == "data") {
                assistant.content.emplace_back(ai::TextContent{.text = event.data});
            }
            return {};
        };
    };

    auto finalize = [](ai::AssistantMessage& assistant) -> support::ExpectedVoid {
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        return {};
    };

    auto result = tests::run_awaitable(ai::providers::execute_sse_stream(
        ai::providers::SseStreamExecutionOptions{
            .protocol_name = "Test",
            .request = request,
            .transport = transport,
            .options = options,
            .initial_assistant = std::move(initial_assistant),
            .sink = std::move(sink),
            .attempt_hook = std::move(hook),
            .finalize_hook = std::move(finalize),
        }));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(hook_instantiations == 2);
    CHECK(transport.requests.size() == 2);
}

TEST_CASE("StreamExecutionEngine isolates sink failure and halts immediately", "[ai][provider][engine]") {
    tests::ScriptedTransport transport;
    transport.attempts.push_back(tests::TransportAttempt{
        .head = {.status_code = 200, .headers = {}},
        .chunks = {"event: message\ndata: payload\n\n"},
    });

    ai::providers::StreamRequest request{.url = "https://api.test/v1/stream"};
    ai::ProviderStreamOptions options;
    ai::AssistantMessage initial_assistant;
    initial_assistant.api = "test-api";

    // Sink fails on AssistantStartEvent
    auto failing_sink = [](const ai::AssistantStreamEvent&) -> support::ExpectedVoid {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "consumer disconnected"));
    };

    auto hook = []() -> support::Expected<ai::providers::SseEventHook> {
        return [](
            const ai::providers::SseEvent&,
            ai::AssistantMessage&,
            ai::AssistantEventSink&) -> support::ExpectedVoid {
            return {};
        };
    };

    auto result = tests::run_awaitable(ai::providers::execute_sse_stream(
        ai::providers::SseStreamExecutionOptions{
            .protocol_name = "Test",
            .request = request,
            .transport = transport,
            .options = options,
            .initial_assistant = std::move(initial_assistant),
            .sink = failing_sink,
            .attempt_hook = hook,
        }));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Unknown);
    CHECK(result.error().message == "consumer disconnected");
}
