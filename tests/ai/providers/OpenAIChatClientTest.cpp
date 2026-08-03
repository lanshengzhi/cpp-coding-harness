#include "ai/providers/OpenAIChatClient.hpp"
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>
#include "ai/glaze/AiJson.hpp"
#include "support/ComplexToolSchemaFixture.hpp"
#include "support/ModelFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ToolArgumentContracts.hpp"
#include "support/UsageAssertions.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
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

std::string sse(std::string data) {
    return "data: " + std::move(data) + "\n\n";
}

class FakeStreamTransport final : public ai::providers::StreamTransport {
public:
    boost::asio::awaitable<util::Expected<ai::providers::StreamResponse>> async_stream(
        const ai::providers::StreamRequest& request,
        ai::providers::BodyChunkHandler on_body_chunk) override {
        requests.push_back(request);
        if (failure) {
            co_return std::unexpected(*failure);
        }
        ai::providers::StreamResponse response;
        response.head.status_code = 200;
        for (const auto& chunk : chunks) {
            CCH_TRY_VOID(on_body_chunk(chunk));
            response.body += chunk;
        }
        co_return response;
    }

    std::vector<std::string> chunks;
    std::optional<util::Error> failure;
    std::vector<ai::providers::StreamRequest> requests;
};

class CancellableFakeStreamTransport final : public ai::providers::StreamTransport {
public:
    boost::asio::awaitable<util::Expected<ai::providers::StreamResponse>> async_stream(
        const ai::providers::StreamRequest& request,
        ai::providers::BodyChunkHandler /*on_body_chunk*/) override {
        stop_possible = request.stop_token.stop_possible();
        auto executor = co_await boost::asio::this_coro::executor;
        gate.emplace(executor);
        gate->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;
        std::stop_callback cancellation{request.stop_token, [this] {
            if (gate) {
                gate->cancel();
            }
        }};

        boost::system::error_code error;
        co_await gate->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (request.stop_token.stop_requested()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Cancelled,
                "provider request cancelled",
                "transport operation was cancelled"));
        }
        co_return ai::providers::StreamResponse{};
    }

    bool started{false};
    bool stop_possible{false};
    std::optional<boost::asio::steady_timer> gate;
};

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

RunResult run_client(
    ai::providers::StreamingOpenAIChatClient& client,
    ai::StreamChatRequest request,
    std::optional<util::Error> text_delta_failure = std::nullopt,
    std::string api_key = "sk-test-api-key") {
    boost::asio::io_context io;
    std::optional<util::Expected<ai::AssistantMessage>> result;
    std::vector<ai::AssistantStreamEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await client.stream(
                request,
                ai::ModelAuth{.api_key = std::move(api_key)},
                [&](const ai::AssistantStreamEvent& event) {
                    events.push_back(event);
                    if (text_delta_failure && std::holds_alternative<ai::TextDeltaEvent>(event)) {
                        return util::ExpectedVoid{std::unexpected(*text_delta_failure)};
                    }
                    return util::ExpectedVoid{};
                });
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return RunResult{std::move(*result), std::move(events)};
}

template <typename T>
std::size_t count_events(const std::vector<ai::AssistantStreamEvent>& events) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<T>(event)) {
            ++count;
        }
    }
    return count;
}

template <typename T>
std::vector<const T*> events_of(const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<const T*> matches;
    for (const auto& event : events) {
        if (const auto* value = std::get_if<T>(&event)) {
            matches.push_back(value);
        }
    }
    return matches;
}

const ai::AssistantErrorEvent& matching_terminal_error(const RunResult& run) {
    REQUIRE(run.result.has_value());
    const auto errors = events_of<ai::AssistantErrorEvent>(run.events);
    REQUIRE(errors.size() == 1);
    const auto& terminal = *errors[0];
    CHECK(terminal.reason == run.result->stop_reason);
    CHECK(terminal.error.stop_reason == run.result->stop_reason);
    CHECK(terminal.error.error_message == run.result->error_message);
    CHECK(terminal.error.api == run.result->api);
    CHECK(terminal.error.provider == run.result->provider);
    CHECK(terminal.error.model == run.result->model);
    CHECK(terminal.error.response_model == run.result->response_model);
    CHECK(terminal.error.response_id == run.result->response_id);
    CHECK(terminal.error.timestamp == run.result->timestamp);
    CHECK(terminal.error.usage.input == run.result->usage.input);
    CHECK(terminal.error.usage.output == run.result->usage.output);
    CHECK(terminal.error.usage.cache_read == run.result->usage.cache_read);
    CHECK(terminal.error.usage.cache_write == run.result->usage.cache_write);
    CHECK(terminal.error.usage.cache_write_1h == run.result->usage.cache_write_1h);
    CHECK(terminal.error.usage.reasoning == run.result->usage.reasoning);
    CHECK(terminal.error.usage.total_tokens == run.result->usage.total_tokens);
    CHECK(terminal.error.usage.cost.total == run.result->usage.cost.total);
    CHECK(terminal.error.diagnostics.has_value() == run.result->diagnostics.has_value());
    REQUIRE(terminal.error.content.size() == run.result->content.size());
    return terminal;
}

util::JsonValue captured_body_json(const FakeStreamTransport& transport) {
    REQUIRE(transport.requests.size() == 1);
    auto parsed = util::read_json<util::JsonValue>(transport.requests[0].body);
    REQUIRE(parsed);
    return std::move(*parsed);
}

} // namespace

