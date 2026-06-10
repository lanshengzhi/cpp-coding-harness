#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../src/ai/providers/OpenAIChatClient.hpp"
#include "../../../src/llm/OpenAIChatClient.hpp"
#include "../../../src/util/JsonSchema.hpp"

#include <boost/json.hpp>

using namespace cch;

namespace {
class FakeHttpTransport final : public ai::providers::HttpTransport {
public:
    util::Result<ai::providers::HttpResponse> send(const ai::providers::HttpRequest& request) override {
        requests.push_back(request);
        if (!error.empty()) {
            return util::Result<ai::providers::HttpResponse>::failure(error);
        }
        return util::Result<ai::providers::HttpResponse>::success(response);
    }

    ai::providers::HttpResponse response{200, {}, R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})"};
    std::string error;
    std::vector<ai::providers::HttpRequest> requests;
};

boost::json::object parse_body(const FakeHttpTransport& transport) {
    REQUIRE_FALSE(transport.requests.empty());
    auto parsed = boost::json::parse(transport.requests.back().body);
    REQUIRE(parsed.is_object());
    return parsed.as_object();
}
} // namespace

TEST_CASE("AI OpenAI provider serializes context to current Chat Completions payload", "[ai][provider][u3]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    ai::providers::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    config.base_url = "https://gateway.example/v1";
    ai::providers::OpenAIChatClient client(transport, config);

    ai::ChatRequest request;
    request.context.model = "gpt-test";
    request.context.messages.push_back(ai::Message::user_text("inspect"));
    ai::Message tool = ai::Message::tool_result("call_1", "read_file", "token=sk-123456789SECRET", false);
    request.context.messages.push_back(tool);
    boost::json::object properties;
    properties["path"] = util::string_property("file path");
    request.context.tools.push_back({"read_file", "Read a file", util::object_schema(std::move(properties), {"path"})});

    auto response = client.complete(request);

    REQUIRE(response.ok());
    CHECK(transport->requests[0].url == "https://gateway.example/v1/chat/completions");
    auto body = parse_body(*transport);
    CHECK(std::string(body.at("model").as_string()) == "gpt-test");
    const auto& messages = body.at("messages").as_array();
    REQUIRE(messages.size() == 2);
    CHECK(std::string(messages[0].as_object().at("role").as_string()) == "user");
    CHECK(std::string(messages[1].as_object().at("role").as_string()) == "tool");
    CHECK(std::string(messages[1].as_object().at("tool_call_id").as_string()) == "call_1");
    CHECK(std::string(messages[1].as_object().at("content").as_string()).find("sk-123456789SECRET") == std::string::npos);
    REQUIRE(body.at("tools").is_array());
    CHECK(std::string(body.at("tools").as_array().front().as_object().at("function").as_object().at("name").as_string()) == "read_file");
}

TEST_CASE("AI OpenAI provider parses tool calls to content blocks", "[ai][provider][u3][ae2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    transport->response.body = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"I'll read it","tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"README.md\"}"}}]}}]})";
    ai::providers::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    ai::providers::OpenAIChatClient client(transport, config);
    ai::ChatRequest request;
    request.context.messages.push_back(ai::Message::user_text("read"));

    auto response = client.complete(request);

    REQUIRE(response.ok());
    CHECK(response.value().stop_reason == ai::StopReason::ToolUse);
    CHECK(response.value().assistant_message.role == ai::MessageRole::Assistant);
    REQUIRE(response.value().assistant_message.content.size() == 2);
    CHECK(response.value().assistant_message.content[0].text == "I'll read it");
    const auto& call = response.value().assistant_message.content[1].tool_call;
    CHECK(call.id == "call_1");
    CHECK(call.name == "read_file");
    CHECK(call.arguments_valid);
    CHECK(std::string(call.arguments.at("path").as_string()) == "README.md");
}

TEST_CASE("legacy llm OpenAI adapter preserves moved provider behavior", "[ai][provider][u3]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    transport->response.body = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"not-json"}}]}}]})";
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "read"});

    auto response = client.complete(request);

    REQUIRE(response.ok());
    CHECK(response.value().stop_reason == "tool_calls");
    REQUIRE(response.value().assistant_message.tool_calls.size() == 1);
    CHECK_FALSE(response.value().assistant_message.tool_calls[0].arguments_valid);
    CHECK(response.value().assistant_message.tool_calls[0].argument_error.find("invalid JSON") != std::string::npos);
}
