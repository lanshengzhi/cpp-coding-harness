#include <cch/ai/Models.hpp>
#include <cch/ai/Provider.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include "ai/providers/KimiCatalog.hpp"
#include "ai/providers/KimiProvider.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiEventSnapshot.hpp"
#include "support/PiFixture.hpp"
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
using tests::partial_stop_reasons;
using tests::run_awaitable;
using tests::ScriptedTransport;
using tests::TransportAttempt;

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

[[nodiscard]] ai::Model kimi_model(std::string_view id = "kimi-for-coding") {
    for (auto model : ai::providers::kimi_coding_models()) {
        if (model.id == id) {
            return model;
        }
    }
    return {};
}

[[nodiscard]] ai::ProviderAuth header_auth() {
    ai::ApiKeyAuth api_key;
    api_key.name = "Kimi OAuth";
    api_key.resolve = [](
        const ai::AuthContext&,
        std::optional<ai::ApiKeyCredential>)
        -> boost::asio::awaitable<util::Expected<std::optional<ai::AuthResult>>> {
        co_return ai::AuthResult{
            .auth = ai::ModelAuth{
                .api_key = std::nullopt,
                .headers = {{"Authorization", "Bearer dummy-kimi-oauth"}},
                .base_url = std::nullopt,
            },
            .env = {},
            .source = "Kimi OAuth",
        };
    };
    return ai::ProviderAuth{.api_key = std::move(api_key)};
}

