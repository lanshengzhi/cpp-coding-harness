#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include "ai/providers/OpenAIProvider.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiFixture.hpp"
#include "support/ScriptedWebSocket.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "util/Json.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

using tests::EmptyAuthContext;
using tests::EmptyCredentialStore;
using tests::run_awaitable;
using tests::ScriptedTransport;
using tests::ScriptedWebSocket;
using tests::ScriptedWebSocketTransport;
using tests::TransportAttempt;

/// JWT-shaped dummy Codex token carrying the chatgpt_account_id claim
/// `acc_test` (same shape as the frozen pi test mockToken).
constexpr std::string_view kCodexToken =
    "aaa.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjX3Rlc3QifX0=.bbb";
constexpr std::string_view kCodexAccountId = "acc_test";

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

[[nodiscard]] std::string read_fixture_text(std::string_view relative_path) {
    const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-ai/" +
                             std::string{relative_path};
    std::ifstream input(path, std::ios::binary);
    return std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
}

[[nodiscard]] ai::Model codex_model() {
    auto model = tests::make_model(
        "gpt-5.5-codex", "openai-codex", "openai-codex-responses");
    model.base_url = "https://chatgpt.com/backend-api";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::XHigh, "xhigh"},
    };
    model.input = {ai::ModelInput::Text};
    model.cost = ai::ModelCost{
        .input = 2.0,
        .output = 4.0,
        .cache_read = 1.0,
        .cache_write = 3.0,
    };
    return model;
}

struct CodexHarness {
    std::shared_ptr<ScriptedTransport> http;
    std::shared_ptr<ScriptedWebSocketTransport> ws;
    std::unique_ptr<ai::Models> models;
};

[[nodiscard]] CodexHarness make_codex_harness(
    const ai::Model& model,
    ai::api::CodexWebSocketCacheConfig cache_config = {}) {
    CodexHarness harness;
    harness.http = std::make_shared<ScriptedTransport>();
    harness.ws = std::make_shared<ScriptedWebSocketTransport>();
    harness.models = std::make_unique<ai::Models>(
        std::make_shared<EmptyCredentialStore>(),
        std::make_shared<EmptyAuthContext>());
    auto provider = ai::providers::make_openai_codex_responses_provider(
        "openai-codex", {model}, {}, harness.http, harness.ws, cache_config);
    auto registered = harness.models->set_provider(std::move(provider));
    REQUIRE(registered);
    return harness;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = {
            ai::text_content("hi"),
            ai::image_content("YWJj", "image/png"),
        },
        .timestamp = 1,
    });
    context.tools.push_back(ai::Tool{
        .name = "lookup",
        .description = "Look up a value",
        .parameters = util::JsonValue::object_t{
            {"properties", util::JsonValue::object_t{
                {"q", util::JsonValue::object_t{{"type", "string"}}},
            }},
            {"required", util::JsonValue::array_t{"q"}},
            {"type", "object"},
        },
    });
    return context;
}

[[nodiscard]] RunResult run_codex(
    ai::Models& models,
    const ai::Model& model,
    ai::AiContext context,
    ai::SimpleStreamOptions options) {    std::vector<ai::AssistantStreamEvent> events;
    auto result = run_awaitable(models.stream_simple(
        model,
        std::move(context),
        std::move(options),
        [&events](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            return {};
        }));
    return RunResult{.result = std::move(result), .events = std::move(events)};
}

[[nodiscard]] ai::AiContext user_context(std::string text) {
    ai::AiContext context;
    context.messages.push_back(ai::user_text_message(std::move(text), 1));
    return context;
}

[[nodiscard]] std::string event_name(const ai::AssistantStreamEvent& event) {
    if (std::holds_alternative<ai::AssistantStartEvent>(event)) return "start";
    if (std::holds_alternative<ai::ThinkingStartEvent>(event)) return "thinking_start";
    if (std::holds_alternative<ai::ThinkingDeltaEvent>(event)) return "thinking_delta";
    if (std::holds_alternative<ai::ThinkingEndEvent>(event)) return "thinking_end";
    if (std::holds_alternative<ai::TextStartEvent>(event)) return "text_start";
    if (std::holds_alternative<ai::TextDeltaEvent>(event)) return "text_delta";
    if (std::holds_alternative<ai::TextEndEvent>(event)) return "text_end";
    if (std::holds_alternative<ai::ToolCallStartEvent>(event)) return "toolcall_start";
    if (std::holds_alternative<ai::ToolCallDeltaEvent>(event)) return "toolcall_delta";
    if (std::holds_alternative<ai::ToolCallEndEvent>(event)) return "toolcall_end";
    if (std::holds_alternative<ai::AssistantDoneEvent>(event)) return "done";
    return "error";
}