TEST_CASE("streaming OpenAI client serializes typed context and emits text deltas", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"id":"resp-1","model":"gpt-test-response","choices":[{"index":0,"delta":{"role":"assistant"}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"content":"hel"}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"content":"lo"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test", "https://gateway.example/v1");
    request.model.headers = ai::ModelHeaders{{"X-Static", "catalog"}};
    request.context.system_prompt = "You are concise";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    auto expected_contract = util::read_json<util::JsonValue>(tests::kComplexToolArgumentContract);
    REQUIRE(expected_contract);
    request.context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        *expected_contract,
    });
    auto context_json = ai::glaze::write_context_json(request.context);
    REQUIRE(context_json);
    auto restored_context = ai::glaze::read_context_json(*context_json);
    REQUIRE(restored_context);
    request.context = std::move(*restored_context);

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->response_id == "resp-1");
    REQUIRE(run.result->response_model);
    CHECK(*run.result->response_model == "gpt-test-response");
    CHECK(run.result->api == "openai-completions");
    CHECK(run.result->provider == "openai-compatible");
    CHECK(run.result->model == "gpt-test");
    CHECK(run.result->timestamp > 0);
    CHECK(run.result->timestamp != 1718000000);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(run.result->content.size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(run.result->content[0]));
    CHECK(std::get<ai::TextContent>(run.result->content[0]).text == "hello");

    CHECK(count_events<ai::AssistantStartEvent>(run.events) == 1);
    const auto starts = events_of<ai::AssistantStartEvent>(run.events);
    REQUIRE(starts.size() == 1);
    CHECK(starts[0]->partial.api == run.result->api);
    CHECK(starts[0]->partial.provider == run.result->provider);
    CHECK(starts[0]->partial.model == run.result->model);
    CHECK(starts[0]->partial.timestamp == run.result->timestamp);
    const auto deltas = events_of<ai::TextDeltaEvent>(run.events);
    REQUIRE(deltas.size() == 2);
    for (const auto* delta : deltas) {
        CHECK(delta->partial.api == run.result->api);
        CHECK(delta->partial.provider == run.result->provider);
        CHECK(delta->partial.model == run.result->model);
        CHECK(delta->partial.timestamp == run.result->timestamp);
    }
    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    CHECK(done[0]->message.api == run.result->api);
    CHECK(done[0]->message.provider == run.result->provider);
    CHECK(done[0]->message.model == run.result->model);
    CHECK(done[0]->message.response_id == run.result->response_id);
    CHECK(done[0]->message.response_model == run.result->response_model);
    CHECK(done[0]->message.timestamp == run.result->timestamp);
    CHECK(count_events<ai::TextStartEvent>(run.events) == 1);
    CHECK(count_events<ai::TextDeltaEvent>(run.events) == 2);
    CHECK(count_events<ai::TextEndEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 1);

    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests[0].url == "https://gateway.example/v1/chat/completions");
    CHECK(transport->requests[0].headers.at("Accept") == "text/event-stream");
    CHECK(transport->requests[0].headers.at("X-Static") == "catalog");
    auto body = captured_body_json(*transport);
    const auto& root = body.get_object();
    CHECK(root.at("model").get_string() == "gpt-test");
    CHECK(root.at("stream").get_boolean());
    const auto& messages = root.at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("role").get_string() == "system");
    CHECK(messages[0].at("content").get_string() == "You are concise");
    CHECK(messages[1].at("role").get_string() == "user");
    CHECK(messages[1].at("content").get_string() == "hello");
    const auto& tools = root.at("tools").get_array();
    REQUIRE(tools.size() == 1);
    const auto& tool = tools[0].get_object();
    CHECK(tool.at("type").get_string() == "function");
    const auto& function = tool.at("function").get_object();
    auto expected_contract_json = util::write_json(*expected_contract);
    auto outgoing_contract_json = util::write_json(function.at("parameters"));
    REQUIRE(expected_contract_json);
    REQUIRE(outgoing_contract_json);
    CHECK(*outgoing_contract_json == *expected_contract_json);
    const auto& stream_options = root.at("stream_options").get_object();
    CHECK(stream_options.at("include_usage").get_boolean());
}

TEST_CASE(
    "streaming OpenAI client rejects incomplete requested identity before events",
    "[ai][provider][stream][issue19]") {
    const auto rejected = [](std::string api, std::string provider, std::string model) {
        auto transport = std::make_shared<FakeStreamTransport>();
        ai::providers::OpenAIStreamConfig config;
        ai::providers::StreamingOpenAIChatClient client(transport, std::move(config));

        ai::StreamChatRequest request;
        request.model = tests::make_openai_model(std::move(model));
        request.model.api = std::move(api);
        request.model.provider = std::move(provider);
        auto run = run_client(client, std::move(request));

        REQUIRE_FALSE(run.result);
        CHECK(run.result.error().code == util::ErrorCode::Validation);
        CHECK(run.events.empty());
        CHECK(transport->requests.empty());
    };

    rejected("", "openai-compatible", "gpt-test");
    rejected("openai-completions", "", "gpt-test");
    rejected("openai-completions", "openai-compatible", "");
}

TEST_CASE(
    "streaming OpenAI client keeps first meaningful optional response identity",
    "[ai][provider][stream][issue19]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"id":"","model":"","choices":[]})json"),
        sse(R"json({"id":"resp-first","model":"gpt-test","choices":[]})json"),
        sse(R"json({"id":"resp-later","model":"routed-first","choices":[]})json"),
        sse(R"json({"id":"resp-last","model":"routed-later","choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);
    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->model == "gpt-test");
    CHECK(run.result->response_id == "resp-first");
    CHECK(run.result->response_model == "routed-first");
}

TEST_CASE(
    "streaming OpenAI client preserves zero usage when the provider omits usage",
    "[ai][provider][stream][issue17]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    tests::check_zero_usage(run.result->usage);

    const auto starts = events_of<ai::AssistantStartEvent>(run.events);
    REQUIRE(starts.size() == 1);
    tests::check_zero_usage(starts[0]->partial.usage);

    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    tests::check_zero_usage(done[0]->message.usage);
}

TEST_CASE(
    "streaming OpenAI client normalizes standard usage details without double counting reasoning",
    "[ai][provider][stream][issue20]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":30,"total_tokens":999,"prompt_cache_hit_tokens":70,"prompt_tokens_details":{"cached_tokens":40,"cache_write_tokens":10},"completion_tokens_details":{"reasoning_tokens":12}}})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    const ai::Usage expected{
        .input = 50,
        .output = 30,
        .cache_read = 40,
        .cache_write = 10,
        .cache_write_1h = std::nullopt,
        .reasoning = 12,
        .total_tokens = 130,
        .cost = {},
    };
    tests::check_usage(run.result->usage, expected);

    const auto starts = events_of<ai::AssistantStartEvent>(run.events);
    REQUIRE(starts.size() == 1);
    tests::check_zero_usage(starts[0]->partial.usage);

    const auto deltas = events_of<ai::TextDeltaEvent>(run.events);
    REQUIRE(deltas.size() == 1);
    tests::check_usage(deltas[0]->partial.usage, expected);

    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    tests::check_usage(done[0]->message.usage, expected);
}

TEST_CASE(
    "streaming OpenAI client falls back to compatible choice usage",
    "[ai][provider][stream][issue20]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop","usage":{"prompt_tokens":20,"completion_tokens":5,"total_tokens":999,"prompt_cache_hit_tokens":7}}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    const ai::Usage expected{
        .input = 13,
        .output = 5,
        .cache_read = 7,
        .cache_write = 0,
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
        .total_tokens = 25,
        .cost = {},
    };
    tests::check_usage(run.result->usage, expected);

    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    tests::check_usage(done[0]->message.usage, expected);
}

TEST_CASE(
    "streaming OpenAI client normalizes missing usage counters and clamps uncached input",
    "[ai][provider][stream][issue20]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":3,"prompt_tokens_details":{"cached_tokens":5,"cache_write_tokens":4}}})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    tests::check_usage(
        run.result->usage,
        ai::Usage{
            .input = 0,
            .output = 0,
            .cache_read = 5,
            .cache_write = 4,
            .cache_write_1h = std::nullopt,
            .reasoning = std::nullopt,
            .total_tokens = 9,
            .cost = {},
        });
}

