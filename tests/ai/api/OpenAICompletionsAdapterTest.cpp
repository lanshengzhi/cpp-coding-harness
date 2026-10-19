#include <cch/ai/Models.hpp>

#include "ai/Headers.hpp"
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
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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
    return model;
}

[[nodiscard]] ai::AiContext request_context() {
    ai::AiContext context;
    context.system_prompt = "system";
    context.messages.push_back(ai::user_text_message("hello", 1));
    context.tools.push_back(lookup_tool());
    return context;
}

[[nodiscard]] ai::Model kimi_model(std::string_view id) {
    auto model = tests::make_model(std::string{id}, "kimi-coding", "openai-completions");
    model.reasoning = true;
    model.thinking_level_map = id == "kimi-for-coding-highspeed"
            ? ai::ThinkingLevelMap{
                      {ai::ModelThinkingLevel::Off, std::nullopt},
                      {ai::ModelThinkingLevel::Minimal, std::nullopt},
                      {ai::ModelThinkingLevel::Low, std::nullopt},
                      {ai::ModelThinkingLevel::Medium, std::nullopt},
                      {ai::ModelThinkingLevel::High, std::nullopt},
                      {ai::ModelThinkingLevel::XHigh, std::nullopt},
                      {ai::ModelThinkingLevel::Max, std::nullopt},
              }
            : ai::ThinkingLevelMap{
                      {ai::ModelThinkingLevel::Off, std::nullopt},
                      {ai::ModelThinkingLevel::Minimal, std::nullopt},
                      {ai::ModelThinkingLevel::Low, std::optional<std::string>{"low"}},
                      {ai::ModelThinkingLevel::Medium, std::nullopt},
                      {ai::ModelThinkingLevel::High, std::optional<std::string>{"high"}},
                      {ai::ModelThinkingLevel::XHigh, std::nullopt},
                      {ai::ModelThinkingLevel::Max, std::optional<std::string>{"max"}},
              };
    return model;
}

} // namespace

TEST_CASE("Chat Completions streams reasoning text tool calls and usage", "[ai][api][completions][issue761]") {
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
    CHECK(request.url == "http://127.0.0.1:3425/v1/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer magpie");
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("messages").get_array().front().at("role").get_string() == "system");
    CHECK(body->at("max_completion_tokens").get_number() == 4096);
    CHECK(body->at("reasoning_effort").get_string() == "high");
    CHECK(body->at("stream_options").at("include_usage").get_boolean());
    CHECK_FALSE(body->get_object().contains("store"));
    CHECK_FALSE(body->get_object().contains("thinking"));
    CHECK_FALSE(body->get_object().contains("reasoning"));
    CHECK_FALSE(body->get_object().contains("prompt_cache_key"));
    CHECK_FALSE(body->at("tools").get_array().front().at("function").get_object().contains("strict"));
    CHECK_FALSE(request.headers.contains("x-session-id"));
    CHECK_FALSE(request.headers.contains("X-Magpie-Session"));

    ai::AiContext continuation;
    continuation.messages.push_back(*run.result);
    continuation.messages.push_back(ai::tool_result_message(call.id, call.name, "tool answer"));
    ai::SimpleStreamOptions replay_options;

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