[[nodiscard]] std::vector<std::string> event_names(
    const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<std::string> result;
    for (const auto& event : events) {
        result.push_back(event_name(event));
    }
    return result;
}

[[nodiscard]] std::string json_string(const util::JsonValue& value) {
    auto text = util::write_json(value);
    REQUIRE(text);
    return std::move(*text);
}

[[nodiscard]] std::string terminal_frame(
    std::string type,
    std::string status,
    util::JsonValue::object_t extra = {}) {
    util::JsonValue::object_t response{
        {"id", "resp_terminal"},
        {"status", std::move(status)},
    };
    for (auto& [key, value] : extra) {
        response.emplace(std::move(key), std::move(value));
    }
    util::JsonValue::object_t event{
        {"type", std::move(type)},
        {"response", std::move(response)},
    };
    return json_string(util::JsonValue{std::move(event)});
}

[[nodiscard]] std::string simple_terminal(
    std::string type = "response.completed",
    std::string status = "completed") {
    return terminal_frame(
        std::move(type),
        std::move(status),
        util::JsonValue::object_t{
            {"usage", util::JsonValue::object_t{
                {"input_tokens", 5.0},
                {"output_tokens", 3.0},
                {"total_tokens", 8.0},
            }},
        });
}

[[nodiscard]] std::string error_frame(std::string code, std::string message) {
    util::JsonValue::object_t error{{"code", std::move(code)}};
    util::JsonValue::object_t event{
        {"error", std::move(error)},
        {"message", std::move(message)},
        {"type", "error"},
    };
    return json_string(util::JsonValue{std::move(event)});
}

} // namespace