TEST_CASE(
    "streaming OpenAI client keeps the latest usage snapshot with standard placement precedence",
    "[ai][provider][stream][issue20]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"a"},"usage":{"prompt_tokens":20,"completion_tokens":5,"prompt_cache_hit_tokens":7}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"content":"b"},"finish_reason":"stop","usage":{"prompt_tokens":900,"completion_tokens":90}}],"usage":{"prompt_tokens":40,"completion_tokens":6,"prompt_tokens_details":{"cached_tokens":10,"cache_write_tokens":4}}})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    const ai::Usage first_snapshot{
        .input = 13,
        .output = 5,
        .cache_read = 7,
        .cache_write = 0,
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
        .total_tokens = 25,
        .cost = {},
    };
    const ai::Usage latest_snapshot{
        .input = 26,
        .output = 6,
        .cache_read = 10,
        .cache_write = 4,
        .cache_write_1h = std::nullopt,
        .reasoning = std::nullopt,
        .total_tokens = 46,
        .cost = {},
    };

    const auto deltas = events_of<ai::TextDeltaEvent>(run.events);
    REQUIRE(deltas.size() == 2);
    tests::check_usage(deltas[0]->partial.usage, first_snapshot);
    tests::check_usage(deltas[1]->partial.usage, latest_snapshot);
    tests::check_usage(run.result->usage, latest_snapshot);

    const auto done = events_of<ai::AssistantDoneEvent>(run.events);
    REQUIRE(done.size() == 1);
    tests::check_usage(done[0]->message.usage, latest_snapshot);
}

TEST_CASE(
    "streaming OpenAI client uses requested Model assistant identity",
    "[ai][provider][stream][u4][issue336]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("kimi-for-coding");
    request.model.provider = "kimi-coding";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->api == "openai-completions");
    CHECK(run.result->provider == "kimi-coding");
    CHECK(run.result->model == "kimi-for-coding");
}

TEST_CASE("streaming OpenAI client builds Kimi-compatible tool requests offline", "[ai][provider][stream][u2]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("kimi-for-coding", "https://api.kimi.com/coding/v1");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello kimi")});
    request.context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        test::path_tool_argument_contract(),
    });

    auto run = run_client(
        client, std::move(request), std::nullopt, "kimi-test-api-key");

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& captured = transport->requests[0];
    CHECK(captured.url == "https://api.kimi.com/coding/v1/chat/completions");
    CHECK(captured.headers.at("Authorization") == "Bearer kimi-test-api-key");
    CHECK(captured.body.find(R"("model":"kimi-for-coding")") != std::string::npos);
    CHECK(captured.body.find(R"("stream":true)") != std::string::npos);
    CHECK(captured.body.find(R"("tools")") != std::string::npos);
    CHECK(captured.body.find(R"("type":"function")") != std::string::npos);
    CHECK(captured.body.find(R"("functions")") == std::string::npos);
    CHECK(captured.body.find(R"("function_call")") == std::string::npos);
    CHECK(captured.body.find(R"("tool_calls")") == std::string::npos);
}

TEST_CASE(
    "streaming OpenAI client keeps runtime authorization above case-variant Model headers",
    "[ai][provider][stream][issue336]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.model.headers = ai::ModelHeaders{{"authorization", "Bearer model-header-value"}};
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(
        client, std::move(request), std::nullopt, "runtime-api-key");

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& captured = transport->requests[0];
    CHECK(captured.headers.at("Authorization") == "Bearer runtime-api-key");
    CHECK(captured.headers.find("authorization") == captured.headers.end());
}

TEST_CASE("streaming OpenAI client normalizes Kimi trailing slash and serializes prior tool calls", "[ai][provider][stream][u2]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"done"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    auto arguments = util::read_json<util::JsonValue>(R"({"path":"README.md"})");
    REQUIRE(arguments);
    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::tool_call_content("call-prev", "read_file", R"({"path":"README.md"})", *arguments));

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("kimi-for-coding", "https://api.kimi.com/coding/v1/");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read README")});
    request.context.messages.push_back(ai::MessageVariant{prior_assistant});
    request.context.messages.push_back(ai::MessageVariant{ai::tool_result_message("call-prev", "read_file", "contents")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& captured = transport->requests[0];
    CHECK(captured.url == "https://api.kimi.com/coding/v1/chat/completions");
    auto body = captured_body_json(*transport);
    const auto& root = body.get_object();
    CHECK(root.at("model").get_string() == "kimi-for-coding");
    const auto& messages = root.at("messages").get_array();
    REQUIRE(messages.size() == 3);
    const auto& assistant = messages[1].get_object();
    CHECK(assistant.at("role").get_string() == "assistant");
    const auto& calls = assistant.at("tool_calls").get_array();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].at("id").get_string() == "call-prev");
    const auto& tool = messages[2].get_object();
    CHECK(tool.at("role").get_string() == "tool");
    CHECK(tool.at("tool_call_id").get_string() == "call-prev");
    CHECK(tool.at("content").get_string() == "contents");
    CHECK(captured.body.find(R"("functions")") == std::string::npos);
}

TEST_CASE("streaming OpenAI client accumulates split tool call arguments", "[ai][provider][stream][u4][ae2]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"pa"}}]}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"th\":\"README.md\"}"}}]},"finish_reason":"tool_calls"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(run.result->content.size() == 1);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(run.result->content[0]));
    const auto& call = std::get<ai::ToolCallContent>(run.result->content[0]);
    CHECK(call.id == "call_1");
    CHECK(call.name == "read_file");
    CHECK(call.raw_arguments == R"({"path":"README.md"})");
    CHECK(call.arguments_valid);
    REQUIRE(call.arguments);
    const auto& args = call.arguments->get<util::JsonValue::object_t>();
    CHECK(args.at("path").get_string() == "README.md");

    CHECK(count_events<ai::ToolCallStartEvent>(run.events) == 1);
    CHECK(count_events<ai::ToolCallDeltaEvent>(run.events) == 2);
    CHECK(count_events<ai::ToolCallEndEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 1);
    auto deltas = events_of<ai::ToolCallDeltaEvent>(run.events);
    REQUIRE(deltas.size() == 2);
    REQUIRE(deltas[0]->partial.content.size() == 1);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(deltas[0]->partial.content[0]));
    CHECK(std::get<ai::ToolCallContent>(deltas[0]->partial.content[0]).raw_arguments == R"({"pa)");
    REQUIRE(deltas[1]->partial.content.size() == 1);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(deltas[1]->partial.content[0]));
    CHECK(std::get<ai::ToolCallContent>(deltas[1]->partial.content[0]).raw_arguments == R"({"path":"README.md"})");
}

