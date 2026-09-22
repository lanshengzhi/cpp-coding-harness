#include "ai/api/MessageConversion.hpp"
#include "ai/providers/ComposedProvider.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

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
    auto model = tests::make_model("minimax-m3", "opencode-go", "anthropic-messages");
    model.reasoning = true;
    model.max_tokens = 8192;
    return model;
}

[[nodiscard]] ai::Model strict_responses_model() { return tests::make_model("gpt-4", "openai", "openai-responses"); }

} // namespace

TEST_CASE("T0 probe records the reachable Anthropic budget-thinking limitation", "[ai][probe][issue758]") {
    ai::ProviderStreamOptions options;
    options.max_tokens = 4096;
    options.reasoning = ai::ModelThinkingLevel::High;

    const auto result = ai::api::build_adapter_payload(
            ai::api::AdapterKind::AnthropicMessages, budget_anthropic_model(), probe_context(), options);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == support::ErrorCode::ModelValidation);
    CHECK(result.error().message == "Budget-based Anthropic thinking is outside the supported adapter surface");
}

TEST_CASE("T0 probe records the ordinary Responses strict-false limitation", "[ai][probe][issue758]") {
    ai::ProviderStreamOptions options;
    options.max_tokens = 64;

    const auto result = ai::api::build_adapter_payload(
            ai::api::AdapterKind::OpenAIResponses, strict_responses_model(), probe_context(), options);

    REQUIRE(result);
    const auto& tools = result->at("tools").get_array();
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().get_object().find("strict") == tools.front().get_object().end());
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
        .chunks = {
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

TEST_CASE("T0 probe keeps authenticated unknown APIs on the zero-transport path", "[ai][probe][issue758]") {
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
