#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "util/ExpectedMacros.hpp"

#include "../../../include/cch/ai/providers/OpenAIChatClient.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "util/Json.hpp"
#include "../../../include/cch/util/JsonValue.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
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

struct RunResult {
    util::Expected<ai::AssistantMessage> result;
    std::vector<ai::AssistantStreamEvent> events;
};

RunResult run_client(ai::providers::StreamingOpenAIChatClient& client, ai::StreamChatRequest request) {
    boost::asio::io_context io;
    std::optional<util::Expected<ai::AssistantMessage>> result;
    std::vector<ai::AssistantStreamEvent> events;

    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await client.stream(
                request,
                [&](const ai::AssistantStreamEvent& event) {
                    events.push_back(event);
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
    config.api_key = "sk-test-api-key";
    config.base_url = "https://gateway.example/v1";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.system_prompt = "You are concise";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    request.context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        ai::JsonSchema::object({{"path", ai::JsonSchema::string("file path")}}, {"path"}),
    });

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(run.result->response_id == "resp-1");
    REQUIRE(run.result->response_model);
    CHECK(*run.result->response_model == "gpt-test-response");
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Stop);
    REQUIRE(run.result->content.size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(run.result->content[0]));
    CHECK(std::get<ai::TextContent>(run.result->content[0]).text == "hello");

    CHECK(count_events<ai::AssistantStartEvent>(run.events) == 1);
    CHECK(count_events<ai::TextStartEvent>(run.events) == 1);
    CHECK(count_events<ai::TextDeltaEvent>(run.events) == 2);
    CHECK(count_events<ai::TextEndEvent>(run.events) == 1);
    CHECK(count_events<ai::AssistantDoneEvent>(run.events) == 1);

    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests[0].url == "https://gateway.example/v1/chat/completions");
    CHECK(transport->requests[0].headers.at("Accept") == "text/event-stream");
    CHECK(transport->requests[0].body.find(R"("model":"gpt-test")") != std::string::npos);
    CHECK(transport->requests[0].body.find(R"("stream":true)") != std::string::npos);
    CHECK(transport->requests[0].body.find(R"("tools")") != std::string::npos);
}

TEST_CASE("streaming OpenAI client builds Kimi-compatible tool requests offline", "[ai][provider][stream][u2]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "kimi-test-api-key";
    config.base_url = "https://api.kimi.com/coding/v1";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "kimi-for-coding";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello kimi")});
    request.context.tools.push_back(ai::Tool{
        "read_file",
        "Read a workspace file",
        ai::JsonSchema::object({{"path", ai::JsonSchema::string("file path")}}, {"path"}),
    });

    auto run = run_client(client, std::move(request));

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

TEST_CASE("streaming OpenAI client normalizes Kimi trailing slash and serializes prior tool calls", "[ai][provider][stream][u2]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"done"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "kimi-test-api-key";
    config.base_url = "https://api.kimi.com/coding/v1/";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    auto arguments = util::read_json<util::JsonValue>(R"({"path":"README.md"})");
    REQUIRE(arguments);
    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::tool_call_content("call-prev", "read_file", R"({"path":"README.md"})", *arguments));

    ai::StreamChatRequest request;
    request.model = "kimi-for-coding";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("read README")});
    request.context.messages.push_back(ai::MessageVariant{prior_assistant});
    request.context.messages.push_back(ai::MessageVariant{ai::tool_result_message("call-prev", "read_file", "contents")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    REQUIRE(transport->requests.size() == 1);
    const auto& captured = transport->requests[0];
    CHECK(captured.url == "https://api.kimi.com/coding/v1/chat/completions");
    CHECK(captured.body.find(R"("model":"kimi-for-coding")") != std::string::npos);
    CHECK(captured.body.find(R"("tool_calls")") != std::string::npos);
    CHECK(captured.body.find(R"("id":"call-prev")") != std::string::npos);
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
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
}

TEST_CASE("streaming OpenAI client reports malformed provider JSON as typed failure", "[ai][provider][stream][u4][ae3]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {"data: {\"choices\":[}\n\n"};

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result);
    CHECK(run.result.error().code == util::ErrorCode::JsonParse);
    CHECK(count_events<ai::AssistantErrorEvent>(run.events) == 1);
}
