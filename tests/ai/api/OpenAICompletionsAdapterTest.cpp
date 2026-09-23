#include <cch/ai/Models.hpp>

#include "ai/api/MessageConversion.hpp"
#include "support/AiScenarioKit.hpp"
#include "support/Json.hpp"
#include "support/ModelFixture.hpp"
#include "support/PiEventSnapshot.hpp"
#include "support/PiFixture.hpp"
#include "support/ScriptedProvider.hpp"
#include "support/StreamAdapterFixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using tests::read_pi_fixture_text;

[[nodiscard]] ai::Tool lookup_tool() {
    return ai::Tool{
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
    auto model = tests::make_model("openai/gpt-5", "openrouter", "openai-completions");
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

[[nodiscard]] ai::Model kimi_model(std::string_view id) {
    for (const auto& definition : ai::builtin_provider_definitions()) {
        if (definition.id != "kimi-coding") {
            continue;
        }
        for (const auto& model : definition.models) {
            if (model.id == id) {
                return model;
            }
        }
    }
    return {};
}

} // namespace

TEST_CASE("DeepSeek Chat Completions streams reasoning text tool calls and usage", "[ai][api][completions][issue761]") {
    const auto sse = read_pi_fixture_text("wire/openai-completions-deepseek.sse");
    REQUIRE(sse);
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {sse->substr(0, sse->size() / 2), sse->substr(sse->size() / 2)},
    });
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"chatcmpl-2\",\"model\":\"deepseek-flash\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"replayed\"},"
                            "\"finish_reason\":\"stop\"}]}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-deepseek-key";
    options.max_tokens = 4096;
    options.reasoning = ai::ThinkingLevel::High;
    const auto run = tests::run_models(*models, model, request_context(), std::move(options));

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
    CHECK(tests::event_names(run.events) == std::vector<std::string>{
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
    const auto event_mismatch =
            tests::pi_event_snapshot_mismatch(run.events, "wire/openai-completions-deepseek-ts-events.json");
    INFO((event_mismatch ? *event_mismatch : ""));
    CHECK_FALSE(event_mismatch);

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
    auto expected_request = read_pi_fixture_text("wire/openai-completions-deepseek-ts-request.json");
    REQUIRE(expected_request);
    if (expected_request->back() == '\n') {
        expected_request->pop_back();
    }
    CHECK(request.body == *expected_request);

    ai::AiContext continuation;
    continuation.messages.push_back(*run.result);
    continuation.messages.push_back(ai::tool_result_message(call.id, call.name, "tool answer"));
    ai::SimpleStreamOptions replay_options;
    replay_options.api_key = "dummy-deepseek-key";
    replay_options.max_tokens = 4096;
    const auto replay = tests::run_models(*models, model, std::move(continuation), std::move(replay_options));
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
            .chunks =
                    {
                            "data: {\"id\":\"router-1\",\"model\":\"openai/gpt-5\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
                            "\"finish_reason\":\"stop\"}]}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    auto model = openrouter_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-openrouter-key";
    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "router-session";
    const auto run = tests::run_models(*models, model, request_context(), std::move(options));

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

TEST_CASE("Kimi Coding uses the vendor Completions catalog and omits effort for highspeed",
        "[ai][api][completions][kimi][issue763][spec]") {
    const auto highspeed = kimi_model("kimi-for-coding-highspeed");
    REQUIRE_FALSE(highspeed.id.empty());
    CHECK(highspeed.api == "openai-completions");
    CHECK(highspeed.provider == "kimi-coding");
    CHECK(highspeed.base_url == "https://api.kimi.com/coding/v1");
    CHECK_FALSE(highspeed.headers.has_value());
    REQUIRE(highspeed.thinking_level_map);
    CHECK(highspeed.thinking_level_map->size() == 7);
    for (const auto level : {
                 ai::ModelThinkingLevel::Off,
                 ai::ModelThinkingLevel::Minimal,
                 ai::ModelThinkingLevel::Low,
                 ai::ModelThinkingLevel::Medium,
                 ai::ModelThinkingLevel::High,
                 ai::ModelThinkingLevel::XHigh,
                 ai::ModelThinkingLevel::Max,
         }) {
        CHECK(highspeed.thinking_level_map->contains(level));
        CHECK_FALSE(highspeed.thinking_level_map->at(level).has_value());
    }

    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"kimi-1\",\"model\":\"kimi-for-coding-highspeed\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
                            "\"finish_reason\":\"stop\"}]}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    auto models = tests::make_scripted_models(highspeed, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-kimi-key";
    options.max_tokens = 512;
    options.reasoning = ai::ThinkingLevel::High;
    ai::AiContext context;
    context.tools.push_back(lookup_tool());
    const auto run = tests::run_models(*models, highspeed, std::move(context), std::move(options));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "https://api.kimi.com/coding/v1/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer dummy-kimi-key");
    CHECK_FALSE(request.headers.contains("User-Agent"));
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("max_completion_tokens").get_number() == 512);
    CHECK_FALSE(body->get_object().contains("reasoning_effort"));
    CHECK_FALSE(body->get_object().contains("effort"));
    CHECK_FALSE(body->at("tools").get_array().front().at("function").get_object().contains("strict"));
}

TEST_CASE("Kimi Coding maps every vendor thinking level without undocumented effort values",
        "[ai][api][completions][kimi][issue763][spec]") {
    constexpr std::array model_ids{
            std::string_view{"k3"},
            std::string_view{"k3-256k"},
            std::string_view{"kimi-for-coding"},
            std::string_view{"kimi-for-coding-highspeed"},
    };
    constexpr std::array levels{
            ai::ModelThinkingLevel::Off,
            ai::ModelThinkingLevel::Minimal,
            ai::ModelThinkingLevel::Low,
            ai::ModelThinkingLevel::Medium,
            ai::ModelThinkingLevel::High,
            ai::ModelThinkingLevel::XHigh,
            ai::ModelThinkingLevel::Max,
    };
    constexpr std::array<std::string_view, 3> documented_efforts{"low", "high", "max"};

    for (const auto model_id : model_ids) {
        const auto model = kimi_model(model_id);
        REQUIRE_FALSE(model.id.empty());
        REQUIRE(model.thinking_level_map);
        for (const auto level : levels) {
            const auto clamped = level == ai::ModelThinkingLevel::Off ? ai::ModelThinkingLevel::Off
                                                                      : ai::clamp_thinking_level(model, level);
            const auto mapped = model.thinking_level_map->at(clamped);
            if (mapped) {
                CHECK(std::ranges::find(documented_efforts, *mapped) != documented_efforts.end());
            }

            ai::ProviderStreamOptions options;
            options.max_tokens = 512;
            options.reasoning = clamped;
            const auto payload =
                    ai::api::build_adapter_payload(ai::api::AdapterKind::OpenAICompletions, model, {}, options);
            REQUIRE(payload);
            const auto& object = payload->get_object();
            const auto effort = object.find("reasoning_effort");
            CHECK((effort != object.end()) == mapped.has_value());
            if (mapped) {
                REQUIRE(effort != object.end());
                CHECK(effort->second.get_string() == *mapped);
            }
        }

        ai::ProviderStreamOptions omitted_options;
        omitted_options.max_tokens = 512;
        const auto omitted =
                ai::api::build_adapter_payload(ai::api::AdapterKind::OpenAICompletions, model, {}, omitted_options);
        REQUIRE(omitted);
        CHECK_FALSE(omitted->get_object().contains("reasoning_effort"));
    }
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
    context.messages.push_back(ai::tool_result_message("call_1|fc_1", "lookup", "ok"));
    ai::ProviderStreamOptions options;
    options.max_tokens = 64;

    const auto payload =
            ai::api::build_adapter_payload(ai::api::AdapterKind::OpenAICompletions, model, context, options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("role").get_string() == "assistant");
    CHECK(messages[0].at("reasoning_content").get_string() == "plan");
    CHECK(messages[0].at("tool_calls").get_array().front().at("id").get_string() == "call_1_fc_1");
    CHECK(messages[1].at("role").get_string() == "tool");
    CHECK(messages[1].at("tool_call_id").get_string() == "call_1_fc_1");
}

TEST_CASE("Completions rejects DONE and clean EOF without finish_reason", "[ai][api][completions][issue761]") {
    for (const auto& stream : {
                 std::string{"data: [DONE]\n\n"},
                 std::string{"data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},"
                             "\"finish_reason\":null}]}\n\n"},
         }) {
        auto transport = std::make_shared<tests::ScriptedTransport>();
        transport->attempts.push_back(tests::TransportAttempt{.chunks = {stream}});
        const auto model = deepseek_model();
        auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
        REQUIRE(models);
        ai::SimpleStreamOptions options;
        options.api_key = "dummy-key";
        const auto run = tests::run_models(*models, model, {}, std::move(options));

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
        REQUIRE_FALSE(run.events.empty());
        CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    }
}

TEST_CASE("Completions cancellation after partial text retains the text with one aborted terminal",
        "[ai][api][completions][issue757][cancellation]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"chatcmpl-partial\",\"choices\":[{\"index\":0,"
                            "\"delta\":{\"content\":\"partial answer\"},\"finish_reason\":null}]}\n\n",
                    },
    });
    const auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    std::stop_source stop;
    transport->on_request = [&stop] { stop.request_stop(); };
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.stop_token = stop.get_token();
    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    REQUIRE(run.result->content.size() == 1);
    CHECK(std::get<ai::TextContent>(run.result->content.front()).text == "partial answer");
    const auto terminals = std::ranges::count_if(run.events, [](const ai::AssistantStreamEvent& event) {
        return std::holds_alternative<ai::AssistantErrorEvent>(event) ||
               std::holds_alternative<ai::AssistantDoneEvent>(event);
    });
    CHECK(terminals == 1);
    const auto* terminal = std::get_if<ai::AssistantErrorEvent>(&run.events.back());
    REQUIRE(terminal);
    CHECK(terminal->reason == ai::AssistantStopReason::Aborted);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().stop_token.stop_requested());
}