TEST_CASE("an off thinking request states the gateway's explicit none", "[ai][api][completions][issue761][spec]") {
    const auto model = deepseek_model();
    const ai::AiContext no_context;

    // No reasoning option (pi's `reasoning: undefined`) is the agent's "off"
    // thinking level, and an explicit Off clamp states the same thing.
    ai::api::PreparedStreamOptions omitted;
    omitted.max_tokens = 64;
    const auto omitted_payload = ai::api::build_openai_completions_payload(model, no_context, omitted);
    REQUIRE(omitted_payload);
    CHECK(omitted_payload->at("reasoning_effort").get_string() == "none");

    ai::api::PreparedStreamOptions explicit_off;
    explicit_off.max_tokens = 64;
    explicit_off.reasoning = ai::ModelThinkingLevel::Off;
    const auto off_payload = ai::api::build_openai_completions_payload(model, no_context, explicit_off);
    REQUIRE(off_payload);
    CHECK(off_payload->at("reasoning_effort").get_string() == "none");

    // A level the map does not mention falls back to the level's own name.
    ai::api::PreparedStreamOptions unmapped;
    unmapped.max_tokens = 64;
    unmapped.reasoning = ai::ModelThinkingLevel::Low;
    const auto unmapped_payload = ai::api::build_openai_completions_payload(model, no_context, unmapped);
    REQUIRE(unmapped_payload);
    CHECK(unmapped_payload->at("reasoning_effort").get_string() == "low");

    // A model without reasoning never states an effort at all.
    const auto plain = tests::make_model("plain", "magpie", "openai-completions");
    const auto plain_payload = ai::api::build_openai_completions_payload(plain, no_context, explicit_off);
    REQUIRE(plain_payload);
    CHECK_FALSE(plain_payload->get_object().contains("reasoning_effort"));
}

TEST_CASE("a session-scoped request carries the gateway session header and the unified body",
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
    auto model = tests::make_model("openai/gpt-5", "openrouter", "openai-completions");
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{
            {ai::ModelThinkingLevel::Off, "none"},
            {ai::ModelThinkingLevel::High, "high"},
    };
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;

    options.max_tokens = 123;
    options.reasoning = ai::ThinkingLevel::High;
    options.session_id = "router-session";
    const auto run = tests::run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "http://127.0.0.1:3425/v1/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer magpie");
    CHECK(request.headers.at("X-Magpie-Session") == "router-session");
    CHECK_FALSE(request.headers.contains("x-session-id"));
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("messages").get_array().front().at("role").get_string() == "system");
    CHECK(body->at("max_completion_tokens").get_number() == 123);
    CHECK(body->at("prompt_cache_key").get_string() == "router-session");
    CHECK_FALSE(body->get_object().contains("store"));
    CHECK_FALSE(body->get_object().contains("reasoning"));
    CHECK(body->at("reasoning_effort").get_string() == "high");
    CHECK_FALSE(body->at("tools").get_array().front().at("function").get_object().contains("strict"));
}

namespace {

/// One scripted "ok" completion, so a test asserts only the outbound request.
[[nodiscard]] std::shared_ptr<tests::ScriptedTransport> ok_transport() {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"gw-1\",\"model\":\"openai/gpt-5\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
                            "\"finish_reason\":\"stop\"}]}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    return transport;
}

[[nodiscard]] support::JsonValue single_request_body(const tests::ScriptedTransport& transport) {
    REQUIRE(transport.requests.size() == 1);
    auto body = support::read_json(transport.requests.front().body);
    REQUIRE(body);
    return std::move(*body);
}

/// A reasoning Completions model whose identity is the only thing a provider
/// id could change; the wire contract itself is uniform (ADR 0074).
[[nodiscard]] ai::Model provider_id_model(std::string id, std::string provider) {
    auto model = tests::make_model(std::move(id), std::move(provider), "openai-completions");
    model.reasoning = true;
    model.thinking_level_map = ai::ThinkingLevelMap{{ai::ModelThinkingLevel::High, "high"}};
    return model;
}

/// One scripted request for a reasoning model driven at high effort.
struct ScriptedRun {
    std::shared_ptr<tests::ScriptedTransport> transport;
    support::JsonValue body;
};

[[nodiscard]] ScriptedRun run_provider_model(ai::Model model,
        std::uint64_t max_tokens,
        bool with_session_id,
        std::string gateway_url = "https://gateway.internal.example") {
    auto transport = ok_transport();
    auto models = tests::make_scripted_models(model,
            tests::ScriptedTransportOptions{.http_transport = transport},
            {.gateway_url = std::move(gateway_url)});
    REQUIRE(models);

    ai::SimpleStreamOptions options;

    options.max_tokens = max_tokens;
    options.reasoning = ai::ThinkingLevel::High;
    if (with_session_id) {
        options.session_id = "gateway-session";
    }
    const auto run = tests::run_models(*models, model, request_context(), std::move(options));
    REQUIRE(run.result);
    auto body = single_request_body(*transport);
    return ScriptedRun{.transport = std::move(transport), .body = std::move(body)};
}

} // namespace