TEST_CASE(
    "Codex streams the frozen WS request and event sequence through Models",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    const auto ws_fixture = tests::read_pi_fixture("wire/openai-codex-responses-ws.json");
    REQUIRE(ws_fixture);
    const auto* ws_object = ws_fixture->get_if<util::JsonValue::object_t>();
    REQUIRE(ws_object);
    const auto request_found = ws_object->find("request");
    const auto events_found = ws_object->find("events");
    REQUIRE(request_found != ws_object->end());
    REQUIRE(events_found != ws_object->end());
    const auto* events = events_found->second.get_if<util::JsonValue::array_t>();
    REQUIRE(events);
    const auto expected_request_bytes = json_string(request_found->second);

    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->on_send = [events](ScriptedWebSocket& socket, std::string_view) {
        for (const auto& event : *events) {
            socket.session()->frames.push_back(json_string(event));
        }
    };
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::XHigh;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;
    auto run = run_codex(*harness.models, codex_model(), request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(run.result->response_id == "resp_codex");
    CHECK(run.result->usage.input == 90);
    CHECK(run.result->usage.output == 30);
    CHECK(run.result->usage.cache_read == 20);
    CHECK(run.result->usage.cache_write == 10);
    CHECK(run.result->usage.reasoning == 7);
    CHECK(run.result->usage.total_tokens == 150);
    CHECK(run.result->usage.cost.input == 0.00018);
    CHECK(run.result->usage.cost.output == 0.00012);
    CHECK(run.result->usage.cost.cache_read == 0.00002);
    CHECK(run.result->usage.cost.cache_write == 0.00003);
    CHECK(std::abs(run.result->usage.cost.total - 0.00035) < 1e-12);
    REQUIRE(run.result->content.size() == 3);
    const auto& thinking = std::get<ai::ThinkingContent>(run.result->content[0]);
    CHECK(thinking.thinking == "plan");
    REQUIRE(thinking.thinking_signature);
    CHECK(thinking.thinking_signature->contains("dummy-encrypted"));
    const auto& text = std::get<ai::TextContent>(run.result->content[1]);
    CHECK(text.text == "Hello");
    REQUIRE(text.text_signature);
    const auto signature = util::read_json<util::JsonValue>(*text.text_signature);
    REQUIRE(signature);
    CHECK(signature->at("id").get_string() == "msg_1");
    CHECK(signature->at("v").get_number() == 1);
    CHECK(signature->at("phase").get_string() == "final_answer");
    const auto& tool = std::get<ai::ToolCallContent>(run.result->content[2]);
    CHECK(tool.id == "call_1|fc_1");
    CHECK(tool.name == "lookup");
    REQUIRE(tool.arguments);
    CHECK(tool.arguments->at("q").get_string() == "x");

    const auto expected_event_snapshot = tests::read_pi_fixture(
        "wire/openai-codex-responses-ws-ts-events.json");
    REQUIRE(expected_event_snapshot);
    const auto* expected_event_values =
        expected_event_snapshot->get_if<util::JsonValue::array_t>();
    REQUIRE(expected_event_values);
    std::vector<std::string> expected_events;
    for (const auto& value : *expected_event_values) {
        expected_events.push_back(value.get_string());
    }
    CHECK(event_names(run.events) == expected_events);

    REQUIRE(harness.ws->requests.size() == 1);
    const auto& connect = harness.ws->requests.front();
    CHECK(connect.url == "wss://chatgpt.com/backend-api/codex/responses");
    CHECK(connect.connect_timeout == std::chrono::milliseconds{15000});
    CHECK(connect.idle_timeout == std::chrono::milliseconds{4321});
    CHECK(connect.headers.at("Authorization") == "Bearer " + std::string{kCodexToken});
    CHECK(connect.headers.at("chatgpt-account-id") == kCodexAccountId);
    CHECK(connect.headers.at("originator") == "pi");
    CHECK(connect.headers.at("session-id") == "session-1");
    CHECK(connect.headers.at("x-client-request-id") == "session-1");
    // pi's connectWebSocket strips the OpenAI-Beta header before the handshake.
    CHECK_FALSE(connect.headers.contains("OpenAI-Beta"));
    CHECK_FALSE(connect.headers.contains("openai-beta"));
    CHECK_FALSE(connect.headers.contains("accept"));
    CHECK_FALSE(connect.headers.contains("content-type"));
    CHECK_FALSE(connect.headers.contains("x-api-key"));
    REQUIRE(harness.ws->sockets.size() == 1);
    const auto& sent = harness.ws->sockets.front()->session()->sent_frames;
    REQUIRE(sent.size() == 1);
    CHECK(sent.front() == expected_request_bytes);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex falls back to SSE with a diagnostic when WebSocket connect fails",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{
            .failure = util::make_error(
                util::ErrorCode::Network,
                "connect refused"),
        });
    const auto sse = read_fixture_text("wire/openai-codex-responses.sse");
    REQUIRE_FALSE(sse.empty());
    harness.http->attempts.push_back(TransportAttempt{.chunks = {sse}});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::XHigh;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;
    auto run = run_codex(*harness.models, codex_model(), request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(run.result->usage.input == 90);
    CHECK(run.result->usage.cache_write == 10);
    REQUIRE(run.result->content.size() == 3);
    CHECK(std::get<ai::TextContent>(run.result->content[1]).text == "Hello");
    const auto expected_event_snapshot = tests::read_pi_fixture(
        "wire/openai-codex-responses-ts-events.json");
    REQUIRE(expected_event_snapshot);
    const auto* expected_event_values =
        expected_event_snapshot->get_if<util::JsonValue::array_t>();
    REQUIRE(expected_event_values);
    std::vector<std::string> expected_events;
    for (const auto& value : *expected_event_values) {
        expected_events.push_back(value.get_string());
    }
    CHECK(event_names(run.events) == expected_events);

    REQUIRE(run.result->diagnostics);
    REQUIRE(run.result->diagnostics->size() == 1);
    const auto& diagnostic = run.result->diagnostics->front();
    CHECK(diagnostic.type == "provider_transport_failure");
    REQUIRE(diagnostic.error);
    CHECK(diagnostic.error->message == "connect refused");
    REQUIRE(diagnostic.details);
    const auto& details = diagnostic.details->get_object();
    CHECK(details.at("configuredTransport").get_string() == "auto");
    CHECK(details.at("fallbackTransport").get_string() == "sse");
    CHECK_FALSE(details.at("eventsEmitted").get_boolean());
    CHECK(details.at("phase").get_string() == "before_message_stream_start");

    REQUIRE(harness.http->requests.size() == 1);
    const auto& request = harness.http->requests.front();
    CHECK(request.url == "https://chatgpt.com/backend-api/codex/responses");
    CHECK(request.timeout == std::chrono::milliseconds{4321});
    CHECK(request.headers.at("Authorization") == "Bearer " + std::string{kCodexToken});
    CHECK(request.headers.at("chatgpt-account-id") == kCodexAccountId);
    CHECK(request.headers.at("originator") == "pi");
    CHECK(request.headers.at("OpenAI-Beta") == "responses=experimental");
    CHECK(request.headers.at("accept") == "text/event-stream");
    CHECK(request.headers.at("content-type") == "application/json");
    CHECK(request.headers.at("session-id") == "session-1");
    CHECK(request.headers.at("x-client-request-id") == "session-1");
    CHECK_FALSE(request.headers.contains("content-encoding"));
    CHECK_FALSE(request.headers.contains("x-api-key"));
    auto expected_request_bytes = read_fixture_text(
        "wire/openai-codex-responses-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(request.body == expected_request_bytes);
    REQUIRE(util::read_json<util::JsonValue>(request.body));
}

TEST_CASE(
    "Codex falls back to SSE when the WebSocket is idle before the first event",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});
    const auto sse = read_fixture_text("wire/openai-codex-responses.sse");
    harness.http->attempts.push_back(TransportAttempt{.chunks = {sse}});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.session_id = "ws-idle-before-start";
    options.timeout_ms = 50;
    auto run = run_codex(*harness.models, codex_model(), request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(run.result->diagnostics);
    REQUIRE(run.result->diagnostics->size() == 1);
    const auto& diagnostic = run.result->diagnostics->front();
    CHECK(diagnostic.type == "provider_transport_failure");
    REQUIRE(diagnostic.error);
    CHECK(diagnostic.error->message == "WebSocket idle timeout after 50ms");
    REQUIRE(diagnostic.details);
    const auto& details = diagnostic.details->get_object();
    CHECK(details.at("fallbackTransport").get_string() == "sse");
    CHECK_FALSE(details.at("eventsEmitted").get_boolean());
    CHECK(details.at("phase").get_string() == "before_message_stream_start");
    REQUIRE(harness.http->requests.size() == 1);
    REQUIRE(session->sent_frames.size() == 1);
}

