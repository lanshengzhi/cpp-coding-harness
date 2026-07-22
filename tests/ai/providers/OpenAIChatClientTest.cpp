#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "util/ExpectedMacros.hpp"

#include "../../../include/cch/ai/providers/OpenAIChatClient.hpp"
#include "../../../include/cch/util/Error.hpp"
#include "util/Json.hpp"
#include "../../../include/cch/util/JsonValue.hpp"
#include "../../support/UsageAssertions.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
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

RunResult run_client(
    ai::providers::StreamingOpenAIChatClient& client,
    ai::StreamChatRequest request,
    std::optional<util::Error> text_delta_failure = std::nullopt) {
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
    const auto& stream_options = root.at("stream_options").get_object();
    CHECK(stream_options.at("include_usage").get_boolean());
}

TEST_CASE(
    "streaming OpenAI client rejects incomplete requested identity before events",
    "[ai][provider][stream][issue19]") {
    const auto rejected = [](std::string api, std::string provider, std::string model) {
        auto transport = std::make_shared<FakeStreamTransport>();
        ai::providers::OpenAIStreamConfig config;
        config.api_key = "sk-test-api-key";
        config.api = std::move(api);
        config.provider = std::move(provider);
        config.model = model;
        ai::providers::StreamingOpenAIChatClient client(transport, std::move(config));

        ai::StreamChatRequest request;
        request.model = std::move(model);
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);
    ai::StreamChatRequest request;
    request.model = "gpt-test";

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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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

TEST_CASE("streaming OpenAI client uses configured assistant identity", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    config.api = "openai-completions";
    config.provider = "kimi-coding";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "kimi-for-coding";
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
    config.api_key = "sk-test-api-key";
    config.compat.supports_store = true;
    config.compat.supports_developer_role = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    config.compat.requires_assistant_after_tool_result = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    auto arguments = util::read_json<util::JsonValue>(R"({"path":"README.md"})");
    REQUIRE(arguments);
    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::tool_call_content("call-prev", "read_file", R"({"path":"README.md"})", *arguments));

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    config.compat.requires_thinking_as_text = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::AssistantMessage prior_assistant;
    prior_assistant.content.emplace_back(ai::ThinkingContent{"reasoning", std::nullopt, false});
    prior_assistant.content.emplace_back(ai::TextContent{"visible", std::nullopt});
    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.messages.push_back(ai::MessageVariant{prior_assistant});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(transport->requests[0].body.find(R"(reasoning\n\nvisible)") != std::string::npos);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].at("content").get_string() == "reasoning\n\nvisible");
}

TEST_CASE("streaming OpenAI client serializes non-text content placeholders", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::UserMessage user;
    user.content.emplace_back(ai::ImageContent{"ZmFrZQ==", "image/png"});
    auto tool = ai::tool_result_message("call-prev", "read_file", "");
    tool.content.clear();
    tool.content.emplace_back(ai::ImageContent{"ZmFrZQ==", "image/png"});

    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.messages.push_back(ai::MessageVariant{user});
    request.context.messages.push_back(ai::MessageVariant{tool});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(transport->requests[0].body.find("[image content omitted: image/png]") != std::string::npos);
    auto body = captured_body_json(*transport);
    const auto& messages = body.at("messages").get_array();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].at("content").get_string() == "[image content omitted: image/png]");
    CHECK(messages[1].at("content").get_string() == "[image content omitted: image/png]");
}

TEST_CASE("streaming OpenAI client accepts standard streaming chunk fields", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"id":"chatcmpl-1","object":"chat.completion.chunk","created":1718000000,"model":"gpt-test-response","system_fingerprint":"fp-test","choices":[{"index":0,"delta":{"content":"ok"},"logprobs":null,"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result);
    CHECK(ai::text_from_assistant_content(run.result->content) == "ok");
    CHECK(run.result->timestamp > 0);
    CHECK(run.result->timestamp != 1718000000);
    REQUIRE(run.result->response_model);
    CHECK(*run.result->response_model == "gpt-test-response");
}

TEST_CASE("streaming OpenAI client preserves default base URL when config base is empty", "[ai][provider][stream][u4]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {
        sse(R"json({"choices":[{"index":0,"delta":{"content":"ok"},"finish_reason":"stop"}]})json"),
        sse("[DONE]"),
    };

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    config.base_url = "";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::BashExecutionMessage bash;
    bash.command = "cat secret.txt";
    bash.output = "SECRET";
    bash.exclude_from_context = true;

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    config.compat.requires_tool_result_name = true;
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key.clear();
    config.api_key_env.clear();
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client({}, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    config.model.clear();
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model.clear();
    request.context.model.clear();
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE_FALSE(run.result.has_value());
    CHECK(run.result.error().code == util::ErrorCode::Validation);
    CHECK(run.result.error().message == "model is required");
    CHECK(transport->requests.empty());
    CHECK(run.events.empty());
}

TEST_CASE(
    "streaming OpenAI client rejects a non-positive timeout before provider-call acceptance",
    "[ai][provider][stream][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    config.timeout = std::chrono::milliseconds{0};
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    std::string invalid_text = "invalid UTF-8: ";
    invalid_text.push_back(static_cast<char>(0xFF));

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
        config.api_key = "sk-test-api-key";
        ai::providers::StreamingOpenAIChatClient client(transport, config);
        ai::StreamChatRequest request;
        request.model = "gpt-test";
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
        config.api_key = "sk-test-api-key";
        ai::providers::StreamingOpenAIChatClient client(transport, config);
        ai::StreamChatRequest request;
        request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);
    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    "streaming OpenAI client completes an accepted network failure as one error message",
    "[ai][provider][stream][issue11][issue14]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->failure = util::make_error(
        util::ErrorCode::Network,
        "provider connection failed",
        "could not resolve api.example: Name or service not known");

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    config.api = "openai-completions";
    config.provider = "openai-compatible";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
    request.context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});

    auto run = run_client(client, std::move(request));

    REQUIRE(run.result.has_value());
    CHECK(run.result->stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(run.result->error_message.has_value());
    CHECK(*run.result->error_message == "could not resolve api.example: Name or service not known");
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
    config.api_key = "sk-test-api-key";
    config.api = "openai-completions";
    config.provider = "openai-compatible";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    config.provider = "opencode-go";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
    config.api_key = "sk-test-api-key";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
}

TEST_CASE(
    "streaming OpenAI client completes malformed provider JSON before content as an error message",
    "[ai][provider][stream][issue12]") {
    auto transport = std::make_shared<FakeStreamTransport>();
    transport->chunks = {"data: {\"choices\":[}\n\n"};

    ai::providers::OpenAIStreamConfig config;
    config.api_key = "sk-test-api-key";
    config.api = "openai-completions";
    config.provider = "openai-compatible";
    ai::providers::StreamingOpenAIChatClient client(transport, config);

    ai::StreamChatRequest request;
    request.model = "gpt-test";
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
}