TEST_CASE("provider identity does not change the Chat Completions body", "[ai][api][completions][provider-identity]") {
    constexpr std::array<std::string_view, 5> providers{
            "deepseek",
            "openrouter",
            "openai",
            "corp-gateway",
            "magpie",
    };
    std::optional<std::string> reference;
    for (const auto provider : providers) {
        const auto run = run_provider_model(provider_id_model("gpt-5", std::string{provider}), 123, true);
        auto serialized = support::write_json(run.body);
        REQUIRE(serialized);
        if (!reference) {
            reference = *serialized;
        }
        CAPTURE(provider);
        CHECK(*serialized == *reference);
        CHECK(run.transport->requests.front().headers.at("X-Magpie-Session") == "gateway-session");
        CHECK_FALSE(run.transport->requests.front().headers.contains("x-session-id"));
    }

    // The unified body carries none of the retired vendor-specific members.
    const auto run = run_provider_model(provider_id_model("gpt-5", "deepseek"), 123, true);
    const auto& raw = run.transport->requests.front().body;
    for (const auto field : {"store",
                 "thinking",
                 "reasoning",
                 "enable_thinking",
                 "cache_control",
                 "strict",
                 "max_tokens",
                 "prompt_cache_retention"}) {
        CHECK(raw.find(std::format("\"{}\"", field)) == std::string::npos);
    }
    CHECK(run.body.at("max_completion_tokens").get_number() == 123);
    CHECK(run.body.at("reasoning_effort").get_string() == "high");
    CHECK(run.body.at("messages").get_array().front().at("role").get_string() == "system");
}

TEST_CASE("the prompt cache key follows the session not the provider id", "[ai][api][completions][provider-identity]") {
    auto run_one = [](std::string provider, std::optional<std::string> session_id, ai::CacheRetention retention) {
        auto transport = ok_transport();
        auto model = tests::make_model("gpt-5", std::move(provider), "openai-completions");
        auto models = tests::make_scripted_models(model,
                tests::ScriptedTransportOptions{.http_transport = transport},
                {.gateway_url = "https://gateway.internal.example"});
        REQUIRE(models);

        ai::SimpleStreamOptions options;

        options.cache_retention = retention;
        options.session_id = std::move(session_id);
        const auto run = tests::run_models(*models, model, request_context(), std::move(options));
        REQUIRE(run.result);
        return single_request_body(*transport);
    };

    for (const auto provider : {"openai", "corp-gateway", "magpie"}) {
        const auto body = run_one(provider, "cache-session", ai::CacheRetention::Short);
        CHECK(body.at("prompt_cache_key").get_string() == "cache-session");
        CHECK_FALSE(body.get_object().contains("prompt_cache_retention"));
    }
    CHECK_FALSE(run_one("openai", std::nullopt, ai::CacheRetention::Short).get_object().contains("prompt_cache_key"));
    CHECK_FALSE(run_one("openai", "cache-session", ai::CacheRetention::None).get_object().contains("prompt_cache_key"));
}

