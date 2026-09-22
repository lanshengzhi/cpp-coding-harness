#include <cch/ai/Models.hpp>

#include "ai/api/MessageConversion.hpp"
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

[[nodiscard]] ai::Tool lookup_tool() {
    return ai::Tool{
            .name = "lookup",
            .description = "Look up a value",
            .parameters = support::JsonValue::object_t{
                    {"properties",
                            support::JsonValue::object_t{
                                    {"q", support::JsonValue::object_t{{"type", "string"}}},
                            }},
                    {"required", support::JsonValue::array_t{"q"}},
                    {"type", "object"},
            },
    };
}

[[nodiscard]] ai::Model deepseek_model() {
    auto model = tests::make_model("deepseek-flash", "deepseek", "openai-completions");
    model.base_url = "https://api.deepseek.com/";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::High, "high"},
    };
    model.cost = ai::ModelCost{
            .input = 2,
            .output = 4,
            .cache_read = 1,
            .cache_write = 3,
    };
    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{
            .supports_store = false,
            .supports_strict_mode = true,
            .max_tokens_field = ai::OpenAICompletionsMaxTokensField::MaxTokens,
            .requires_reasoning_content_on_assistant_messages = true,
            .thinking_format = ai::OpenAICompletionsThinkingFormat::DeepSeek,
    }};
    return model;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::user_text_message("hello", 1));
    context.tools.push_back(lookup_tool());
    return context;
}

[[nodiscard]] ai::Model openrouter_model() {
    auto model = tests::make_model(
            "openai/gpt-5",
            "openrouter",
            "openai-completions");
    model.base_url = "https://openrouter.ai/api/v1";
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::Off, "none"},
            {ai::ModelThinkingLevel::High, "high"},
    };
    model.compat = ai::ModelCompatVariant{ai::OpenAICompletionsCompat{
            .supports_strict_mode = true,
            .thinking_format = ai::OpenAICompletionsThinkingFormat::OpenRouter,
    }};
    return model;
}

} // namespace

TEST_CASE("DeepSeek Chat Completions streams reasoning text tool calls and usage",
        "[ai][api][completions][issue761]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"plan\"},"
                    "\"finish_reason\":null}]}\n\n"
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"answer\"},"
                    "\"finish_reason\":null}]}\n\n"
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                    "\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"lookup\","
                    "\"arguments\":\"{\\\"q\\\":\"}}]},\"finish_reason\":null}]}\n\n"
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
                    "\"id\":\"changed-id\",\"function\":{\"arguments\":\"\\\"x\\\"}\"}}]},"
                    "\"finish_reason\":null}]}\n\n"
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
                    "data: {\"id\":\"chatcmpl-1\",\"model\":\"deepseek-flash\",\"choices\":[],"
                    "\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":30,"
                    "\"prompt_tokens_details\":{\"cached_tokens\":20,\"cache_write_tokens\":10},"
                    "\"completion_tokens_details\":{\"reasoning_tokens\":7}}}\n\n"
                    "data: [DONE]\n\n",
            },
    });
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {
                    "data: {\"id\":\"chatcmpl-2\",\"model\":\"deepseek-flash\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"replayed\"},"
                    "\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
            },
    });
    auto model = deepseek_model();
    auto models = tests::make_scripted_models(
            model,
            tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.max_tokens = 4096;
    options.reasoning = ai::ThinkingLevel::High;
    const auto run = tests::run_models(
            *models,
            model,
            request_context(),
            std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(run.result->response_id == "chatcmpl-1");
    CHECK(run.result->usage.input == 90);
    CHECK(run.result->usage.output == 30);
    CHECK(run.result->usage.cache_read == 20);
    CHECK(run.result->usage.cache_write == 10);
    CHECK(run.result->usage.reasoning == 7);
    REQUIRE(run.result->content.size() == 3);
    CHECK(std::get<ai::ThinkingContent>(run.result->content[0]).thinking == "plan");
    CHECK(std::get<ai::TextContent>(run.result->content[1]).text == "answer");
    const auto& call = std::get<ai::ToolCallContent>(run.result->content[2]);
    CHECK(call.id == "call_1");
    CHECK(call.name == "lookup");
    REQUIRE(call.arguments);
    CHECK(call.arguments->at("q").get_string() == "x");
    CHECK(tests::event_names(run.events) ==
            std::vector<std::string>{
                    "start",
                    "thinking_start",
                    "thinking_delta",
                    "text_start",
                    "text_delta",
                    "toolcall_start",
                    "toolcall_delta",
                    "toolcall_delta",
                    "thinking_end",
                    "text_end",
                    "toolcall_end",
                    "done",
            });

    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.deepseek.com/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer dummy-deepseek-key");
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("messages").get_array().front().at("role").get_string() == "system");
    CHECK(body->at("max_tokens").get_number() == 4096);
    CHECK(body->at("thinking").at("type").get_string() == "enabled");
    CHECK(body->at("reasoning_effort").get_string() == "high");
    CHECK(body->at("stream_options").at("include_usage").get_boolean());
    CHECK_FALSE(body->get_object().contains("store"));
    CHECK(body->at("tools").get_array().front().at("function").at("strict").get_boolean() == false);

    ai::AiContext continuation;
    continuation.messages.push_back(*run.result);
    continuation.messages.push_back(ai::tool_result_message(
            call.id,
            call.name,
            "tool answer"));
    ai::SimpleStreamOptions replay_options;
    replay_options.api_key = "dummy-deepseek-key";
    replay_options.max_tokens = 4096;
    const auto replay = tests::run_models(
            *models,
            model,
            std::move(continuation),
            std::move(replay_options));
    REQUIRE(replay.result);
    CHECK(replay.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(replay.result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(replay.result->content.front()).text == "replayed");

    REQUIRE(transport->requests.size() == 2);
    const auto replay_body = support::read_json(transport->requests.back().body);
    REQUIRE(replay_body);
    const auto& replay_messages = replay_body->at("messages").get_array();
    REQUIRE(replay_messages.size() == 2);
    CHECK(replay_messages[0].at("reasoning_content").get_string() == "plan");
    CHECK(replay_messages[0].at("tool_calls").get_array().front().at("id").get_string() == "call_1");
    CHECK(replay_messages[1].at("role").get_string() == "tool");
    CHECK(replay_messages[1].at("tool_call_id").get_string() == "call_1");
}

TEST_CASE("OpenRouter Chat Completions selects developer reasoning and affinity defaults",
        "[ai][api][completions][issue761]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {
                    "data: {\"id\":\"router-1\",\"model\":\"openai/gpt-5\","
                    "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
                    "\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
            },
    });
    auto model = openrouter_model();
    auto models = tests::make_scripted_models(
            model,
            tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-openrouter-key";
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "router-session";
    const auto run = tests::run_models(
            *models,
            model,
            request_context(),
            std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://openrouter.ai/api/v1/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer dummy-openrouter-key");
    CHECK(request.headers.at("x-session-id") == "router-session");
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("messages").get_array().front().at("role").get_string() == "developer");
    CHECK(body->at("max_completion_tokens").get_number() == 123);
    CHECK(body->at("store").get_boolean() == false);
    CHECK(body->at("reasoning").at("effort").get_string() == "high");
    CHECK(body->at("tools").get_array().front().at("function").at("strict").get_boolean() == false);
}