TEST_CASE("streaming OpenAI client serializes compat request fields", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.supports_store = true;
    config.compat.supports_developer_role = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test", "https://gateway.example/v1");
    request.context.system_prompt = "You are concise";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& root = body.get_object();
    CHECK(root.at("store").get_boolean() == false);
    CHECK(transport->requests[0].body.find(R"("store":true)") == std::string::npos);
    CHECK(transport->requests[0].body.find(R"("store":false)") != std::string::npos);
    const auto& messages = root.at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("role").get_string() == "developer");
    CHECK(messages[0].at("content").get_string() == "You are concise");
    CHECK(transport->requests[0].body.find(R"("role":"system","content":"You are concise")") == std::string::npos);
}

TEST_CASE("streaming OpenAI client inserts assistant after tool result when compat requires it", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.requires_assistant_after_tool_result = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    auto arguments = util::read_json<util::JsonValue>(R"({"path":"README.md"})");
    REQUIRE(arguments);
    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::tool_call_content("call-prev", "read_file", R"({"path":"README.md"})", *arguments));

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read")});
    request.context.messages.push_back(ai::MessageVariant{prior_assistant});
    request.context.messages.push_back(ai::MessageVariant{ai::tool_result_message("call-prev", "read_file", "contents")});
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("continue")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 5);
    CHECK(messages[2].at("role").get_string() == "tool");
    CHECK(messages[3].at("role").get_string() == "assistant");
    CHECK(messages[3].at("content").get_string() == "I have processed the tool results.");
    CHECK(messages[4].at("role").get_string() == "user");
    CHECK(messages[4].at("content").get_string() == "continue");
}

TEST_CASE("streaming OpenAI client serializes assistant thinking as text when compat requires it", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.requires_thinking_as_text = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::ThinkingContent{
        .thinking = "reasoning",
        .thinking_signature = std::nullopt,
        .redacted = false,
    });
    prior_assistant.content.emplace_back(ai::TextContent{
        .text = "visible",
        .text_signature = std::nullopt,
    });
    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{prior_assistant});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(transport->requests[0].body.find(R"(reasoning\n\nvisible)") != std::string::npos);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].at("content").get_string() == "reasoning\n\nvisible");
}

TEST_CASE(
    "streaming OpenAI client preserves mixed user and custom image content",
    "[ai][provider][stream][u4][issue22]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::UserMessage user;
    user.content.emplace_back(ai::text_content("before"));
    user.content.emplace_back(ai::ImageContent{
        .data = "cG5nLWJ5dGVz",
        .mime_type = "image/png",
    });
    user.content.emplace_back(ai::text_content("between"));
    user.content.emplace_back(ai::ImageContent{
        .data = "d2VicC1ieXRlcw==",
        .mime_type = "image/webp",
    });
    user.content.emplace_back(ai::text_content("after"));

    ai::CustomMessage custom;
    custom.custom_type = "extension-image";
    custom.content.emplace_back(ai::ImageContent{
        .data = "anBlZy1ieXRlcw==",
        .mime_type = "image/jpeg",
    });
    custom.content.emplace_back(ai::text_content("custom caption"));
    custom.content.emplace_back(ai::ImageContent{
        .data = "Z2lmLWJ5dGVz",
        .mime_type = "image/gif",
    });

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(user));
    request.context.messages.emplace_back(std::move(custom));
    request.context.messages.emplace_back(ai::user_text_message("text only"));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 3);

    const auto& user_parts = messages[0].at("content").get_array();
    REQUIRE(user_parts.size() == 5);
    CHECK(user_parts[0].at("type").get_string() == "text");
    CHECK(user_parts[0].at("text").get_string() == "before");
    CHECK(user_parts[1].at("type").get_string() == "image_url");
    CHECK(user_parts[1].at("image_url").at("url").get_string() ==
          "data:image/png;base64,cG5nLWJ5dGVz");
    CHECK(user_parts[2].at("text").get_string() == "between");
    CHECK(user_parts[3].at("image_url").at("url").get_string() ==
          "data:image/webp;base64,d2VicC1ieXRlcw==");
    CHECK(user_parts[4].at("text").get_string() == "after");

    const auto& custom_parts = messages[1].at("content").get_array();
    REQUIRE(custom_parts.size() == 3);
    CHECK(custom_parts[0].at("image_url").at("url").get_string() ==
          "data:image/jpeg;base64,anBlZy1ieXRlcw==");
    CHECK(custom_parts[1].at("text").get_string() == "custom caption");
    CHECK(custom_parts[2].at("image_url").at("url").get_string() ==
          "data:image/gif;base64,Z2lmLWJ5dGVz");

    CHECK(messages[2].at("content").get_string() == "text only");

    const auto& request_body = transport->requests[0].body;
    CHECK(request_body.find("image content omitted") == std::string::npos);
    CHECK(request_body.find("image omitted") == std::string::npos);
    for (const std::string_view data_url : {
             "data:image/png;base64,cG5nLWJ5dGVz",
             "data:image/webp;base64,d2VicC1ieXRlcw==",
             "data:image/jpeg;base64,anBlZy1ieXRlcw==",
             "data:image/gif;base64,Z2lmLWJ5dGVz",
         }) {
        const auto first = request_body.find(data_url);
        REQUIRE(first != std::string::npos);
        CHECK(request_body.find(data_url, first + 1) == std::string::npos);
    }
}

TEST_CASE(
    "streaming OpenAI client keeps tool text attached and batches consecutive tool images",
    "[ai][provider][stream][u4][issue22]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::ToolResultMessage mixed;
    mixed.tool_call_id = "call-mixed";
    mixed.tool_name = "inspect";
    mixed.content.emplace_back(ai::text_content("first text"));
    mixed.content.emplace_back(ai::ImageContent{
        .data = "Zmlyc3QtaW1hZ2U=",
        .mime_type = "image/png",
    });
    mixed.content.emplace_back(ai::text_content(""));
    mixed.content.emplace_back(ai::text_content("second text"));
    mixed.content.emplace_back(ai::ImageContent{
        .data = "c2Vjb25kLWltYWdl",
        .mime_type = "image/webp",
    });

    ai::ToolResultMessage image_only;
    image_only.tool_call_id = "call-image";
    image_only.tool_name = "capture";
    image_only.content.emplace_back(ai::ImageContent{
        .data = "dGhpcmQtaW1hZ2U=",
        .mime_type = "image/jpeg",
    });

    ai::ToolResultMessage empty;
    empty.tool_call_id = "call-empty";
    empty.tool_name = "noop";

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(mixed));
    request.context.messages.emplace_back(std::move(image_only));
    request.context.messages.emplace_back(std::move(empty));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].at("role").get_string() == "tool");
    CHECK(messages[0].at("tool_call_id").get_string() == "call-mixed");
    CHECK(messages[0].at("content").get_string() == "first text\n\nsecond text");
    CHECK(messages[1].at("role").get_string() == "tool");
    CHECK(messages[1].at("tool_call_id").get_string() == "call-image");
    CHECK(messages[1].at("content").get_string() == "(see attached image)");
    CHECK(messages[2].at("role").get_string() == "tool");
    CHECK(messages[2].at("tool_call_id").get_string() == "call-empty");
    CHECK(messages[2].at("content").get_string() == "(no tool output)");

    CHECK(messages[3].at("role").get_string() == "user");
    const auto& attachment_parts = messages[3].at("content").get_array();
    REQUIRE(attachment_parts.size() == 4);
    CHECK(attachment_parts[0].at("type").get_string() == "text");
    CHECK(attachment_parts[0].at("text").get_string() ==
          "Attached image(s) from tool result:");
    CHECK(attachment_parts[1].at("image_url").at("url").get_string() ==
          "data:image/png;base64,Zmlyc3QtaW1hZ2U=");
    CHECK(attachment_parts[2].at("image_url").at("url").get_string() ==
          "data:image/webp;base64,c2Vjb25kLWltYWdl");
    CHECK(attachment_parts[3].at("image_url").at("url").get_string() ==
          "data:image/jpeg;base64,dGhpcmQtaW1hZ2U=");
}