TEST_CASE("session affinity is the gateway session header", "[ai][api][completions][provider-identity]") {
    auto run_one = [](std::optional<std::string> session_id, std::optional<std::string> caller_value) {
        auto transport = ok_transport();
        auto model = tests::make_model("gpt-5.6-luna", "corp-gateway", "openai-completions");
        auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
        REQUIRE(models);

        ai::SimpleStreamOptions options;
        options.max_tokens = 16;
        options.session_id = std::move(session_id);
        if (caller_value) {
            options.headers["X-Magpie-Session"] = *caller_value;
        }
        const auto run = tests::run_models(*models, model, ai::AiContext{}, std::move(options));

        REQUIRE(transport->requests.size() == 1);
        const auto& request = transport->requests.front();
        CHECK(request.url == "http://127.0.0.1:3425/v1/chat/completions");
        CHECK(request.headers.at("Authorization") == "Bearer magpie");
        return request.headers;
    };

    // The session id rides the gateway's own header and no vendor spelling
    // (the retired x-opencode-session / x-session-id) is ever added.
    const auto injected = run_one("session-abc", std::nullopt);
    CHECK(injected.at("X-Magpie-Session") == "session-abc");
    CHECK_FALSE(injected.contains("x-session-id"));
    CHECK_FALSE(injected.contains("x-opencode-session"));

    // A caller-set value wins over the injected one; without a session id the
    // header is absent.
    CHECK(run_one("session-abc", "caller-value").at("X-Magpie-Session") == "caller-value");
    CHECK_FALSE(ai::find_header(run_one(std::nullopt, std::nullopt), "X-Magpie-Session").has_value());

    // An explicit null caller value is a tombstone: the adapter default stays
    // suppressed even when the request carries a session id.
    {
        auto transport = ok_transport();
        auto model = tests::make_model("gpt-5.6-luna", "corp-gateway", "openai-completions");
        auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
        REQUIRE(models);

        ai::SimpleStreamOptions options;
        options.session_id = "session-abc";
        options.headers["x-magpie-session"] = std::nullopt;
        options.headers["content-type"] = std::nullopt;
        options.headers["accept"] = std::nullopt;
        const auto run = tests::run_models(*models, model, ai::AiContext{}, std::move(options));
        REQUIRE(run.result);

        REQUIRE(transport->requests.size() == 1);
        CHECK_FALSE(ai::find_header(transport->requests.front().headers, "X-Magpie-Session").has_value());
        CHECK_FALSE(ai::find_header(transport->requests.front().headers, "Content-Type").has_value());
        CHECK_FALSE(ai::find_header(transport->requests.front().headers, "Accept").has_value());
    }
}

TEST_CASE("an unsupported thinking vocabulary reports the gateway's explicit none",
        "[ai][api][completions][kimi][issue763][spec]") {
    const auto highspeed = kimi_model("kimi-for-coding-highspeed");
    REQUIRE_FALSE(highspeed.id.empty());
    CHECK(highspeed.api == "openai-completions");
    CHECK(highspeed.provider == "kimi-coding");
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

    options.max_tokens = 512;
    options.reasoning = ai::ThinkingLevel::High;
    ai::AiContext context;
    context.tools.push_back(lookup_tool());
    const auto run = tests::run_models(*models, highspeed, std::move(context), std::move(options));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& request = transport->requests.front();
    CHECK(request.url == "http://127.0.0.1:3425/v1/chat/completions");
    CHECK(request.headers.at("Authorization") == "Bearer magpie");
    CHECK_FALSE(request.headers.contains("User-Agent"));
    const auto body = support::read_json(request.body);
    REQUIRE(body);
    CHECK(body->at("max_completion_tokens").get_number() == 512);
    // The clamped request lands on Off, which the gateway contract states as
    // `reasoning_effort: "none"` — never as a vendor effort object.
    CHECK(body->at("reasoning_effort").get_string() == "none");
    CHECK_FALSE(body->get_object().contains("effort"));
    CHECK_FALSE(body->at("tools").get_array().front().at("function").get_object().contains("strict"));
}

TEST_CASE("Chat Completions sends the system instruction role", "[ai][api][completions][kimi][issue757]") {
    // Live-verified Kimi vendor rule (2026-09-23, HTTP 400 "role 'developer' is
    // not allowed"): the completions endpoint only accepts system/user/assistant.
    // The unified gateway contract always sends the system role.
    const auto model = kimi_model("kimi-for-coding");
    REQUIRE_FALSE(model.id.empty());
    REQUIRE(model.reasoning);

    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks =
                    {
                            "data: {\"id\":\"kimi-2\",\"model\":\"kimi-for-coding\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
                            "\"finish_reason\":\"stop\"}]}\n\n"
                            "data: [DONE]\n\n",
                    },
    });
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);

    ai::SimpleStreamOptions options;

    options.max_tokens = 512;
    const auto run = tests::run_models(*models, model, request_context(), std::move(options));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto body = support::read_json(transport->requests.front().body);
    REQUIRE(body);
    const auto& messages = body->at("messages").get_array();
    REQUIRE(messages.size() >= 2);
    CHECK(messages.front().at("role").get_string() == "system");
}

