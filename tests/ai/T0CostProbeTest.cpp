#include <cch/ai/Models.hpp>

#include "ai/api/MessageConversion.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] ai::AiContext probe_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::user_text_message("hello", 1));
    context.tools.push_back(ai::Tool{
            .name = "lookup",
            .description = "Look up a value",
            .parameters =
                    support::JsonValue::object_t{
                            {"properties",
                                    support::JsonValue::object_t{
                                            {"q", support::JsonValue::object_t{{"type", "string"}}},
                                    }},
                            {"required", support::JsonValue::array_t{"q"}},
                            {"type", "object"},
                    },
    });
    return context;
}

[[nodiscard]] ai::Model budget_anthropic_model() {
    for (const auto& definition : ai::builtin_provider_definitions()) {
        if (definition.id != "opencode-go") {
            continue;
        }
        for (const auto& model : definition.models) {
            if (model.id == "minimax-m3") {
                return model;
            }
        }
    }
    auto fallback = tests::make_model("minimax-m3", "opencode-go", "anthropic-messages");
    fallback.reasoning = true;
    fallback.max_tokens = 8192;
    return fallback;
}

[[nodiscard]] ai::Model strict_responses_model() {
    for (const auto& definition : ai::builtin_provider_definitions()) {
        if (definition.id != "openai") {
            continue;
        }
        for (const auto& model : definition.models) {
            if (model.id == "gpt-4") {
                return model;
            }
        }
    }
    return {};
}

} // namespace

TEST_CASE("T4 probe reaches Anthropic budget-based thinking", "[ai][probe][issue762]") {
    ai::ProviderStreamOptions options;
    options.max_tokens = 6144;
    options.reasoning = ai::ModelThinkingLevel::Low;

    const auto result = ai::api::build_adapter_payload(
            ai::api::AdapterKind::AnthropicMessages, budget_anthropic_model(), probe_context(), options);

    REQUIRE(result);
    CHECK(result->at("thinking").at("type").get_string() == "enabled");
    CHECK(result->at("thinking").at("budget_tokens").get_number() == 2048);
    CHECK(result->at("thinking").at("display").get_string() == "summarized");
    CHECK_FALSE(result->get_object().contains("output_config"));
}

TEST_CASE("T4 Anthropic budget thinking follows harness levels and answer-room clamp", "[ai][probe][issue762]") {
    struct BudgetCase {
        ai::ModelThinkingLevel level;
        double expected;
    };
    const std::vector<BudgetCase> cases{
            {ai::ModelThinkingLevel::Minimal, 1024},
            {ai::ModelThinkingLevel::Low, 2048},
            {ai::ModelThinkingLevel::Medium, 8192},
            {ai::ModelThinkingLevel::High, 16384},
            {ai::ModelThinkingLevel::XHigh, 16384},
            {ai::ModelThinkingLevel::Max, 16384},
    };
    for (const auto& test_case : cases) {
        ai::ProviderStreamOptions options;
        options.max_tokens = 32768;
        options.reasoning = test_case.level;
        const auto result = ai::api::build_adapter_payload(
                ai::api::AdapterKind::AnthropicMessages, budget_anthropic_model(), probe_context(), options);
        REQUIRE(result);
        CHECK(result->at("thinking").at("budget_tokens").get_number() == test_case.expected);
    }

    ai::ProviderStreamOptions clamped_options;
    clamped_options.max_tokens = 2048;
    clamped_options.reasoning = ai::ModelThinkingLevel::High;
    const auto clamped = ai::api::build_adapter_payload(
            ai::api::AdapterKind::AnthropicMessages, budget_anthropic_model(), probe_context(), clamped_options);
    REQUIRE(clamped);
    CHECK(clamped->at("thinking").at("budget_tokens").get_number() == 1024);
}