TEST_CASE(
    "streaming OpenAI client bridges grouped tool images when compatibility requires it",
    "[ai][provider][stream][u4][issue22]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.requires_assistant_after_tool_result = true;
    config.compat.requires_tool_result_name = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::ToolResultMessage tool;
    tool.tool_call_id = "call-image";
    tool.tool_name = "capture";
    tool.content.emplace_back(ai::ImageContent{
        .data = "aW1hZ2U=",
        .mime_type = "image/png",
    });

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(tool));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].at("role").get_string() == "tool");
    CHECK(messages[0].at("name").get_string() == "capture");
    CHECK(messages[1].at("role").get_string() == "assistant");
    CHECK(messages[1].at("content").get_string() ==
          "I have processed the tool results.");
    CHECK(messages[2].at("role").get_string() == "user");
    const auto& attachment_parts = messages[2].at("content").get_array();
    REQUIRE(attachment_parts.size() == 2);
    const auto& attached_image = attachment_parts[1];
    CHECK(attached_image.at("image_url").at("url").get_string() ==
          "data:image/png;base64,aW1hZ2U=");
}

TEST_CASE(
    "streaming OpenAI client groups tool images after excluding model-hidden messages",
    "[ai][provider][stream][u4][issue29]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.requires_assistant_after_tool_result = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::ToolResultMessage first_tool;
    first_tool.tool_call_id = "call-first";
    first_tool.tool_name = "capture";
    first_tool.content.emplace_back(ai::text_content("first capture"));
    first_tool.content.emplace_back(ai::ImageContent{
        .data = "Zmlyc3Q=",
        .mime_type = "image/png",
    });

    ai::BashExecutionMessage excluded_bash;
    excluded_bash.command = "cat hidden-command";
    excluded_bash.output = "HIDDEN_OUTPUT";
    excluded_bash.exclude_from_context = true;

    ai::ToolResultMessage second_tool;
    second_tool.tool_call_id = "call-second";
    second_tool.tool_name = "capture";
    second_tool.content.emplace_back(ai::ImageContent{
        .data = "c2Vjb25k",
        .mime_type = "image/webp",
    });

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(first_tool));
    request.context.messages.emplace_back(std::move(excluded_bash));
    request.context.messages.emplace_back(std::move(second_tool));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 4);
    CHECK(messages[0].at("role").get_string() == "tool");
    CHECK(messages[0].at("tool_call_id").get_string() == "call-first");
    CHECK(messages[0].at("content").get_string() == "first capture");
    CHECK(messages[1].at("role").get_string() == "tool");
    CHECK(messages[1].at("tool_call_id").get_string() == "call-second");
    CHECK(messages[1].at("content").get_string() == "(see attached image)");
    CHECK(messages[2].at("role").get_string() == "assistant");
    CHECK(messages[2].at("content").get_string() ==
          "I have processed the tool results.");
    CHECK(messages[3].at("role").get_string() == "user");

    const auto& attachment_parts = messages[3].at("content").get_array();
    REQUIRE(attachment_parts.size() == 3);
    CHECK(attachment_parts[1].at("image_url").at("url").get_string() ==
          "data:image/png;base64,Zmlyc3Q=");
    CHECK(attachment_parts[2].at("image_url").at("url").get_string() ==
          "data:image/webp;base64,c2Vjb25k");

    const auto& request_body = transport->requests[0].body;
    CHECK(request_body.find("hidden-command") == std::string::npos);
    CHECK(request_body.find("HIDDEN_OUTPUT") == std::string::npos);
}

TEST_CASE(
    "streaming OpenAI client lets model-facing messages split tool image groups",
    "[ai][provider][stream][u4][issue29]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::ToolResultMessage first_tool;
    first_tool.tool_call_id = "call-first";
    first_tool.tool_name = "capture";
    first_tool.content.emplace_back(ai::ImageContent{
        .data = "Zmlyc3Q=",
        .mime_type = "image/png",
    });

    ai::BashExecutionMessage visible_bash;
    visible_bash.command = "printf visible";
    visible_bash.output = "visible output";

    ai::ToolResultMessage second_tool;
    second_tool.tool_call_id = "call-second";
    second_tool.tool_name = "capture";
    second_tool.content.emplace_back(ai::ImageContent{
        .data = "c2Vjb25k",
        .mime_type = "image/webp",
    });

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(first_tool));
    request.context.messages.emplace_back(std::move(visible_bash));
    request.context.messages.emplace_back(std::move(second_tool));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 5);
    CHECK(messages[0].at("role").get_string() == "tool");
    CHECK(messages[1].at("role").get_string() == "user");
    CHECK(messages[2].at("role").get_string() == "user");
    CHECK(messages[2].at("content").get_string().find("printf visible") !=
          std::string::npos);
    CHECK(messages[3].at("role").get_string() == "tool");
    CHECK(messages[4].at("role").get_string() == "user");

    const auto& first_attachment = messages[1].at("content").get_array();
    REQUIRE(first_attachment.size() == 2);
    CHECK(first_attachment[1].at("image_url").at("url").get_string() ==
          "data:image/png;base64,Zmlyc3Q=");
    const auto& second_attachment = messages[4].at("content").get_array();
    REQUIRE(second_attachment.size() == 2);
    CHECK(second_attachment[1].at("image_url").at("url").get_string() ==
          "data:image/webp;base64,c2Vjb25k");
}