TEST_CASE("Completions replay normalizes reasoning and pipe-separated tool history",
        "[ai][conversion][completions][issue761]") {
    auto model = deepseek_model();
    ai::AiContext context;
    ai::AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.content = {
            ai::ThinkingContent{
                    .thinking = "plan",
                    .thinking_signature = "reasoning_content",
            },
            ai::ToolCallContent{
                    .id = "call_1|fc_1",
                    .name = "lookup",
                    .arguments = support::JsonValue::object_t{{"q", "x"}},
                    .raw_arguments = "{\"q\":\"x\"}",
                    .thought_signature = std::nullopt,
                    .arguments_valid = true,
                    .argument_error = std::nullopt,
            },
    };
    context.messages.push_back(std::move(assistant));
    context.messages.push_back(ai::tool_result_message(
            "call_1|fc_1",
            "lookup",
            "ok"));
    ai::ProviderStreamOptions options;
    options.max_tokens = 64;

    const auto payload = ai::api::build_adapter_payload(
            ai::api::AdapterKind::OpenAICompletions,
            model,
            context,
            options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("role").get_string() == "assistant");
    CHECK(messages[0].at("reasoning_content").get_string() == "plan");
    CHECK(messages[0].at("tool_calls").get_array().front().at("id").get_string() ==
            "call_1_fc_1");
    CHECK(messages[1].at("role").get_string() == "tool");
    CHECK(messages[1].at("tool_call_id").get_string() == "call_1_fc_1");
}

TEST_CASE("Completions rejects DONE and clean EOF without finish_reason",
        "[ai][api][completions][issue761]") {
    for (const auto& stream : {
                 std::string{"data: [DONE]\n\n"},
                 std::string{"data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},"
                             "\"finish_reason\":null}]}\n\n"},
         }) {
        auto transport = std::make_shared<tests::ScriptedTransport>();
        transport->attempts.push_back(tests::TransportAttempt{.chunks = {stream}});
        const auto model = deepseek_model();
        auto models = tests::make_scripted_models(
                model,
                tests::ScriptedTransportOptions{.http_transport = transport});
        REQUIRE(models);
        ai::SimpleStreamOptions options;
        options.api_key = "dummy-key";
        const auto run = tests::run_models(
                *models,
                model,
                {},
                std::move(options));

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
        REQUIRE_FALSE(run.events.empty());
        CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    }
}