TEST_CASE("Kimi Coding maps every vendor thinking level to a documented effort or the gateway none",
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

            ai::api::PreparedStreamOptions options;
            options.max_tokens = 512;
            options.reasoning = clamped;
            const auto payload = ai::api::build_openai_completions_payload(model, {}, options);
            REQUIRE(payload);
            const auto& object = payload->get_object();
            const auto effort = object.find("reasoning_effort");
            if (clamped == ai::ModelThinkingLevel::Off) {
                REQUIRE(effort != object.end());
                CHECK(effort->second.get_string() == "none");
            } else if (mapped) {
                REQUIRE(effort != object.end());
                CHECK(effort->second.get_string() == *mapped);
            } else {
                CHECK(effort == object.end());
            }
        }

        ai::api::PreparedStreamOptions omitted_options;
        omitted_options.max_tokens = 512;
        const auto omitted = ai::api::build_openai_completions_payload(model, {}, omitted_options);
        REQUIRE(omitted);
        CHECK(omitted->get_object().at("reasoning_effort").get_string() == "none");
    }
}

TEST_CASE("Chat Completions replays reasoning and tool history with the received ids",
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
    ai::api::PreparedStreamOptions options;
    options.max_tokens = 64;

    const auto payload = ai::api::build_openai_completions_payload(model, context, options);

    REQUIRE(payload);
    const auto& messages = payload->at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("role").get_string() == "assistant");
    CHECK(messages[0].at("reasoning_content").get_string() == "plan");
    CHECK(messages[0].at("tool_calls").get_array().front().at("id").get_string() == "call_1|fc_1");
    CHECK(messages[1].at("role").get_string() == "tool");
    CHECK(messages[1].at("tool_call_id").get_string() == "call_1|fc_1");
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

        const auto run = tests::run_models(*models, model, {}, std::move(options));

        REQUIRE(run.result);
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
        REQUIRE_FALSE(run.events.empty());
        CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    }
}

TEST_CASE("Completions maps an unknown finish_reason to one error terminal", "[ai][api][completions][issue1040]") {
    // Retired with the Responses/Anthropic termination matrices (ADR 0074),
    // the unknown-stop-reason regression lock now lives here, on the live
    // Chat Completions fall-through (CompletionsEvents set_finish_reason):
    // a provider reason outside the mapped vocabulary errors and keeps the
    // provider wording visible, it never masquerades as a mapped stop reason.
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .chunks = {std::string{"data: {\"choices\":[{\"delta\":{\"content\":\"some\"},"
                                   "\"finish_reason\":\"future_reason\"}]}\n\n"}},
    });
    const auto model = deepseek_model();
    auto models = tests::make_scripted_models(model, tests::ScriptedTransportOptions{.http_transport = transport});
    REQUIRE(models);
    ai::SimpleStreamOptions options;

    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->error_message == "Provider finish_reason: future_reason");
    CHECK(run.result->raw_stop_reason == std::optional<std::string>{"future_reason"});
    REQUIRE_FALSE(run.events.empty());
    CHECK(std::holds_alternative<ai::AssistantErrorEvent>(run.events.back()));
    CHECK(std::ranges::count_if(run.events, [](const ai::AssistantStreamEvent& event) {
        return std::holds_alternative<ai::AssistantErrorEvent>(event);
    }) == 1);
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

    const auto run = tests::run_models(*models, model, {}, std::move(options));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->error_message == "connection reset");
    CHECK(tests::event_names(run.events) == std::vector<std::string>{"error"});
}
