#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/llm/OpenAIChatClient.hpp"
#include "../../src/util/JsonSchema.hpp"

#include <boost/json.hpp>

using namespace cch;

namespace {
class FakeHttpTransport final : public llm::HttpTransport {
public:
    util::Result<llm::HttpResponse> send(const llm::HttpRequest& request) override {
        requests.push_back(request);
        if (!error.empty()) {
            return util::Result<llm::HttpResponse>::failure(error);
        }
        return util::Result<llm::HttpResponse>::success(response);
    }

    llm::HttpResponse response{200, {}, R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"ok"}}]})"};
    std::string error;
    std::vector<llm::HttpRequest> requests;
};

boost::json::object parse_body(const FakeHttpTransport& transport) {
    REQUIRE_FALSE(transport.requests.empty());
    auto parsed = boost::json::parse(transport.requests.back().body);
    REQUIRE(parsed.is_object());
    return parsed.as_object();
}
} // namespace

TEST_CASE("tool definition maps to OpenAI function tool schema", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);

    boost::json::object properties;
    properties["path"] = util::string_property("file path");
    agent::ToolDefinition tool{"read_file", "Read a file", util::object_schema(std::move(properties), {"path"})};
    llm::ChatRequest request;
    request.model = "gpt-test";
    request.messages.push_back({agent::Role::User, "read file"});
    request.tools.push_back(tool);

    auto response = client.complete(request);

    REQUIRE(response.ok());
    auto body = parse_body(*transport);
    REQUIRE(body["tools"].is_array());
    const auto& first_tool = body["tools"].as_array().front().as_object();
    CHECK(std::string(first_tool.at("type").as_string()) == "function");
    const auto& function = first_tool.at("function").as_object();
    CHECK(std::string(function.at("name").as_string()) == "read_file");
    CHECK(function.at("parameters").as_object().at("required").as_array().front().as_string() == "path");
}

TEST_CASE("OpenAI fixture response maps text and tool calls to neutral message", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    transport->response.body = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"I'll read it","tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"README.md\"}"}}]}}]})";
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "read"});

    auto response = client.complete(request);

    REQUIRE(response.ok());
    CHECK(response.value().stop_reason == "tool_calls");
    CHECK(response.value().assistant_message.content == "I'll read it");
    REQUIRE(response.value().assistant_message.tool_calls.size() == 1);
    const auto& call = response.value().assistant_message.tool_calls[0];
    CHECK(call.id == "call_1");
    CHECK(call.name == "read_file");
    CHECK(call.arguments_valid);
    CHECK(std::string(call.arguments.at("path").as_string()) == "README.md");
}

TEST_CASE("invalid provider tool arguments become structured malformed tool call", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    transport->response.body = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_bad","type":"function","function":{"name":"read_file","arguments":"not-json"}}]}}]})";
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "read"});

    auto response = client.complete(request);

    REQUIRE(response.ok());
    REQUIRE(response.value().assistant_message.tool_calls.size() == 1);
    CHECK_FALSE(response.value().assistant_message.tool_calls[0].arguments_valid);
    CHECK(response.value().assistant_message.tool_calls[0].argument_error.find("invalid JSON") != std::string::npos);
}

TEST_CASE("provider tool calls missing linkage fields are rejected", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    transport->response.body = R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","tool_calls":[{"type":"function","function":{"name":"read_file","arguments":"{}"}}]}}]})";
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "read"});

    auto response = client.complete(request);

    REQUIRE_FALSE(response.ok());
    CHECK(response.error().find("missing id") != std::string::npos);
}

TEST_CASE("missing API key and provider errors are structured and redacted", "[llm][u2]") {
    auto missing_transport = std::make_shared<FakeHttpTransport>();
    llm::OpenAIConfig missing_config;
    missing_config.api_key_env = "CCH_TEST_MISSING_OPENAI_KEY";
    llm::OpenAIChatClient missing_client(missing_transport, missing_config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "hello"});

    auto missing = missing_client.complete(request);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().find("missing API key") != std::string::npos);

    auto error_transport = std::make_shared<FakeHttpTransport>();
    error_transport->response.status_code = 401;
    error_transport->response.body = "bad token sk-123456789SECRET";
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(error_transport, config);

    auto error = client.complete(request);

    REQUIRE_FALSE(error.ok());
    CHECK(error.error().find("HTTP 401") != std::string::npos);
    CHECK(error.error().find("sk-123456789SECRET") == std::string::npos);
}

TEST_CASE("assistant tool call arguments are redacted in provider requests", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    agent::Message assistant;
    assistant.role = agent::Role::Assistant;
    agent::ToolCall call;
    call.id = "call_1";
    call.name = "write_file";
    call.arguments = {{"path", "secret.txt"}, {"apiKey", "plain-secret-value"}, {"access_token", "second-secret-value"}};
    call.raw_arguments = R"({"path":"secret.txt","apiKey":"plain-secret-value","access_token":"second-secret-value"})";
    assistant.tool_calls.push_back(call);
    request.messages.push_back(assistant);

    auto response = client.complete(request);

    REQUIRE(response.ok());
    auto body = parse_body(*transport);
    const auto& messages = body.at("messages").as_array();
    const auto& function = messages[0].as_object().at("tool_calls").as_array()[0].as_object().at("function").as_object();
    const auto arguments = std::string(function.at("arguments").as_string());
    CHECK(arguments.find("plain-secret-value") == std::string::npos);
    CHECK(arguments.find("second-secret-value") == std::string::npos);
    CHECK(arguments.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("request body preserves message ordering and redacts secret-looking tool output", "[llm][u2]") {
    auto transport = std::make_shared<FakeHttpTransport>();
    llm::OpenAIConfig config;
    config.api_key = "sk-test-api-key";
    config.base_url = "https://gateway.example/v1";
    llm::OpenAIChatClient client(transport, config);
    llm::ChatRequest request;
    request.messages.push_back({agent::Role::User, "inspect"});
    agent::Message tool;
    tool.role = agent::Role::Tool;
    tool.tool_call_id = "call_1";
    tool.content = "token=sk-123456789SECRET";
    request.messages.push_back(tool);

    auto response = client.complete(request);

    REQUIRE(response.ok());
    REQUIRE(transport->requests.size() == 1);
    CHECK(transport->requests[0].url == "https://gateway.example/v1/chat/completions");
    auto body = parse_body(*transport);
    const auto& messages = body.at("messages").as_array();
    REQUIRE(messages.size() == 2);
    CHECK(std::string(messages[0].as_object().at("role").as_string()) == "user");
    CHECK(std::string(messages[1].as_object().at("role").as_string()) == "tool");
    CHECK(std::string(messages[1].as_object().at("content").as_string()).find("sk-123456789SECRET") == std::string::npos);
}