TEST_CASE(
    "Codex surfaces WebSocket failures after the first event without SSE fallback",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames.push_back(
            "{\"type\":\"response.output_item.added\",\"output_index\":0,"
            "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\","
            "\"status\":\"in_progress\",\"content\":[]}}");
        socket.session()->receive_failures.push_back(util::make_error(
            util::ErrorCode::Network,
            "connection reset"));
    };
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.session_id = "post-start-failure";
    options.timeout_ms = 50;
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("connection reset") != std::string::npos);
    REQUIRE(run.result->diagnostics);
    REQUIRE(run.result->diagnostics->size() == 1);
    const auto& diagnostic = run.result->diagnostics->front();
    REQUIRE(diagnostic.details);
    const auto& details = diagnostic.details->get_object();
    CHECK(details.at("fallbackTransport").holds<util::JsonValue::null_t>());
    CHECK(details.at("eventsEmitted").get_boolean());
    CHECK(details.at("phase").get_string() == "after_message_stream_start");
    CHECK(harness.http->requests.empty());
    CHECK(run.events.size() >= 2);
}

TEST_CASE(
    "Codex retries previous_response_not_found once on WebSocket",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto first = std::make_shared<ScriptedWebSocket::Session>();
    auto second = std::make_shared<ScriptedWebSocket::Session>();
    first->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        if (socket.session()->sent_frames.size() == 1) {
            socket.session()->frames.push_back(simple_terminal());
        } else {
            socket.session()->frames.push_back(error_frame(
                "previous_response_not_found",
                "Previous response with id 'resp_1' not found."));
        }
    };
    second->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames = {
            "{\"type\":\"response.output_item.added\",\"output_index\":0,"
            "\"item\":{\"type\":\"message\",\"id\":\"msg_2\",\"role\":\"assistant\","
            "\"status\":\"in_progress\",\"content\":[]}}",
            "{\"type\":\"response.output_text.delta\",\"output_index\":0,"
            "\"delta\":\"Recovered\"}",
            "{\"type\":\"response.output_item.done\",\"output_index\":0,"
            "\"item\":{\"type\":\"message\",\"id\":\"msg_2\",\"role\":\"assistant\","
            "\"status\":\"completed\",\"content\":[{\"type\":\"output_text\","
            "\"text\":\"Recovered\",\"annotations\":[]}]}}",
            simple_terminal(),
        };
    };
    harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = first},
        ScriptedWebSocketTransport::ConnectScript{.session = second},
    };

    const auto model = codex_model();
    ai::SimpleStreamOptions first_options;
    first_options.api_key = std::string{kCodexToken};
    first_options.session_id = "missing-continuation";
    auto first_run = run_codex(
        *harness.models, model,
        user_context("Say hello"), std::move(first_options));
    REQUIRE(first_run.result);
    CHECK(first_run.result->response_id == "resp_terminal");

    ai::AiContext second_context;
    second_context.messages.push_back(ai::UserMessage{
        .content = {ai::text_content("Say hello")},
        .timestamp = 1,
    });
    second_context.messages.push_back(*first_run.result);
    second_context.messages.push_back(ai::UserMessage{
        .content = {ai::text_content("Now finish")},
        .timestamp = 2,
    });
    ai::SimpleStreamOptions second_options;
    second_options.api_key = std::string{kCodexToken};
    second_options.session_id = "missing-continuation";
    auto second_run = run_codex(
        *harness.models, model, std::move(second_context), std::move(second_options));

    REQUIRE(second_run.result);
    CHECK(second_run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(second_run.result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(second_run.result->content[0]).text == "Recovered");
    CHECK(harness.ws->requests.size() == 2);
    CHECK(first->sent_frames.size() == 2);
    CHECK(second->sent_frames.size() == 1);
    const auto first_body = util::read_json<util::JsonValue>(first->sent_frames[0]);
    REQUIRE(first_body);
    CHECK_FALSE(first_body->get_object().contains("previous_response_id"));
    const auto delta_body = util::read_json<util::JsonValue>(first->sent_frames[1]);
    REQUIRE(delta_body);
    CHECK(delta_body->at("previous_response_id").get_string() == "resp_terminal");
    CHECK_FALSE(delta_body->at("store").get_boolean());
    const auto& delta_input = delta_body->at("input").get_array();
    REQUIRE(delta_input.size() == 1);
    CHECK(delta_input[0].at("role").get_string() == "user");
    const auto retry_body = util::read_json<util::JsonValue>(second->sent_frames[0]);
    REQUIRE(retry_body);
    CHECK_FALSE(retry_body->get_object().contains("previous_response_id"));
    REQUIRE(retry_body->at("input").get_array().size() == 2);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex retries websocket_connection_limit_reached once before start",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto first = std::make_shared<ScriptedWebSocket::Session>();
    auto second = std::make_shared<ScriptedWebSocket::Session>();
    first->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames.push_back(error_frame(
            "websocket_connection_limit_reached",
            "Connection limit reached"));
    };
    second->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames.push_back(simple_terminal());
    };
    harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = first},
        ScriptedWebSocketTransport::ConnectScript{.session = second},
    };

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(harness.ws->requests.size() == 2);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex marks a session SSE-only after a WebSocket failure",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{
            .failure = util::make_error(
                util::ErrorCode::Network,
                "connect refused"),
        });
    const auto terminal = "data: " + terminal_frame("response.completed", "completed") + "\n\n";
    harness.http->attempts = {
        TransportAttempt{.chunks = {terminal}},
        TransportAttempt{.chunks = {terminal}},
    };

    const auto model = codex_model();
    ai::SimpleStreamOptions first_options;
    first_options.api_key = std::string{kCodexToken};
    first_options.session_id = "session-sse-only";
    auto first_run = run_codex(*harness.models, model, {}, std::move(first_options));
    REQUIRE(first_run.result);
    CHECK(first_run.result->stop_reason == ai::AssistantStopReason::Stop);

    ai::SimpleStreamOptions second_options;
    second_options.api_key = std::string{kCodexToken};
    second_options.session_id = "session-sse-only";
    auto second_run = run_codex(*harness.models, model, {}, std::move(second_options));
    REQUIRE(second_run.result);
    CHECK(second_run.result->stop_reason == ai::AssistantStopReason::Stop);

    CHECK(harness.ws->requests.size() == 1);
    CHECK(harness.ws->sockets.empty());
    CHECK(harness.http->requests.size() == 2);
}