[[nodiscard]] std::unique_ptr<ai::Models> make_models(
    const std::shared_ptr<ScriptedTransport>& transport,
    const ai::Model&) {
    auto models = std::make_unique<ai::Models>(
        std::make_shared<EmptyCredentialStore>(),
        std::make_shared<EmptyAuthContext>());
    auto provider = ai::providers::make_kimi_coding_provider(
        header_auth(), transport);
    if (auto registered = models->set_provider(std::move(provider)); !registered) {
        return nullptr;
    }
    return models;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::UserMessage{
        .content = std::vector<ai::Content>{ai::image_content("YWJj", "image/png")},
        .timestamp = 1,
    });
    ai::AssistantMessage assistant;
    assistant.api = "anthropic-messages";
    assistant.provider = "kimi-coding";
    assistant.model = "kimi-for-coding";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 2;
    assistant.content = {
        ai::ThinkingContent{.thinking = "thought", .thinking_signature = ""},
        ai::ThinkingContent{
            .thinking = "[Reasoning redacted]",
            .thinking_signature = "dummy-redacted",
            .redacted = true,
        },
        ai::TextContent{.text = "answer", .text_signature = std::nullopt},
    };
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(ai::user_text_message("next", 3));
    ai::AssistantMessage tool_assistant;
    tool_assistant.api = "openai-responses";
    tool_assistant.provider = "deepseek";
    tool_assistant.model = "deepseek-v4-flash";
    tool_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    tool_assistant.timestamp = 4;
    tool_assistant.content.push_back(ai::ToolCallContent{
        .id = "bad id!",
        .name = "lookup",
        .arguments = util::JsonValue::object_t{{"q", "x"}},
        .raw_arguments = "{\"q\":\"x\"}",
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    });
    context.messages.push_back(std::move(tool_assistant));
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "bad id!",
        .tool_name = "lookup",
        .content = {ai::text_content("failed")},
        .details = std::nullopt,
        .is_error = true,
        .timestamp = 5,
    });
    context.messages.push_back(ai::ToolResultMessage{
        .tool_call_id = "second",
        .tool_name = "lookup",
        .content = {ai::image_content("ZGVm", "image/png")},
        .details = std::nullopt,
        .is_error = false,
        .timestamp = 6,
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

[[nodiscard]] RunResult run_models(
    ai::Models& models,
    const ai::Model& model,
    ai::AiContext context,
    ai::SimpleStreamOptions options) {
    std::vector<ai::AssistantStreamEvent> events;
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
    std::vector<std::string> names;
    for (const auto& event : events) {
        names.push_back(event_name(event));
    }
    return names;
}

[[nodiscard]] std::string terminal_sse(
    std::string stop_reason,
    std::string stop_details = {},
    bool include_message_stop = true) {
    std::string result =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_terminal\","
        "\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" +
        std::move(stop_reason) + "\"" + std::move(stop_details) +
        "},\"usage\":{\"output_tokens\":1}}\n\n";
    if (include_message_stop) {
        result += "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    }
    return result;
}

[[nodiscard]] ai::SimpleStreamOptions authorized_options() {
    return {};
}

} // namespace

TEST_CASE(
    "Kimi catalog carries the frozen Anthropic Messages compat values",
    "[ai][provider][anthropic][issue341]") {
    const auto models = ai::providers::kimi_coding_models();

    REQUIRE(models.size() == 4);
    CHECK(models[0].id == "k3");
    REQUIRE(models[0].compat);
    CHECK(models[0].compat->force_adaptive_thinking == true);
    CHECK(models[0].compat->allow_empty_signature == true);
    REQUIRE(models[0].thinking_level_map);
    CHECK(models[0].thinking_level_map->at(ai::ModelThinkingLevel::Off) == std::nullopt);
    CHECK(models[1].id == "k3-256k");
    REQUIRE(models[1].compat);
    CHECK(models[1].compat->force_adaptive_thinking == true);
    CHECK(models[1].compat->allow_empty_signature == std::nullopt);
    CHECK(models[2].id == "kimi-for-coding");
    REQUIRE(models[2].compat);
    CHECK(models[2].compat->force_adaptive_thinking == true);
    CHECK(models[2].compat->allow_empty_signature == true);
    CHECK(models[3].id == "kimi-for-coding-highspeed");
    REQUIRE(models[3].compat);
    CHECK(models[3].compat->force_adaptive_thinking == true);
    CHECK(models[3].compat->allow_empty_signature == std::nullopt);
}

TEST_CASE(
    "Kimi Anthropic Messages streams the frozen request and repaired SSE sequence through Models",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse = read_fixture_text("wire/anthropic-messages-kimi.sse");
    REQUIRE_FALSE(sse.empty());
    const auto split = sse.size() / 2;
    transport->attempts.push_back(TransportAttempt{
        .chunks = {sse.substr(0, split), sse.substr(split)},
    });
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    auto options = authorized_options();
    options.temperature = 0.5;
    options.max_tokens = 256;
    options.reasoning = ai::ThinkingLevel::High;
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 4321;

    auto run = run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(run.result->response_id == "msg_kimi");
    CHECK(run.result->usage.input == 100);
    CHECK(run.result->usage.output == 30);
    CHECK(run.result->usage.cache_read == 20);
    CHECK(run.result->usage.cache_write == 10);
    CHECK(run.result->usage.cache_write_1h == 4);
    CHECK(run.result->usage.reasoning == 7);
    CHECK(run.result->usage.total_tokens == 160);
    CHECK(std::abs(run.result->usage.cost.total - 0.0002264) < 1e-12);
    REQUIRE(run.result->content.size() == 4);
    CHECK(std::get<ai::TextContent>(run.result->content[0]).text == "Hello world");
    const auto& thinking = std::get<ai::ThinkingContent>(run.result->content[1]);
    CHECK(thinking.thinking == "plan more");
    CHECK(thinking.thinking_signature == "dummy-signature-tail");
    const auto& redacted = std::get<ai::ThinkingContent>(run.result->content[2]);
    CHECK(redacted.redacted);
    CHECK(redacted.thinking == "[Reasoning redacted]");
    CHECK(redacted.thinking_signature == "dummy-redacted");
    const auto& tool = std::get<ai::ToolCallContent>(run.result->content[3]);
    CHECK(tool.id == "toolu_test");
    CHECK(tool.name == "edit");
    CHECK(tool.raw_arguments == "{\"path\":\"A\\H\",\"text\":\"col1\tcol2\"}");
    REQUIRE(tool.arguments);
    CHECK(tool.arguments->at("path").get_string() == "A\\H");
    CHECK(tool.arguments->at("text").get_string() == "col1\tcol2");

    tests::check_pi_event_snapshot(
        run.events,
        "wire/anthropic-messages-kimi-ts-events.json");

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.kimi.com/coding/v1/messages");
    CHECK(request.timeout == std::chrono::milliseconds{4321});
    CHECK(request.headers.at("Authorization") == "Bearer dummy-kimi-oauth");
    CHECK(request.headers.at("anthropic-version") == "2023-06-01");
    CHECK(request.headers.at("anthropic-dangerous-direct-browser-access") == "true");
    CHECK_FALSE(request.headers.contains("x-api-key"));
    CHECK_FALSE(request.headers.contains("anthropic-beta"));
    CHECK_FALSE(request.headers.contains("x-app"));
    CHECK(request.headers.at("User-Agent") == "KimiCLI/1.5");

    auto expected_request = read_fixture_text("wire/anthropic-messages-kimi-ts-request.json");
    REQUIRE_FALSE(expected_request.empty());
    if (expected_request.back() == '\n') {
        expected_request.pop_back();
    }
    CHECK(request.body == expected_request);
}

TEST_CASE(
    "Kimi raw stop reason capture: rejected refusal matches the frozen TS snapshot",
    "[ai][provider][anthropic][issue374][issue375]") {
    auto transport = std::make_shared<ScriptedTransport>();
    const auto sse =
        read_fixture_text("wire/anthropic-messages-kimi-refusal.sse");
    REQUIRE_FALSE(sse.empty());
    transport->attempts.push_back(TransportAttempt{.chunks = {sse}});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    auto options = authorized_options();
    options.temperature = 0.5;
    options.max_tokens = 256;
    options.reasoning = ai::ThinkingLevel::High;
    options.cache_retention = ai::CacheRetention::Short;
    options.timeout_ms = 4321;

    auto run = run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    // pi maps refusal to error with the generic refusal message when no
    // stop_details explanation arrives (anthropic-messages.ts mapStopReason).
    CHECK(*run.result->error_message ==
        "The model refused to complete the request");
    // The raw provider value is captured before the mapping rejects it.
    REQUIRE(run.result->raw_stop_reason);
    CHECK(*run.result->raw_stop_reason == "refusal");
    CHECK(run.result->response_id == "msg_refusal");
    tests::check_pi_event_snapshot(
        run.events,
        "wire/anthropic-messages-kimi-refusal-ts-events.json");

    REQUIRE(transport->requests.size() == 1);
    auto expected_request =
        read_fixture_text("wire/anthropic-messages-kimi-ts-request.json");
    REQUIRE_FALSE(expected_request.empty());
    if (expected_request.back() == '\n') {
        expected_request.pop_back();
    }
    CHECK(transport->requests.front().body == expected_request);
}

TEST_CASE(
    "Kimi Anthropic Messages keeps adaptive thinking and k3 off-null semantics",
    "[ai][provider][anthropic][issue341]") {
    auto enabled_transport = std::make_shared<ScriptedTransport>();
    enabled_transport->attempts.push_back(TransportAttempt{
        .chunks = {terminal_sse("end_turn")},
    });
    const auto enabled_model = kimi_model("k3");
    auto enabled_models = make_models(enabled_transport, enabled_model);
    REQUIRE(enabled_models);
    auto enabled_options = authorized_options();
    enabled_options.reasoning = ai::ThinkingLevel::Medium;
    enabled_options.temperature = 0.9;

    auto enabled = run_models(
        *enabled_models, enabled_model, {}, std::move(enabled_options));

    REQUIRE(enabled.result);
    REQUIRE(enabled_transport->requests.size() == 1);
    const auto& enabled_body = enabled_transport->requests.front().body;
    CHECK(enabled_body.contains("\"thinking\":{\"display\":\"summarized\",\"type\":\"adaptive\"}"));
    CHECK(enabled_body.contains("\"output_config\":{\"effort\":\"high\"}"));
    CHECK_FALSE(enabled_body.contains("temperature"));

    auto off_transport = std::make_shared<ScriptedTransport>();
    off_transport->attempts.push_back(TransportAttempt{
        .chunks = {terminal_sse("end_turn")},
    });
    const auto off_model = kimi_model("k3");
    auto off_models = make_models(off_transport, off_model);
    REQUIRE(off_models);
    auto off_options = authorized_options();
    off_options.temperature = 0.4;

    auto off = run_models(*off_models, off_model, {}, std::move(off_options));

    REQUIRE(off.result);
    REQUIRE(off_transport->requests.size() == 1);
    const auto& off_body = off_transport->requests.front().body;
    CHECK_FALSE(off_body.contains("\"thinking\""));
    CHECK(off_body.contains("\"temperature\":0.4"));
}

TEST_CASE(
    "Kimi Anthropic Messages enforces the strict termination matrix",
    "[ai][provider][anthropic][issue341]") {
    struct Case {
        std::string sse;
        ai::AssistantStopReason expected;
        std::string diagnostic;
    };
    const std::vector<Case> cases{
        Case{
            .sse = terminal_sse("end_turn"),
            .expected = ai::AssistantStopReason::Stop,
            .diagnostic = {},
        },
        Case{
            .sse = terminal_sse("pause_turn"),
            .expected = ai::AssistantStopReason::Stop,
            .diagnostic = {},
        },
        Case{
            .sse = terminal_sse("stop_sequence"),
            .expected = ai::AssistantStopReason::Stop,
            .diagnostic = {},
        },
        Case{
            .sse = terminal_sse("max_tokens"),
            .expected = ai::AssistantStopReason::Length,
            .diagnostic = {},
        },
        Case{
            .sse = terminal_sse("tool_use"),
            .expected = ai::AssistantStopReason::ToolUse,
            .diagnostic = {},
        },
        Case{
            .sse = terminal_sse(
                "refusal",
                ",\"stop_details\":{\"explanation\":\"blocked by policy\"}"),
            .expected = ai::AssistantStopReason::Error,
            .diagnostic = "blocked by policy",
        },
        Case{
            .sse = terminal_sse("sensitive"),
            .expected = ai::AssistantStopReason::Error,
            .diagnostic = "sensitive",
        },
        Case{
            .sse = terminal_sse("future_reason"),
            .expected = ai::AssistantStopReason::Error,
            .diagnostic = "Unhandled Anthropic stop reason",
        },
        Case{
            .sse = terminal_sse("end_turn", {}, false),
            .expected = ai::AssistantStopReason::Error,
            .diagnostic = "message_stop",
        },
    };

    for (const auto& test_case : cases) {
        auto transport = std::make_shared<ScriptedTransport>();
        transport->attempts.push_back(TransportAttempt{.chunks = {test_case.sse}});
        const auto model = kimi_model();
        auto models = make_models(transport, model);
        REQUIRE(models);

        auto run = run_models(*models, model, {}, authorized_options());

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == test_case.expected);
        REQUIRE_FALSE(run.events.empty());
        if (test_case.expected == ai::AssistantStopReason::Error) {
            CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
            REQUIRE(run.result->error_message);
            CHECK(run.result->error_message->contains(test_case.diagnostic));
        } else {
            CHECK(std::holds_alternative<ai::AssistantDoneEvent>(run.events.back()));
        }
    }
}

TEST_CASE(
    "Kimi Anthropic Messages partials carry the pending stop reason",
    "[ai][provider][anthropic][issue374]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_pending\","
        "\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"hel\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":1}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
    }});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    auto run = run_models(*models, model, {}, authorized_options());

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    const std::vector<std::string> expected{
        "start", "text_start", "text_delta", "text_end", "done"};
    CHECK(event_names(run.events) == expected);
    const auto partials = partial_stop_reasons(run.events);
    REQUIRE(partials.size() == 4);
    for (const auto reason : partials) {
        CHECK(reason == ai::AssistantStopReason::Pending);
    }
}

