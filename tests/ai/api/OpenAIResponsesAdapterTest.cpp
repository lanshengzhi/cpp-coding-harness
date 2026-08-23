#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include "ai/providers/StreamTransport.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "ai/providers/EnvApiKeyAuth.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiEventSnapshot.hpp"
#include "support/PiFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
using tests::partial_stop_reasons;
using tests::run_async_result;
using tests::run_awaitable;
using tests::ScriptedTransport;
using tests::TransportAttempt;

struct RunResult {
    support::Expected<ai::AssistantMessage> result;
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

[[nodiscard]] ai::Model deepseek_model() {
    auto model = tests::make_model(
        "deepseek-v4-flash", "deepseek", "openai-responses");
    model.base_url = "https://api.deepseek.example/v1";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
        {ai::ModelThinkingLevel::High, "max"},
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

[[nodiscard]] std::shared_ptr<ai::Models> make_models(
    const std::shared_ptr<ScriptedTransport>& transport,
    const ai::Model& model) {
    auto models = std::make_shared<ai::Models>(
        std::make_shared<EmptyCredentialStore>(),
        std::make_shared<EmptyAuthContext>());
    auto provider = ai::providers::make_composed_provider(
        "deepseek", "deepseek", {model},
        ai::providers::make_env_api_key_auth("API key", {}), transport);
    if (auto registered = models->set_provider(std::move(provider)); !registered) {
        return nullptr;
    }
    return models;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = std::vector<ai::Content>{
            ai::text_content("hi"),
            ai::image_content("YWJj", "image/png"),
        },
        .timestamp = 1,
    });
    context.tools.push_back(ai::Tool{
        .name = "lookup",
        .description = "Look up a value",
        .parameters = support::JsonValue::object_t{
            {"properties", support::JsonValue::object_t{
                {"q", support::JsonValue::object_t{{"type", "string"}}},
            }},
            {"required", support::JsonValue::array_t{"q"}},
            {"type", "object"},
        },
    });
    return context;
}

[[nodiscard]] ai::Tool lookup_tool() {
    return ai::Tool{
        .name = "lookup",
        .description = "Look up a value",
        .parameters = support::JsonValue::object_t{
            {"properties", support::JsonValue::object_t{
                {"q", support::JsonValue::object_t{{"type", "string"}}},
            }},
            {"required", support::JsonValue::array_t{"q"}},
            {"type", "object"},
        },
    };
}

[[nodiscard]] ai::AiContext string_content_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = std::vector<ai::Content>{},
        .timestamp = 1,
    });
    context.messages.push_back(ai::UserMessage{
        .content = std::string{"hello"},
        .timestamp = 2,
    });
    context.tools.push_back(lookup_tool());
    return context;
}

[[nodiscard]] ai::AiContext empty_string_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = std::string{""},
        .timestamp = 1,
    });
    context.tools.push_back(lookup_tool());
    return context;
}

[[nodiscard]] RunResult run_models(
    ai::Models& models,
    const ai::Model& model,
    ai::AiContext context,
    ai::SimpleStreamOptions options) {
    std::vector<ai::AssistantStreamEvent> events;
    auto stream = models.stream(model, std::move(context), std::move(options));
    auto result = run_async_result(
        std::move(stream).run(
        [&events](const ai::AssistantStreamEvent& event) -> support::ExpectedVoid {
            events.push_back(event);
            return {};
        }));
    return RunResult{.result = std::move(result), .events = std::move(events)};
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

[[nodiscard]] std::string terminal_sse(
    std::string type,
    std::string status,
    std::string extra = {}) {
    return "data: {\"type\":\"" + std::move(type) +
           "\",\"response\":{\"id\":\"resp_terminal\",\"status\":\"" +
           std::move(status) + "\"" + std::move(extra) + "}}\n\n";
}

} // namespace