TEST_CASE(
        "T4 OpenCode Go Anthropic budget thinking preserves image auth and session behavior", "[ai][probe][issue762]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 200, .headers = {}},
            .chunks =
                    {
                            "event: message_start\n"
                            "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_opencode\","
                            "\"model\":\"minimax-m3\",\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
                            "event: message_delta\n"
                            "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                            "\"usage\":{\"output_tokens\":1}}\n\n"
                            "event: message_stop\n"
                            "data: {\"type\":\"message_stop\"}\n\n",
                    },
    });
    const auto model = budget_anthropic_model();
    auto provider = ai::providers::make_composed_provider("opencode-go", "OpenCode Go", {model}, {}, transport);
    auto context = probe_context();
    context.messages.push_back(ai::UserMessage{
            .content =
                    std::vector<ai::Content>{
                            ai::text_content("image"),
                            ai::image_content("YWJj", "image/png"),
                    },
            .timestamp = 2,
    });

    ai::ProviderStreamOptions options;
    options.auth.api_key = "opencode-key";
    options.max_tokens = 4096;
    options.reasoning = ai::ModelThinkingLevel::High;
    options.session_id = "opencode-session";
    const auto result = tests::run_async_result(provider->stream(model, std::move(context), std::move(options))
                    .run([](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; }));

    REQUIRE(result);
    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://opencode.ai/zen/go/v1/messages");
    CHECK(request.headers.at("x-api-key") == "opencode-key");
    CHECK(request.headers.at("x-opencode-session") == "opencode-session");
    CHECK(request.headers.at("anthropic-version") == "2023-06-01");
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("thinking").at("type").get_string() == "enabled");
    CHECK(body->at("thinking").at("budget_tokens").get_number() == 3072);
    CHECK(body->at("thinking").at("display").get_string() == "summarized");
    CHECK(request.body.contains("\"type\":\"image\""));
}

TEST_CASE("T4 probe serializes Responses strict-false from the catalog flag", "[ai][probe][issue762]") {
    ai::ProviderStreamOptions options;
    options.max_tokens = 64;

    const auto model = strict_responses_model();
    REQUIRE_FALSE(model.id.empty());
    const auto result =
            ai::api::build_adapter_payload(ai::api::AdapterKind::OpenAIResponses, model, probe_context(), options);

    REQUIRE(result);
    const auto& tools = result->at("tools").get_array();
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().at("strict").get_boolean() == false);
    CHECK_FALSE(tools.front().at("parameters").get_object().contains("strict"));
}

TEST_CASE("T0 probe reaches the DeepSeek OpenAI Completions adapter", "[ai][probe][issue758][issue761]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    auto model = tests::make_model("deepseek-flash", "deepseek", "openai-completions");
    model.base_url = "https://api.deepseek.com";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::High, "high"},
    };
    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{
            .supports_store = false,
            .supports_strict_mode = true,
            .max_tokens_field = ai::OpenAICompletionsMaxTokensField::MaxTokens,
            .requires_reasoning_content_on_assistant_messages = true,
            .thinking_format = ai::OpenAICompletionsThinkingFormat::DeepSeek,
    }};
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"chatcmpl-probe\",\"model\":\"deepseek-flash\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"plan\"},"
                            "\"finish_reason\":null}]}\n\n"
                            "data: {\"id\":\"chatcmpl-probe\",\"model\":\"deepseek-flash\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"answer\"},"
                            "\"finish_reason\":null}]}\n\n"
                            "data: {\"id\":\"chatcmpl-probe\",\"model\":\"deepseek-flash\","
                            "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                            "data: {\"id\":\"chatcmpl-probe\",\"model\":\"deepseek-flash\","
                            "\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":2}}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    auto provider = ai::providers::make_composed_provider("deepseek", "DeepSeek", {model}, {}, transport);

    ai::ProviderStreamOptions options;
    options.auth.api_key = "dummy-probe-key";
    options.max_tokens = 4096;
    options.reasoning = ai::ModelThinkingLevel::High;
    auto stream = provider->stream(model, probe_context(), std::move(options));
    const auto result = tests::run_async_result(
            std::move(stream).run([](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; }));

    REQUIRE(result);
    CHECK(result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(result->response_id == "chatcmpl-probe");
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().url == "https://api.deepseek.com/chat/completions");
    CHECK(transport->requests.front().headers.at("Authorization") == "Bearer dummy-probe-key");
    const auto body = support::read_json(transport->requests.front().body);
    REQUIRE(body);
    CHECK(body->at("max_tokens").get_number() == 4096);
    CHECK_FALSE(body->get_object().contains("store"));
    CHECK(body->at("stream_options").at("include_usage").get_boolean());
    CHECK(body->at("tools").get_array().front().at("function").at("strict").get_boolean() == false);
}

TEST_CASE("T4 probe keeps authenticated unknown APIs on the zero-transport path", "[ai][probe][issue762]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    auto model = tests::make_model("unknown", "deepseek", "unknown-api");
    auto provider = ai::providers::make_composed_provider("deepseek", "DeepSeek", {model}, {}, transport);

    ai::ProviderStreamOptions options;
    options.auth.api_key = "dummy-probe-key";
    auto stream = provider->stream(model, probe_context(), std::move(options));
    const auto result = tests::run_async_result(
            std::move(stream).run([](const ai::AssistantStreamEvent&) -> support::ExpectedVoid { return {}; }));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::Stream);
    CHECK(result.error().message == "Provider deepseek has no API implementation for \"unknown-api\"");
    CHECK(transport->requests.empty());
}