TEST_CASE(
    "Codex reuses session sockets and sends previous_response_id input deltas",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->on_send = [](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames.push_back(simple_terminal());
    };
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});

    const auto model = codex_model();
    ai::SimpleStreamOptions first_options;
    first_options.api_key = std::string{kCodexToken};
    first_options.session_id = "session-1";
    auto first_run = run_codex(
        *harness.models, model,
        user_context("Use the tool"), std::move(first_options));
    REQUIRE(first_run.result);
    CHECK(first_run.result->stop_reason == ai::AssistantStopReason::Stop);

    ai::AiContext second_context;
    second_context.messages.push_back(ai::UserMessage{
        .content = {ai::text_content("Use the tool")},
        .timestamp = 1,
    });
    second_context.messages.push_back(*first_run.result);
    second_context.messages.push_back(ai::UserMessage{
        .content = {ai::text_content("Now finish")},
        .timestamp = 2,
    });
    ai::SimpleStreamOptions second_options;
    second_options.api_key = std::string{kCodexToken};
    second_options.session_id = "session-1";
    auto second_run = run_codex(
        *harness.models, model, std::move(second_context), std::move(second_options));

    REQUIRE(second_run.result);
    CHECK(second_run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(harness.ws->requests.size() == 1);
    REQUIRE(session->sent_frames.size() == 2);

    const auto first_body = util::read_json<util::JsonValue>(session->sent_frames[0]);
    REQUIRE(first_body);
    CHECK_FALSE(first_body->get_object().contains("previous_response_id"));
    REQUIRE(first_body->at("input").get_array().size() == 1);
    CHECK(first_body->at("prompt_cache_key").get_string() == "session-1");
    CHECK_FALSE(first_body->at("store").get_boolean());

    const auto delta_body = util::read_json<util::JsonValue>(session->sent_frames[1]);
    REQUIRE(delta_body);
    CHECK(delta_body->at("previous_response_id").get_string() == "resp_terminal");
    CHECK_FALSE(delta_body->at("store").get_boolean());
    CHECK(delta_body->at("prompt_cache_key").get_string() == "session-1");
    const auto& delta_input = delta_body->at("input").get_array();
    REQUIRE(delta_input.size() == 1);
    CHECK(delta_input[0].at("role").get_string() == "user");
    CHECK(delta_input[0].at("content").get_array()[0].at("text").get_string() == "Now finish");
}