TEST_CASE(
    "DeepSeek Responses streams the frozen request and SSE sequence through Models",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse = read_fixture_text("wire/openai-responses-deepseek.sse");
    REQUIRE_FALSE(sse.empty());
    const auto split = sse.size() / 2;
    transport->attempts.push_back(TransportAttempt{
        .chunks = {sse.substr(0, split), sse.substr(split)},
    });
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;
    auto run = run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(run.result->response_id == "resp_deepseek");
    CHECK(run.result->usage.input == 100);
    CHECK(run.result->usage.output == 30);
    CHECK(run.result->usage.cache_read == 20);
    CHECK(run.result->usage.cache_write == 0);
    CHECK(run.result->usage.reasoning == 7);
    CHECK(run.result->usage.total_tokens == 150);
    CHECK(run.result->usage.cost.input == 0.0002);
    CHECK(run.result->usage.cost.output == 0.00012);
    CHECK(run.result->usage.cost.cache_read == 0.00002);
    CHECK(run.result->usage.cost.total == 0.00034);
    REQUIRE(run.result->content.size() == 3);
    CHECK(std::get<ai::ThinkingContent>(run.result->content[0]).thinking == "plan");
    CHECK(std::get<ai::TextContent>(run.result->content[1]).text == "Hello");
    const auto& tool = std::get<ai::ToolCallContent>(run.result->content[2]);
    CHECK(tool.id == "call_1|fc_1");
    CHECK(tool.name == "lookup");
    CHECK(tool.arguments_valid);
    REQUIRE(tool.arguments);
    CHECK(tool.arguments->at("q").get_string() == "x");
    CHECK_FALSE(tests::pi_event_snapshot_mismatch(
        run.events,
        "wire/openai-responses-deepseek-ts-events.json"));

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.deepseek.example/v1/responses");
    CHECK(request.timeout == std::chrono::milliseconds{4321});
    CHECK(request.headers.at("Authorization") == "Bearer dummy-deepseek-key");
    CHECK(request.headers.at("session_id") == "session-1");
    CHECK(request.headers.at("x-client-request-id") == "session-1");
    CHECK_FALSE(request.body.contains("previous_response_id"));
    CHECK_FALSE(request.body.contains("input_image"));
    CHECK_FALSE(request.body.contains("strict"));

    auto expected_request_bytes = read_fixture_text(
        "wire/openai-responses-deepseek-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(request.body == expected_request_bytes);
    REQUIRE(support::read_json(request.body));
}

TEST_CASE(
    "DeepSeek Responses emits a string user message as one input_text and omits an empty block array",
    "[ai][provider][responses][issue366]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse = read_fixture_text("wire/openai-responses-deepseek.sse");
    REQUIRE_FALSE(sse.empty());
    transport->attempts.push_back(TransportAttempt{.chunks = {sse}});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;
    auto run = run_models(*models, model, string_content_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK_FALSE(tests::pi_event_snapshot_mismatch(
        run.events,
        "wire/openai-responses-deepseek-ts-events.json"));

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    auto expected_request_bytes = read_fixture_text(
        "wire/openai-responses-deepseek-string-content-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(request.body == expected_request_bytes);
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    const auto& input = body->at("input").get_array();
    // The empty block-array message is omitted; the string alternative becomes
    // exactly one sanitized input_text item (pi `openai-responses-shared.ts:185-209`).
    REQUIRE(input.size() == 2);
    CHECK(input[0].at("role").get_string() == "developer");
    CHECK(input[1].at("role").get_string() == "user");
    const auto& content = input[1].at("content").get_array();
    REQUIRE(content.size() == 1);
    CHECK(content[0].at("type").get_string() == "input_text");
    CHECK(content[0].at("text").get_string() == "hello");
}

TEST_CASE(
    "DeepSeek Responses emits an empty string user message as one input_text",
    "[ai][provider][responses][issue366]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse = read_fixture_text("wire/openai-responses-deepseek.sse");
    REQUIRE_FALSE(sse.empty());
    transport->attempts.push_back(TransportAttempt{.chunks = {sse}});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;
    auto run = run_models(*models, model, empty_string_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK_FALSE(tests::pi_event_snapshot_mismatch(
        run.events,
        "wire/openai-responses-deepseek-ts-events.json"));

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    auto expected_request_bytes = read_fixture_text(
        "wire/openai-responses-deepseek-empty-string-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(request.body == expected_request_bytes);
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    const auto& input = body->at("input").get_array();
    // The empty string is still emitted as exactly one input_text item.
    REQUIRE(input.size() == 2);
    const auto& content = input[1].at("content").get_array();
    REQUIRE(content.size() == 1);
    CHECK(content[0].at("type").get_string() == "input_text");
    CHECK(content[0].at("text").get_string() == "");
}

TEST_CASE(
    "DeepSeek Responses preserves post-merge transformed headers",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{
        .chunks = {terminal_sse("response.completed", "completed")},
    });
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.transform_headers = [](ai::RequestHeaders headers)
        -> support::Expected<ai::RequestHeaders> {
        headers.insert_or_assign("Authorization", "Custom dummy-auth");
        headers.insert_or_assign("Accept", "application/x-test-sse");
        headers.insert_or_assign("Content-Type", "application/x-test-json");
        return headers;
    };

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& headers = transport->requests.front().headers;
    CHECK(headers.at("Authorization") == "Custom dummy-auth");
    CHECK(headers.at("Accept") == "application/x-test-sse");
    CHECK(headers.at("Content-Type") == "application/x-test-json");

    auto deletion_transport = std::make_shared<ScriptedTransport>();
    deletion_transport->attempts.push_back(TransportAttempt{
        .chunks = {terminal_sse("response.completed", "completed")},
    });
    auto deletion_models = make_models(deletion_transport, model);
    REQUIRE(deletion_models);
    ai::SimpleStreamOptions deletion_options;
    deletion_options.api_key = "dummy-key";
    deletion_options.transform_headers = [](ai::RequestHeaders transformed)
        -> support::Expected<ai::RequestHeaders> {
        transformed.insert_or_assign("Authorization", std::nullopt);
        transformed.insert_or_assign("Accept", std::nullopt);
        transformed.insert_or_assign("Content-Type", std::nullopt);
        return transformed;
    };

    auto deleted = run_models(
        *deletion_models, model, {}, std::move(deletion_options));

    REQUIRE(deleted.result);
    REQUIRE(deletion_transport->requests.size() == 1);
    const auto& deleted_headers = deletion_transport->requests.front().headers;
    CHECK_FALSE(deleted_headers.contains("Authorization"));
    CHECK_FALSE(deleted_headers.contains("Accept"));
    CHECK_FALSE(deleted_headers.contains("Content-Type"));
}

TEST_CASE("DeepSeek Responses partials start pending and flip to stop at final_answer",
        "[ai][provider][responses][issue374][issue370][issue536]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_pending\"}}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
        "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\","
        "\"status\":\"in_progress\",\"content\":[],\"phase\":\"final_answer\"}}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"delta\":\"Hi\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
        "\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"status\":\"completed\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"Hi\",\"annotations\":[]}],\"phase\":\"final_answer\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_pending\","
        "\"status\":\"completed\",\"usage\":{\"input_tokens\":5,\"output_tokens\":1,\"total_tokens\":6}}}\n\n",
    }});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    // The Responses family records the raw terminal status as rawStopReason
    // (pi's finalizeResponse), matching the strengthened event snapshot.
    REQUIRE(run.result->raw_stop_reason);
    CHECK(*run.result->raw_stop_reason == "completed");
    const std::vector<std::string> expected{
        "start", "text_start", "text_delta", "text_end", "done"};
    CHECK(event_names(run.events) == expected);
    // pi's `applyMessagePhaseStopReason`: partials are constructed `pending`
    // and flip to `stop` once a message item with `phase: "final_answer"`
    // arrives (openai-responses-shared.ts).
    const std::vector<ai::AssistantStopReason> expected_partials{
        ai::AssistantStopReason::Pending,
        ai::AssistantStopReason::Stop,
        ai::AssistantStopReason::Stop,
        ai::AssistantStopReason::Stop,
    };
    CHECK(partial_stop_reasons(run.events) == expected_partials);
}

TEST_CASE("DeepSeek Responses stream ending without a terminal event is a terminal error",
        "[ai][provider][responses][issue374][issue375][issue536]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse =
        read_fixture_text("wire/openai-responses-deepseek-no-terminal.sse");
    REQUIRE_FALSE(sse.empty());
    transport->attempts.push_back(TransportAttempt{.chunks = {sse}});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.temperature = 0.2;
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "session-1";
    options.cache_retention = ai::CacheRetention::Long;
    options.timeout_ms = 4321;

    auto run = run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    // pi observably surfaces the shared processor guard
    // (openai-responses-shared.ts) for a stream ending without a terminal
    // response event; the wrapper's defensive pending check
    // (openai-responses.ts) is unreachable in pi and not mirrored.
    CHECK(*run.result->error_message ==
        "OpenAI Responses stream ended before a terminal response event");
    REQUIRE_FALSE(run.events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    const auto& terminal = std::get<ai::AssistantErrorEvent>(run.events.back());
    CHECK(terminal.reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error.error_message);
    CHECK(*terminal.error.error_message == *run.result->error_message);
    CHECK_FALSE(tests::pi_event_snapshot_mismatch(
        run.events,
        "wire/openai-responses-deepseek-no-terminal-ts-events.json"));

    REQUIRE(transport->requests.size() == 1);
    auto expected_request_bytes = read_fixture_text(
        "wire/openai-responses-deepseek-ts-request.json");
    REQUIRE_FALSE(expected_request_bytes.empty());
    if (expected_request_bytes.back() == '\n') {
        expected_request_bytes.pop_back();
    }
    CHECK(transport->requests.front().body == expected_request_bytes);
}

TEST_CASE(
    "DeepSeek Responses termination matrix does not treat DONE as terminal",
    "[ai][provider][responses][issue340]") {
    struct Case {
        std::string sse;
        ai::AssistantStopReason expected;
    };
    const std::vector<Case> cases{
        {terminal_sse("response.completed", "completed"), ai::AssistantStopReason::Stop},
        {terminal_sse("response.done", "completed"), ai::AssistantStopReason::Stop},
        {terminal_sse("response.incomplete", "incomplete"), ai::AssistantStopReason::Length},
        {terminal_sse(
             "response.failed",
             "failed",
             ",\"error\":{\"code\":\"server_error\",\"message\":\"boom\"}"),
         ai::AssistantStopReason::Error},
        {"data: [DONE]\n\n", ai::AssistantStopReason::Error},
        {"data: {\"type\":\"response.created\",\"response\":{\"id\":\"early\"}}\n\n",
         ai::AssistantStopReason::Error},
    };

    for (const auto& test_case : cases) {
        auto transport = std::make_shared<ScriptedTransport>();
        transport->attempts.push_back(TransportAttempt{.chunks = {test_case.sse}});
        const auto model = deepseek_model();
        auto models = make_models(transport, model);
        REQUIRE(models);
        ai::SimpleStreamOptions options;
        options.api_key = "dummy-key";

        auto run = run_models(*models, model, {}, std::move(options));

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

TEST_CASE(
    "DeepSeek Responses surfaces SSE error data and ignores unknown events",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: future.event\ndata: {\"ignored\":true}\n\n"
        "event: error\ndata: deepseek exploded\n\n",
    }});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->contains("deepseek exploded"));
    const std::vector<std::string> expected_events{"start", "error"};
    CHECK(event_names(run.events) == expected_events);
}

TEST_CASE(
    "DeepSeek Responses removes parser scratch state from partial failures",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
        "\"item\":{\"type\":\"function_call\",\"id\":\"fc_partial\","
        "\"call_id\":\"call_partial\",\"name\":\"lookup\",\"arguments\":\"\"}}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
        "\"output_index\":0,\"delta\":\"{\\\"q\\\":\"}\n\n"
        "event: error\ndata: partial failure\n\n",
    }});
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->content.size() == 1);
    const auto* tool = std::get_if<ai::ToolCallContent>(&run.result->content.front());
    REQUIRE(tool);
    CHECK(tool->raw_arguments == "{\"q\":");
    // pi's `parseStreamingJson` (partial-json) drops the trailing key with no
    // value instead of completing it to `null`; a partial `{"q":` parses to {}.
    REQUIRE(tool->arguments);
    CHECK(tool->arguments->get_object().empty());
    CHECK(tool->arguments_valid);
    CHECK_FALSE(tool->argument_error);
    const std::vector<std::string> expected_events{
        "start", "toolcall_start", "toolcall_delta", "error"};
    CHECK(event_names(run.events) == expected_events);
    const auto* delta = std::get_if<ai::ToolCallDeltaEvent>(&run.events[2]);
    REQUIRE(delta);
    const auto& partial_tool = std::get<ai::ToolCallContent>(
        delta->partial.content.front());
    REQUIRE(partial_tool.arguments);
    CHECK(partial_tool.arguments->get_object().empty());
}