TEST_CASE(
    "resumed image messages produce the same OpenAI request as fresh content",
    "[ai][provider][stream][session][issue22][issue28]") {
    ai::UserMessage user;
    user.content.emplace_back(ai::text_content("compare"));
    user.content.emplace_back(ai::ImageContent{
        .data = "dXNlci1pbWFnZQ==",
        .mime_type = "image/png",
    });

    ai::CustomMessage custom;
    custom.custom_type = "extension-image";
    custom.content.emplace_back(ai::ImageContent{
        .data = "Y3VzdG9tLWltYWdl",
        .mime_type = "image/webp",
    });
    custom.content.emplace_back(ai::text_content("custom context"));

    ai::AssistantMessage assistant;
    assistant.api = "openai-completions";
    assistant.provider = "openai-compatible";
    assistant.model = "gpt-test";
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1784678400000;
    assistant.content.emplace_back(ai::tool_call_content("call-one", "capture", "{}"));
    assistant.content.emplace_back(ai::tool_call_content("call-two", "capture", "{}"));

    ai::ToolResultMessage first_tool;
    first_tool.tool_call_id = "call-one";
    first_tool.tool_name = "capture";
    first_tool.content.emplace_back(ai::text_content("first capture"));
    first_tool.content.emplace_back(ai::ImageContent{
        .data = "dG9vbC1pbWFnZS0x",
        .mime_type = "image/jpeg",
    });

    ai::ToolResultMessage second_tool;
    second_tool.tool_call_id = "call-two";
    second_tool.tool_name = "capture";
    second_tool.content.emplace_back(ai::ImageContent{
        .data = "dG9vbC1pbWFnZS0y",
        .mime_type = "image/gif",
    });

    std::vector<ai::MessageVariant> fresh_messages;
    fresh_messages.emplace_back(std::move(user));
    fresh_messages.emplace_back(std::move(custom));
    fresh_messages.emplace_back(std::move(assistant));
    fresh_messages.emplace_back(std::move(first_tool));
    fresh_messages.emplace_back(std::move(second_tool));

    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "image-resume.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(
        session_path,
        harness::session::SessionMetadata{
            .session_id = "image-resume",
            .created_at = "2026-07-22T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "openai-compatible",
            .model = "gpt-test",
        });
    REQUIRE(store);
    REQUIRE(store->append(fresh_messages[0]));
    const auto& persisted_custom = std::get<ai::CustomMessage>(fresh_messages[1]);
    std::vector<harness::session::CustomMessageEntryContentBlock> persisted_custom_content;
    persisted_custom_content.emplace_back(ai::ImageContent{
        .data = "Y3VzdG9tLWltYWdl",
        .mime_type = "image/webp",
    });
    persisted_custom_content.emplace_back(ai::text_content("custom context"));
    REQUIRE(store->append_custom_message_entry(
        std::nullopt,
        persisted_custom.custom_type,
        std::move(persisted_custom_content),
        persisted_custom.display,
        persisted_custom.details));
    for (std::size_t i = 2; i < fresh_messages.size(); ++i) {
        REQUIRE(store->append(fresh_messages[i]));
    }

    auto persisted = harness::session::JsonlSessionStore::load(session_path);
    REQUIRE(persisted);
    REQUIRE(persisted->entries.size() == fresh_messages.size() + 1);
    CHECK(persisted->entries[2].kind == harness::session::SessionEntryKind::CustomMessage);

    auto resumed = harness::session::resume_session(session_path);
    REQUIRE(resumed);
    REQUIRE(resumed->topology == harness::session::SessionTopology::Linear);
    REQUIRE(resumed->history.size() == fresh_messages.size());

    auto fresh_transport = std::make_shared<FakeStreamTransport>();
    fresh_transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };
    ai::providers::OpenAIStreamConfig fresh_config;
    ai::providers::StreamingOpenAIChatClient fresh_client(fresh_transport, fresh_config);
    ai::StreamChatRequest fresh_request;
    fresh_request.model = tests::make_openai_model("gpt-test");
    fresh_request.context.messages = fresh_messages;
    auto fresh_run = run_client(fresh_client, std::move(fresh_request));
    REQUIRE(fresh_run.result);

    auto resumed_transport = std::make_shared<FakeStreamTransport>();
    resumed_transport->chunks = fresh_transport->chunks;
    ai::providers::OpenAIStreamConfig resumed_config;
    ai::providers::StreamingOpenAIChatClient resumed_client(resumed_transport, resumed_config);
    ai::StreamChatRequest resumed_request;
    resumed_request.model = tests::make_openai_model("gpt-test");
    resumed_request.context.messages = std::move(resumed->history);
    auto resumed_run = run_client(resumed_client, std::move(resumed_request));
    REQUIRE(resumed_run.result);

    REQUIRE(fresh_transport->requests.size() == 1);
    REQUIRE(resumed_transport->requests.size() == 1);
    CHECK(resumed_transport->requests[0].body == fresh_transport->requests[0].body);
}

TEST_CASE("streaming OpenAI client accepts standard streaming chunk fields", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"id":"chatcmpl-1","object":"chat.completion.chunk","created":1718000000,"model":"gpt-test-response","system_fingerprint":"fp-test","choices":[{"index":0,"delta":{"content":"ok"},"logprobs":null,"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(ai::text_from_assistant_content(run.result->content) == "ok");
    CHECK(run.result->timestamp > 0);
    CHECK(run.result->timestamp != 1718000000);
    REQUIRE(run.result->response_model);
    CHECK(*run.result->response_model == "gpt-test-response");
}

TEST_CASE(
    "streaming OpenAI client uses the requested Model base URL",
    "[ai][provider][stream][u4][issue336]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests[0].url == "https://api.openai.com/v1/chat/completions");
}

TEST_CASE("streaming OpenAI client skips excluded bash execution messages", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::BashExecutionMessage bash;
    bash.command = "cat secret.txt";
    bash.output = "SECRET";
    bash.exclude_from_context = true;

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{bash});
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("visible")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(transport->requests[0].body.find("visible") != std::string::npos);
    CHECK(transport->requests[0].body.find("cat secret.txt") == std::string::npos);
    CHECK(transport->requests[0].body.find("SECRET") == std::string::npos);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].at("content").get_string() == "visible");
}

TEST_CASE("streaming OpenAI client serializes tool result names when compat requires it", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.compat.requires_tool_result_name = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::tool_result_message("call-prev", "read_file", "contents")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].at("role").get_string() == "tool");
    CHECK(messages[0].at("tool_call_id").get_string() == "call-prev");
    CHECK(messages[0].at("name").get_string() == "read_file");
}

TEST_CASE("streaming OpenAI client keeps malformed streamed tool arguments as invalid call state", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_bad","type":"function","function":{"name":"read_file","arguments":"{\"path\":"}}]},"finish_reason":"tool_calls"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->stop_reason == ai::AssistantStopReason::ToolUse);
    REQUIRE(run.result->content.size() == 1);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(run.result->content[0]));
    const auto& call = std::get<ai::ToolCallContent>(run.result->content[0]);
    CHECK(call.arguments == std::nullopt);
    CHECK_FALSE(call.arguments_valid);
    REQUIRE(call.argument_error);
    CHECK_FALSE(call.argument_error->empty());
    auto ends = events_of<ai::ToolCallEndEvent>(run.events);
    REQUIRE(ends.size() == 1);
    CHECK(ends[0]->tool_call.arguments == std::nullopt);
    CHECK_FALSE(ends[0]->tool_call.arguments_valid);
    REQUIRE(ends[0]->tool_call.argument_error);
    CHECK_FALSE(ends[0]->tool_call.argument_error->empty());
}