TEST_CASE(
    "Codex opens a fresh socket after the idle close window",
    "[ai][provider][codex][issue342]") {
    ai::api::CodexWebSocketCacheConfig config;
    config.idle_close = std::chrono::milliseconds{0};
    auto harness = make_codex_harness(codex_model(), config);
    const auto make_session = [] {
        auto session = std::make_shared<ScriptedWebSocket::Session>();
        session->on_send = [](ScriptedWebSocket& socket, std::string_view) {
            socket.session()->frames.push_back(simple_terminal());
        };
        return session;
    };
    harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
    };

    const auto model = codex_model();
    for (int index = 0; index < 2; ++index) {
        ai::SimpleStreamOptions options;
        options.api_key = std::string{kCodexToken};
        options.session_id = "aged-ws-session";
        auto run = run_codex(
            *harness.models, model,
            user_context("Say hello"), std::move(options));
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    }
    CHECK(harness.ws->requests.size() == 2);
    CHECK(harness.ws->sockets.size() == 2);

    ai::api::CodexWebSocketCacheConfig age_config;
    age_config.max_age = std::chrono::milliseconds{0};
    auto age_harness = make_codex_harness(codex_model(), age_config);
    age_harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
    };
    for (int index = 0; index < 2; ++index) {
        ai::SimpleStreamOptions options;
        options.api_key = std::string{kCodexToken};
        options.session_id = "aged-ws-session";
        auto run = run_codex(
            *age_harness.models, model,
            user_context("Say hello"), std::move(options));
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    }
    CHECK(age_harness.ws->requests.size() == 2);
}

