#include <cch/ai/Models.hpp>

#include "support/AiScenarioKit.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/ScriptedProvider.hpp"
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
                                            {"q", support::JsonValue::object_t{{"type", "string"}}}}},
                            {"required", support::JsonValue::array_t{"q"}},
                            {"type", "object"},
                    },
    });
    return context;
}

} // namespace

TEST_CASE("T0 probe reaches the DeepSeek OpenAI Completions adapter", "[ai][probe][issue758][issue761]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    auto model = tests::make_model("deepseek-flash", "deepseek", "openai-completions");
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::High, "high"},
    };
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
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;

    options.max_tokens = 4096;
    options.reasoning = ai::ThinkingLevel::High;
    const auto run = tests::run_models(*models, model, probe_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(run.result->response_id == "chatcmpl-probe");
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().url == "http://127.0.0.1:3425/v1/chat/completions");
    CHECK(transport->requests.front().headers.at("Authorization") == "Bearer magpie");
    const auto body = support::read_json(transport->requests.front().body);
    REQUIRE(body);
    CHECK(body->at("max_completion_tokens").get_number() == 4096);
    CHECK(body->at("reasoning_effort").get_string() == "high");
    CHECK(body->at("stream_options").at("include_usage").get_boolean());
    CHECK_FALSE(body->get_object().contains("store"));
    CHECK_FALSE(body->get_object().contains("thinking"));
    CHECK_FALSE(body->at("tools").get_array().front().at("function").get_object().contains("strict"));
}

TEST_CASE("Legacy APIs remain on the zero-transport failure path", "[ai][probe][issue762]") {
    for (const auto* api : {"anthropic-messages", "openai-responses", "openai-codex-responses", "unknown-api"}) {
        auto transport = std::make_shared<tests::ScriptedTransport>();
        auto model = tests::make_model("unimplemented", "deepseek", api);
        auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});

        ai::SimpleStreamOptions options;

        const auto run = tests::run_models(*models, model, probe_context(), std::move(options));

        CAPTURE(api);
        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
        CHECK(run.result->error_message ==
                "Provider deepseek has no API implementation for \"" + std::string{api} + "\"");
        CHECK(transport->requests.empty());
    }
}