TEST_CASE("Completions cancellation after partial tool arguments retains the arguments with one aborted terminal",
        "[ai][api][completions][issue757][cancellation]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"chatcmpl-partial-tools\",\"choices\":[{\"index\":0,"
                            "\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
                            "\"type\":\"function\",\"function\":{\"name\":\"lookup\","
                            "\"arguments\":\"{\\\"q\\\": \\\"lo\"}}]},\"finish_reason\":null}]}\n\n",
                    },
    });
    const auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    std::stop_source stop;
    transport->on_request = [&stop] { stop.request_stop(); };
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.stop_token = stop.get_token();
    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    REQUIRE(run.result->content.size() == 1);
    const auto& call = std::get<ai::ToolCallContent>(run.result->content.front());
    CHECK(call.id == "call_1");
    CHECK(call.name == "lookup");
    REQUIRE(call.arguments);
    CHECK(call.arguments->at("q").get_string() == "lo");
    const auto terminals = std::ranges::count_if(run.events, [](const ai::AssistantStreamEvent& event) {
        return std::holds_alternative<ai::AssistantErrorEvent>(event) ||
               std::holds_alternative<ai::AssistantDoneEvent>(event);
    });
    CHECK(terminals == 1);
    const auto* terminal = std::get_if<ai::AssistantErrorEvent>(&run.events.back());
    REQUIRE(terminal);
    CHECK(terminal->reason == ai::AssistantStopReason::Aborted);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().stop_token.stop_requested());
}

TEST_CASE("Completions cancellation yields one aborted terminal", "[ai][api][completions][issue761][cancellation]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {},
            .failure = support::make_error(support::ErrorCode::Cancelled, "transport cancelled"),
    });
    const auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    std::stop_source stop;
    transport->on_request = [&stop] { stop.request_stop(); };
    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    options.stop_token = stop.get_token();
    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(run.result->error_message == "Request was aborted");
    CHECK(tests::event_names(run.events) == std::vector<std::string>{"error"});
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests.front().stop_token.stop_requested());
}

TEST_CASE("Completions transport failures yield one error terminal", "[ai][api][completions][issue761][failure]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {},
            .failure = support::make_error(support::ErrorCode::Network, "connection reset"),
    });
    const auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;
    options.api_key = "dummy-key";
    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->error_message == "connection reset");
    CHECK(tests::event_names(run.events) == std::vector<std::string>{"error"});
}