TEST_CASE(
    "Codex closes one-shot sockets when cacheRetention is none",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    const auto make_session = [] {
        auto session = std::make_shared<ScriptedWebSocket::Session>();
        session->on_send = [](ScriptedWebSocket& socket, std::string_view) {
            socket.session()->frames.push_back(simple_terminal());
        };
        return session;
    };
    harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
    };

    const auto model = codex_model();
    for (int index = 0; index < 2; ++index) {
        ai::SimpleStreamOptions options;
        options.api_key = std::string{kCodexToken};
        options.session_id = "one-off-summary";
        options.cache_retention = ai::CacheRetention::None;
        auto run = run_codex(
            *harness.models, model,
            user_context("Say hello"), std::move(options));
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    }
    CHECK(harness.ws->requests.size() == 2);
    REQUIRE(harness.ws->sockets.size() == 2);
    for (const auto& socket : harness.ws->sockets) {
        CHECK(socket->session()->close_count == 1);
        REQUIRE(socket->session()->sent_frames.size() == 1);
        const auto body = util::read_json<util::JsonValue>(socket->session()->sent_frames[0]);
        REQUIRE(body);
        CHECK_FALSE(body->get_object().contains("prompt_cache_key"));
    }
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex WebSocket termination matrix maps statuses",
    "[ai][provider][codex][issue342]") {
    struct Case {
        std::string frame;
        ai::AssistantStopReason expected;
    };
    const std::vector<Case> cases{
        {terminal_frame("response.completed", "completed"), ai::AssistantStopReason::Stop},
        {terminal_frame("response.done", "completed"), ai::AssistantStopReason::Stop},
        {terminal_frame("response.incomplete", "incomplete"), ai::AssistantStopReason::Length},
        {terminal_frame("response.completed", "failed"), ai::AssistantStopReason::Error},
        {terminal_frame("response.completed", "queued"), ai::AssistantStopReason::Stop},
        {"{\"type\":\"response.completed\"}", ai::AssistantStopReason::Stop},
    };

    for (const auto& test_case : cases) {
        auto harness = make_codex_harness(codex_model());
        auto session = std::make_shared<ScriptedWebSocket::Session>();
        session->frames.push_back(test_case.frame);
        harness.ws->connect_scripts.push_back(
            ScriptedWebSocketTransport::ConnectScript{.session = session});

        ai::SimpleStreamOptions options;
        options.api_key = std::string{kCodexToken};
        auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == test_case.expected);
        REQUIRE_FALSE(run.events.empty());
        if (test_case.expected == ai::AssistantStopReason::Error) {
            CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
        } else {
            CHECK(std::holds_alternative<ai::AssistantDoneEvent>(run.events.back()));
        }
        CHECK(harness.http->requests.empty());
    }

    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->frames.push_back(
        "{\"type\":\"response.output_item.added\",\"output_index\":0,"
        "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\","
        "\"status\":\"in_progress\",\"content\":[]}}");
    session->closed = true;
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});
    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));
    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("before response.completed") != std::string::npos);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex cancellation closes the socket and yields one aborted terminal",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    std::stop_source stop;
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});
    session->on_send = [&stop](ScriptedWebSocket& socket, std::string_view) {
        socket.session()->frames.push_back(terminal_frame("response.completed", "completed"));
        stop.request_stop();
    };

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.stop_token = stop.get_token();
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    const std::vector<std::string> expected_events{"error"};
    CHECK(event_names(run.events) == expected_events);
    const auto* terminal = std::get_if<ai::AssistantErrorEvent>(&run.events.front());
    REQUIRE(terminal);
    CHECK(terminal->reason == ai::AssistantStopReason::Aborted);
    CHECK(session->close_count >= 1);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex rejects tokens without a chatgpt_account_id claim",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    ai::SimpleStreamOptions options;
    options.api_key = "not-a-jwt-token";
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("Failed to extract accountId") != std::string::npos);
    CHECK(harness.ws->requests.empty());
    CHECK(harness.http->requests.empty());
    const std::vector<std::string> expected_events{"error"};
    CHECK(event_names(run.events) == expected_events);
}

TEST_CASE(
    "Codex SSE retries per the shared policy and never retries quota failures",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{
            .failure = util::make_error(
                util::ErrorCode::Network,
                "connect refused"),
        });
    const auto terminal = "data: " + terminal_frame("response.completed", "completed") + "\n\n";
    harness.http->attempts = {
        TransportAttempt{
            .head = ai::providers::StreamResponseHead{
                .status_code = 429,
                .headers = {{"retry-after-ms", "0"}},
            },
            .chunks = {"temporarily unavailable"},
        },
        TransportAttempt{.chunks = {terminal}},
    };

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.max_retries = 1;
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(harness.http->requests.size() == 2);

    auto quota_harness = make_codex_harness(codex_model());
    quota_harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{
            .failure = util::make_error(
                util::ErrorCode::Network,
                "connect refused"),
        });
    quota_harness.http->attempts.push_back(TransportAttempt{
        .head = ai::providers::StreamResponseHead{
            .status_code = 429,
            .headers = {{"retry-after-ms", "0"}},
        },
        .chunks = {"insufficient quota"},
    });
    ai::SimpleStreamOptions quota_options;
    quota_options.api_key = std::string{kCodexToken};
    quota_options.max_retries = 3;
    auto quota = run_codex(
        *quota_harness.models, codex_model(), {}, std::move(quota_options));
    REQUIRE(quota.result);
    CHECK(quota.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(quota_harness.http->requests.size() == 1);
}

