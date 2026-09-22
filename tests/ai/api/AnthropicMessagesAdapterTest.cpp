#include <cch/ai/Models.hpp>
#include "ai/api/PayloadBuilders.hpp"

#include "support/AiScenarioKit.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

[[nodiscard]] ai::Model anthropic_model() {
    auto model = tests::make_model("claude-sonnet", "anthropic", "anthropic-messages");
    model.base_url = "https://api.anthropic.com";
    model.reasoning = false;
    return model;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::user_text_message("hello", 1));
    return context;
}

} // namespace

TEST_CASE("Anthropic Messages streams a generic text response and builds its request",
        "[ai][api][anthropic][issue339][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_generic\","
                    "\"usage\":{\"input_tokens\":2,\"output_tokens\":0}}}\n\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,"
                    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,"
                    "\"delta\":{\"type\":\"text_delta\",\"text\":\"hello\"}}\n\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
                    "\"usage\":{\"output_tokens\":3}}\n\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n\n",
            },
    });

    const auto model = anthropic_model();
    auto models = tests::make_scripted_models(
            model,
            tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-anthropic-key";
    options.max_tokens = 256;
    const auto run = tests::run_models(
            *models,
            model,
            request_context(),
            std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(run.result->response_id == "msg_generic");
    REQUIRE(run.result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(run.result->content.front()).text == "hello");

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.anthropic.com/v1/messages");
    CHECK(request.headers.at("x-api-key") == "dummy-anthropic-key");
    CHECK(request.headers.at("anthropic-version") == "2023-06-01");
    CHECK_FALSE(request.headers.contains("User-Agent"));
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("model").get_string() == "claude-sonnet");
    CHECK(body->at("max_tokens").get_number() == 256);
    CHECK(body->at("messages").get_array().front().at("role").get_string() == "user");
}

TEST_CASE("Anthropic conversion keeps cache markers on the trailing user turn only",
        "[ai][conversion][anthropic][issue339][spec]") {
    const auto model = anthropic_model();
    ai::AiContext context;
    context.messages.push_back(ai::user_text_message("question", 1));
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.content.push_back(ai::text_content("answer"));
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(ai::user_text_message("next", 3));

    ai::ProviderStreamOptions options;
    options.cache_retention = ai::CacheRetention::Short;
    const auto payload = ai::api::build_adapter_payload(
            ai::api::AdapterKind::AnthropicMessages,
            model,
            context,
            options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 3);
    CHECK_FALSE(messages[0].at("content").get_array().front().get_object().contains("cache_control"));
    CHECK_FALSE(messages[1].at("content").get_array().front().get_object().contains("cache_control"));
    const auto& trailing_content = messages.back().at("content").get_array();
    REQUIRE(trailing_content.size() == 1);
    CHECK(trailing_content.front().at("text").get_string() == "next");
    REQUIRE(trailing_content.front().get_object().contains("cache_control"));
    CHECK(trailing_content.front().at("cache_control").at("type").get_string() == "ephemeral");
}