TEST_CASE(
    "streaming OpenAI client rejects missing credentials before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(
        client, std::move(request), std::nullopt, "");

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Provider);
    CHECK(run.result.error().message == "missing API key");
    CHECK(transport->requests.empty());
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client rejects a missing transport capability before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client({}, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "missing stream transport");
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client rejects an empty effective model before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = ai::Model{};
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "invalid model");
    CHECK(transport->requests.empty());
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client rejects a non-positive timeout before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    config.timeout = std::chrono::milliseconds{0};
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "request timeout must be positive");
    CHECK(transport->requests.empty());
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client rejects request serialization failure before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    std::string invalid_text = "invalid UTF-8: ";
    invalid_text.push_back(static_cast<char>(0xFF));

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{
        ai::user_text_message(std::move(invalid_text))});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::JsonSerialize);
    CHECK(run.result.error().message == "failed to serialize OpenAI request");
    CHECK(transport->requests.empty());
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client serializes valid Unicode before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{
        ai::user_text_message("こんにちは 🌍")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    CHECK(transport->requests.size() == 1);
    CHECK(transport->requests[0].body.find("こんにちは 🌍") != std::string::npos);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 1);
}

TEST_CASE(
    "streaming OpenAI client normalizes supported finish reason aliases",
    "[ai][provider][stream][issue18]") {
    const std::vector<std::pair<std::string, ai::AssistantStopReason>> cases{
        {"stop", ai::AssistantStopReason::Stop},
        {"end", ai::AssistantStopReason::Stop},
        {"length", ai::AssistantStopReason::Length},
        {"function_call", ai::AssistantStopReason::ToolUse},
        {"tool_calls", ai::AssistantStopReason::ToolUse},
    };

    for (const auto& [finish_reason, expected] : cases) {
        auto transport = std::make_shared<FakeStreamTransport>();
        transport->chunks = {
            sse("{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"" +
                finish_reason + "\"}]}"),
            sse("[DONE]"),
        };
        ai::providers::OpenAIStreamConfig config;
        ai::providers::StreamingOpenAIChatClient client(transport, config);
        ai::StreamChatRequest request;
        request.model = tests::make_openai_model("gpt-test");
        request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

        auto run = run_client(client, std::move(request));

        REQUIRE(run.result.has_value());
        CHECK(run.result->stop_reason == expected);
        CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 1);
        CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 0);
    }
}

TEST_CASE(
    "streaming OpenAI client fails closed on unsupported finish reasons",
    "[ai][provider][stream][issue18]") {
    const std::vector<std::string> finish_reasons{
        "content_filter", "network_error", "safety_policy", "future_reason"};

    for (const auto& finish_reason : finish_reasons) {
        auto transport = std::make_shared<FakeStreamTransport>();
        transport->chunks = {
            sse("{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"" +
                finish_reason + "\"}]}"),
            sse("[DONE]"),
        };
        ai::providers::OpenAIStreamConfig config;
        ai::providers::StreamingOpenAIChatClient client(transport, config);
        ai::StreamChatRequest request;
        request.model = tests::make_openai_model("gpt-test");
        request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

        auto run = run_client(client, std::move(request));

        REQUIRE(run.result.has_value());
        CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
        REQUIRE(run.result->error_message.has_value());
        CHECK(*run.result->error_message == "Provider finish_reason: " + finish_reason);
        CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);
        CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
        const auto& terminal = matching_terminal_error(run);
        CHECK(terminal.error.error_message == run.result->error_message);
    }
}

TEST_CASE(
    "streaming OpenAI client does not infer success without a non-null finish reason",
    "[ai][provider][stream][issue18]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"partial"},"finish_reason":null}]})json"),
        sse("[DONE]"),
    };
    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);
    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(run.result->error_message->find("non-null finish_reason") != std::string::npos);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
}

TEST_CASE(
    "streaming OpenAI client completes an accepted image request failure as one error message",
    "[ai][provider][stream][issue11][issue14][issue22]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->failure = util::make_error(
        util::ErrorCode::Network,
        "provider connection failed",
        "could not resolve api.example: Name or service not known");

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::UserMessage user;
    user.content.emplace_back(ai::text_content("inspect"));
    user.content.emplace_back(ai::ImageContent{
        .data = "aW1hZ2U=",
        .mime_type = "image/png",
    });
    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.emplace_back(std::move(user));

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == "could not resolve api.example: Name or service not known");
    CHECK(transport->requests[0].body.find("data:image/png;base64,aW1hZ2U=") !=
          std::string::npos);
    CHECK(run.result->api == "openai-completions");
    CHECK(run.result->provider == "openai-compatible");
    CHECK(run.result->model == "gpt-test");
    CHECK(run.result->content.empty());

    CHECK(transport->requests.size() == 1);
    CHECK(count_events<ai::AssistantStartEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);

    const auto& terminal_event = matching_terminal_error(run);
    CHECK(terminal_event.reason == ai::AssistantStopReason::Error);
    CHECK(terminal_event.error.content.empty());
    REQUIRE(terminal_event.failure);
    CHECK(terminal_event.failure->code == util::ErrorCode::Stream);
}