TEST_CASE(
    "Codex API errors never fall back to SSE",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->frames.push_back(error_frame("server_error", "upstream exploded"));
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("upstream exploded") != std::string::npos);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex scopes cached sockets to the authenticated account",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto make_session = [] {
        auto session = std::make_shared<ScriptedWebSocket::Session>();
        session->on_send = [](ScriptedWebSocket& socket, std::string_view) {
            socket.session()->frames.push_back(simple_terminal());
        };
        return session;
    };
    harness.ws->connect_scripts = {
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
        ScriptedWebSocketTransport::ConnectScript{.session = make_session()},
    };

    const auto model = codex_model();
    // Two tokens for two accounts sharing one session must not reuse sockets.
    const std::string token_a =
        "aaa.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjX2EifX0=.bbb";
    const std::string token_b =
        "aaa.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjX2IifX0=.bbb";

    for (const auto& token : {token_a, token_b}) {
        ai::SimpleStreamOptions options;
        options.api_key = token;
        options.session_id = "shared-session";
        auto run = run_codex(
            *harness.models, model,
            user_context("Say hello"), std::move(options));
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    }
    CHECK(harness.ws->requests.size() == 2);
    REQUIRE(harness.ws->sockets.size() == 2);
    CHECK(harness.ws->requests[0].headers.at("chatgpt-account-id") == "acc_a");
    CHECK(harness.ws->requests[1].headers.at("chatgpt-account-id") == "acc_b");
}

TEST_CASE(
    "Codex preserves post-merge transformed headers on the SSE fallback",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{
            .failure = util::make_error(
                util::ErrorCode::Network,
                "connect refused"),
        });
    const auto terminal = "data: " + terminal_frame("response.completed", "completed") + "\n\n";
    harness.http->attempts.push_back(TransportAttempt{.chunks = {terminal}});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    options.transform_headers = [](ai::RequestHeaders headers)
        -> util::Expected<ai::RequestHeaders> {
        headers.insert_or_assign("Accept", "application/x-test-sse");
        headers.insert_or_assign("Content-Type", "application/x-test-json");
        headers.insert_or_assign("x-custom", "custom-value");
        return headers;
    };
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(harness.http->requests.size() == 1);
    const auto& headers = harness.http->requests.front().headers;
    CHECK(headers.at("Accept") == "application/x-test-sse");
    CHECK(headers.at("Content-Type") == "application/x-test-json");
    CHECK(headers.at("x-custom") == "custom-value");
    CHECK(headers.at("Authorization") == "Bearer " + std::string{kCodexToken});
    CHECK(headers.at("chatgpt-account-id") == kCodexAccountId);
}

TEST_CASE(
    "Codex protocol errors never fall back to SSE",
    "[ai][provider][codex][issue342]") {
    auto harness = make_codex_harness(codex_model());
    auto session = std::make_shared<ScriptedWebSocket::Session>();
    session->frames.push_back("this is not json");
    harness.ws->connect_scripts.push_back(
        ScriptedWebSocketTransport::ConnectScript{.session = session});

    ai::SimpleStreamOptions options;
    options.api_key = std::string{kCodexToken};
    auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->find("Invalid Codex WebSocket JSON") != std::string::npos);
    CHECK(harness.http->requests.empty());
}

TEST_CASE(
    "Codex SSE terminal matrix maps statuses and treats DONE as non-terminal",
    "[ai][provider][codex][issue342]") {
    struct Case {
        std::string sse;
        ai::AssistantStopReason expected;
    };
    const std::vector<Case> cases{
        {"data: " + terminal_frame("response.completed", "completed") + "\n\n",
         ai::AssistantStopReason::Stop},
        {"data: " + terminal_frame("response.incomplete", "incomplete") + "\n\n",
         ai::AssistantStopReason::Length},
        {"data: " + terminal_frame("response.completed", "failed") + "\n\n",
         ai::AssistantStopReason::Error},
        {"data: [DONE]\n\n", ai::AssistantStopReason::Error},
        {"data: {\"type\":\"response.created\",\"response\":{\"id\":\"early\"}}\n\n",
         ai::AssistantStopReason::Error},
    };

    for (const auto& test_case : cases) {
        auto harness = make_codex_harness(codex_model());
        harness.ws->connect_scripts.push_back(
            ScriptedWebSocketTransport::ConnectScript{
                .failure = util::make_error(
                    util::ErrorCode::Network,
                    "connect refused"),
            });
        harness.http->attempts.push_back(TransportAttempt{.chunks = {test_case.sse}});

        ai::SimpleStreamOptions options;
        options.api_key = std::string{kCodexToken};
        auto run = run_codex(*harness.models, codex_model(), {}, std::move(options));

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == test_case.expected);
        REQUIRE_FALSE(run.events.empty());
        if (test_case.expected == ai::AssistantStopReason::Error) {
            CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
        } else {
            CHECK(std::holds_alternative<ai::AssistantDoneEvent>(run.events.back()));
        }
    }
}