TEST_CASE(
    "DeepSeek Responses retries only eligible setup failures",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts = {
        TransportAttempt{
            .head = ai::providers::StreamResponseHead{
                .status_code = 429,
                .headers = {{"retry-after-ms", "0"}},
            },
            .chunks = {"temporarily unavailable"},
        },
        TransportAttempt{
            .chunks = {terminal_sse("response.completed", "completed")},
        },
    };
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.max_retries = 1;

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(transport->requests.size() == 2);

    auto network_transport = std::make_shared<ScriptedTransport>();
    network_transport->attempts = {
        TransportAttempt{
            .head = {},
            .chunks = {},
            .failure = support::make_error(
                support::ErrorCode::Network,
                "connection reset"),
        },
        TransportAttempt{
            .chunks = {terminal_sse("response.completed", "completed")},
        },
    };
    auto network_models = make_models(network_transport, model);
    REQUIRE(network_models);
    ai::SimpleStreamOptions network_options;
    network_options.api_key = "dummy-key";
    network_options.max_retries = 1;

    auto network = run_models(
        *network_models, model, {}, std::move(network_options));

    REQUIRE(network.result);
    CHECK(network.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(network_transport->requests.size() == 2);

    auto terminal_transport = std::make_shared<ScriptedTransport>();
    terminal_transport->attempts.push_back(TransportAttempt{
        .head = ai::providers::StreamResponseHead{
            .status_code = 429,
            .headers = {{"retry-after-ms", "0"}},
        },
        .chunks = {"insufficient quota"},
    });
    auto terminal_models = make_models(terminal_transport, model);
    REQUIRE(terminal_models);
    ai::SimpleStreamOptions terminal_options;
    terminal_options.api_key = "dummy-key";
    terminal_options.max_retries = 3;

    auto terminal = run_models(
        *terminal_models, model, {}, std::move(terminal_options));

    REQUIRE(terminal.result);
    CHECK(terminal.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(terminal_transport->requests.size() == 1);
}

TEST_CASE(
    "DeepSeek Responses cancellation yields one aborted terminal",
    "[ai][provider][responses][issue340]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{
        .head = {},
        .chunks = {},
        .failure = support::make_error(
            support::ErrorCode::Cancelled,
            "transport cancelled"),
    });
    const auto model = deepseek_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    std::stop_source stop;
    transport->on_request = [&stop] {
        stop.request_stop();
    };
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.stop_token = stop.get_token();

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    const std::vector<std::string> expected_events{"error"};
    CHECK(event_names(run.events) == expected_events);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().stop_token.stop_possible());
    CHECK(transport->requests.front().stop_token.stop_requested());
    const auto* terminal = std::get_if<ai::AssistantErrorEvent>(&run.events.front());
    REQUIRE(terminal);
    CHECK(terminal->reason == ai::AssistantStopReason::Aborted);
}