TEST_CASE(
    "Kimi Anthropic Messages captures the raw stop reason before mapping",
    "[ai][provider][anthropic][issue374]") {
    struct Case {
        std::string sse;
        ai::AssistantStopReason mapped;
        std::string raw;
    };
    const std::vector<Case> cases{
        Case{
            .sse = terminal_sse("end_turn"),
            .mapped = ai::AssistantStopReason::Stop,
            .raw = "end_turn",
        },
        // pause_turn is normalized to stop by the mapper; the raw provider
        // value is still preserved verbatim.
        Case{
            .sse = terminal_sse("pause_turn"),
            .mapped = ai::AssistantStopReason::Stop,
            .raw = "pause_turn",
        },
        // Unknown reasons are rejected by the mapper; the raw value is
        // captured before the rejection.
        Case{
            .sse = terminal_sse("future_reason"),
            .mapped = ai::AssistantStopReason::Error,
            .raw = "future_reason",
        },
    };

    for (const auto& test_case : cases) {
        auto transport = std::make_shared<ScriptedTransport>();
        transport->attempts.push_back(TransportAttempt{.chunks = {test_case.sse}});
        const auto model = kimi_model();
        auto models = make_models(transport, model);
        REQUIRE(models);

        auto run = run_models(*models, model, {}, authorized_options());

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == test_case.mapped);
        REQUIRE(run.result->raw_stop_reason);
        CHECK(*run.result->raw_stop_reason == test_case.raw);
    }
}