TEST_CASE(
    "streaming OpenAI client redacts provider error detail before bounding model-visible text",
    "[ai][provider][stream][issue72]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    const std::string secret = "sk-provider-body-secret-123456";
    transport->failure = util::make_error(
        util::ErrorCode::Provider,
        "provider returned non-success HTTP status",
        "401: upstream rejected " + secret + " " + std::string(5000, 'x'));

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    REQUIRE(run.result->error_message.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    CHECK(run.result->error_message->find(secret) == std::string::npos);
    CHECK(run.result->error_message->find("[REDACTED]") != std::string::npos);
    CHECK(run.result->error_message->size() <= 4096);
    const auto& terminal = matching_terminal_error(run);
    CHECK(terminal.error.error_message == run.result->error_message);
}

TEST_CASE(
    "streaming OpenAI client propagates prompt cancellation through its transport",
    "[ai][provider][stream][abort][issue39]") {
    auto transport = std::make_shared<CancellableFakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    std::stop_source stop_source;
    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.stop_token = stop_source.get_token();
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    boost::asio::io_context io;
    std::optional<util::Expected<ai::AssistantMessage>> result;
    std::vector<ai::AssistantStreamEvent> events;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await client.stream(
                request,
                ai::ModelAuth{.api_key = "sk-test-api-key"},
                [&](const ai::AssistantStreamEvent& event) -> util::ExpectedVoid {
                    events.push_back(event);
                    return {};
                });
            co_return;
        },
        boost::asio::detached);

    while (!transport->started) {
        REQUIRE(io.poll_one() == 1);
    }
    CHECK(transport->stop_possible);
    CHECK(stop_source.request_stop());
    if (io.stopped()) {
        io.restart();
    }
    io.run();

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK((*result)->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE((*result)->error_message.has_value());
    CHECK(*(*result)->error_message == "transport operation was cancelled");
    CHECK(count_events<ai::AssistantStartEvent>(events) == 1);
    CHECK(count_events<ai::AssistantErrorEvent>(events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(events) == 0);
}

TEST_CASE(
    "streaming OpenAI client completes a cancellation-class transport failure as one aborted message",
    "[ai][provider][stream][issue13]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->failure = util::make_error(
        util::ErrorCode::Cancelled,
        "provider request cancelled",
        "transport operation was cancelled");

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Aborted);
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == "transport operation was cancelled");
    CHECK(run.result->api == "openai-completions");
    CHECK(run.result->provider == "openai-compatible");
    CHECK(run.result->model == "gpt-test");
    CHECK(run.result->content.empty());

    CHECK(transport->requests.size() == 1);
    CHECK(count_events<ai::AssistantStartEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);

    const auto& terminal_event = matching_terminal_error(run);
    CHECK(terminal_event.reason == ai::AssistantStopReason::Aborted);
    CHECK(terminal_event.error.stop_reason == ai::AssistantStopReason::Aborted);
    CHECK(terminal_event.error.content.empty());
    REQUIRE(terminal_event.failure);
    CHECK(terminal_event.failure->code == util::ErrorCode::Cancelled);
}

TEST_CASE(
    "streaming OpenAI client keeps consumer sink failures outside the assistant outcome",
    "[ai][provider][stream][issue11][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"hello"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    const auto sink_failure = util::make_error(
        util::ErrorCode::Session,
        "consumer event sink failed",
        "synthetic subscriber infrastructure error");

    auto run = run_client(client, std::move(request), sink_failure);

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Session);
    CHECK(run.result.error().message == "consumer event sink failed");
    CHECK(run.result.error().detail == "synthetic subscriber infrastructure error");
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 0);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);
    CHECK(transport->requests.size() == 1);
}

TEST_CASE(
    "streaming OpenAI client preserves partial content when the stream ends before a terminal marker",
    "[ai][provider][stream][issue12]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"reasoning":"considering"}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"content":"useful text"}}]})json"),
        sse(R"json({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_partial","type":"function","function":{"name":"echo","arguments":"{\"value\":\"par"}}]}}]})json"),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.model.provider = "opencode-go";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(run.result->error_message->find("without a non-null finish_reason") != std::string::npos);
    REQUIRE(run.result->content.size() == 3);

    REQUIRE(std::holds_alternative<ai::ThinkingContent>(run.result->content[0]));
    const auto& thinking = std::get<ai::ThinkingContent>(run.result->content[0]);
    CHECK(thinking.thinking == "considering");
    REQUIRE(thinking.thinking_signature.has_value());
    CHECK(*thinking.thinking_signature == "reasoning_content");

    REQUIRE(std::holds_alternative<ai::TextContent>(run.result->content[1]));
    CHECK(std::get<ai::TextContent>(run.result->content[1]).text == "useful text");

    REQUIRE(std::holds_alternative<ai::ToolCallContent>(run.result->content[2]));
    const auto& tool_call = std::get<ai::ToolCallContent>(run.result->content[2]);
    CHECK(tool_call.id == "call_partial");
    CHECK(tool_call.name == "echo");
    CHECK(tool_call.raw_arguments == R"({"value":"par)");
    CHECK_FALSE(tool_call.arguments.has_value());
    CHECK_FALSE(tool_call.arguments_valid);
    REQUIRE(tool_call.argument_error.has_value());

    CHECK(count_events<ai::ThinkingStartEvent>(run.events) == 1);
    CHECK(count_events<ai::ThinkingDeltaEvent>(run.events) == 1);
    CHECK(count_events<ai::ThinkingEndEvent>(run.events) == 1);
    CHECK(count_events<ai::TextStartEvent>(run.events) == 1);
    CHECK(count_events<ai::TextDeltaEvent>(run.events) == 1);
    CHECK(count_events<ai::TextEndEvent>(run.events) == 1);
    CHECK(count_events<ai::ToolCallStartEvent>(run.events) == 1);
    CHECK(count_events<ai::ToolCallDeltaEvent>(run.events) == 1);
    CHECK(count_events<ai::ToolCallEndEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);

    const auto& terminal_event = matching_terminal_error(run);
    CHECK(std::get<ai::ThinkingContent>(terminal_event.error.content[0]).thinking == thinking.thinking);
    CHECK(std::get<ai::TextContent>(terminal_event.error.content[1]).text == "useful text");
    const auto& event_tool_call = std::get<ai::ToolCallContent>(terminal_event.error.content[2]);
    CHECK(event_tool_call.id == tool_call.id);
    CHECK(event_tool_call.name == tool_call.name);
    CHECK(event_tool_call.raw_arguments == tool_call.raw_arguments);
    CHECK(event_tool_call.arguments_valid == tool_call.arguments_valid);
    CHECK(event_tool_call.argument_error == tool_call.argument_error);
}

TEST_CASE(
    "streaming OpenAI client preserves partial text after malformed SSE",
    "[ai][provider][stream][issue12]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"kept"}}]})json"),
        std::string((8 * 1024 * 1024) + 1, 'x'),
    };

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(run.result->error_message->find("unbounded SSE line") != std::string::npos);
    REQUIRE(run.result->content.size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(run.result->content[0]));
    CHECK(std::get<ai::TextContent>(run.result->content[0]).text == "kept");
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);

    const auto& terminal_event = matching_terminal_error(run);
    CHECK(std::get<ai::TextContent>(terminal_event.error.content[0]).text == "kept");
    REQUIRE(terminal_event.failure);
    CHECK(terminal_event.failure->code == util::ErrorCode::Stream);
}

TEST_CASE(
    "streaming OpenAI client completes malformed provider JSON before content as an error message",
    "[ai][provider][stream][issue12]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {"data: {\"choices\":[}\n\n"};

    ai::providers::OpenAIStreamConfig config;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = tests::make_openai_model("gpt-test");
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK_FALSE(run.result->error_message->empty());
    CHECK(run.result->content.empty());
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 0);

    const auto& terminal_event = matching_terminal_error(run);
    CHECK(terminal_event.error.content.empty());
    REQUIRE(terminal_event.failure);
    CHECK(terminal_event.failure->code == util::ErrorCode::Stream);
}