TEST_CASE(
    "Kimi Anthropic Messages stream ending still pending is a terminal error",
    "[ai][provider][anthropic][issue374]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_pending\","
        "\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
    }});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    auto run = run_models(*models, model, {}, authorized_options());

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->contains(
        "Anthropic stream ended without a stop reason"));
    REQUIRE_FALSE(run.events.empty());
    REQUIRE(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    const auto& terminal = std::get<ai::AssistantErrorEvent>(run.events.back());
    CHECK(terminal.reason == ai::AssistantStopReason::Error);
    CHECK(terminal.error.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error.error_message);
    CHECK(*terminal.error.error_message == *run.result->error_message);
}

TEST_CASE(
    "Kimi Anthropic Messages missing message_stop carries the partial message",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_partial\","
        "\"usage\":{\"input_tokens\":2}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"partial\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
    }});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    auto run = run_models(*models, model, {}, authorized_options());

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->contains("message_stop"));
    REQUIRE(run.result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(run.result->content.front()).text == "partial");
}

TEST_CASE(
    "Kimi Anthropic Messages ignores unknown SSE events and throws SSE error data",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: future.event\ndata: not-json\n\n"
        "event: error\ndata: kimi exploded\n\n",
    }});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    auto run = run_models(*models, model, {}, authorized_options());

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message);
    CHECK(run.result->error_message->contains("kimi exploded"));
    const std::vector<std::string> expected{"start", "error"};
    CHECK(event_names(run.events) == expected);
}

TEST_CASE(
    "Kimi Anthropic Messages removes tool scratch state from partial failures",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{.chunks = {
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_partial\","
        "\"name\":\"lookup\",\"input\":{}}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"q\\\":\"}}\n\n"
        "event: error\ndata: partial failure\n\n",
    }});
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);

    auto run = run_models(*models, model, {}, authorized_options());

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
}

TEST_CASE(
    "Kimi Anthropic Messages retries only eligible setup failures",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts = {
        TransportAttempt{
            .head = ai::providers::StreamResponseHead{
                .status_code = 500,
                .headers = {{"retry-after-ms", "0"}},
            },
            .chunks = {"temporary"},
        },
        TransportAttempt{.chunks = {terminal_sse("end_turn")}},
    };
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    auto options = authorized_options();
    options.max_retries = 1;

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(transport->requests.size() == 2);

    auto rate_transport = std::make_shared<ScriptedTransport>();
    rate_transport->attempts = {
        TransportAttempt{
            .head = ai::providers::StreamResponseHead{
                .status_code = 429,
                .headers = {{"retry-after-ms", "0"}},
            },
            .chunks = {"temporarily unavailable"},
        },
        TransportAttempt{.chunks = {terminal_sse("end_turn")}},
    };
    auto rate_models = make_models(rate_transport, model);
    REQUIRE(rate_models);
    auto rate_options = authorized_options();
    rate_options.max_retries = 1;

    auto rate = run_models(*rate_models, model, {}, std::move(rate_options));

    REQUIRE(rate.result);
    CHECK(rate.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(rate_transport->requests.size() == 2);

    auto network_transport = std::make_shared<ScriptedTransport>();
    network_transport->attempts = {
        TransportAttempt{
            .chunks = {},
            .failure = util::make_error(util::ErrorCode::Network, "connection reset"),
        },
        TransportAttempt{.chunks = {terminal_sse("end_turn")}},
    };
    auto network_models = make_models(network_transport, model);
    REQUIRE(network_models);
    auto network_options = authorized_options();
    network_options.max_retries = 1;

    auto network = run_models(
        *network_models, model, {}, std::move(network_options));

    REQUIRE(network.result);
    CHECK(network.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(network_transport->requests.size() == 2);

    auto quota_transport = std::make_shared<ScriptedTransport>();
    quota_transport->attempts.push_back(TransportAttempt{
        .head = ai::providers::StreamResponseHead{
            .status_code = 429,
            .headers = {{"retry-after-ms", "0"}},
        },
        .chunks = {"quota exceeded"},
    });
    auto quota_models = make_models(quota_transport, model);
    REQUIRE(quota_models);
    auto quota_options = authorized_options();
    quota_options.max_retries = 3;

    auto quota = run_models(*quota_models, model, {}, std::move(quota_options));

    REQUIRE(quota.result);
    CHECK(quota.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(quota_transport->requests.size() == 1);
}

TEST_CASE(
    "Kimi Anthropic Messages cancellation yields one aborted terminal",
    "[ai][provider][anthropic][issue341]") {
    auto transport = std::make_shared<ScriptedTransport>();
    transport->attempts.push_back(TransportAttempt{
        .chunks = {},
        .failure = util::make_error(util::ErrorCode::Cancelled, "transport cancelled"),
    });
    const auto model = kimi_model();
    auto models = make_models(transport, model);
    REQUIRE(models);
    std::stop_source stop;
    transport->on_request = [&stop] { stop.request_stop(); };
    auto options = authorized_options();
    options.stop_token = stop.get_token();

    auto run = run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    const std::vector<std::string> expected{"error"};
    CHECK(event_names(run.events) == expected);
}
