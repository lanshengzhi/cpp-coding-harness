#include "OpenAIChatClient.hpp"

#include "../util/Redactor.hpp"

#include <boost/json.hpp>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace cch::llm {
namespace {

boost::json::object message_to_openai(const agent::Message& message) {
    boost::json::object obj;
    obj["role"] = agent::to_string(message.role);
    obj["content"] = util::redact_text(message.content);
    if (message.role == agent::Role::Tool) {
        obj["tool_call_id"] = message.tool_call_id;
    }
    if (message.role == agent::Role::Assistant && !message.tool_calls.empty()) {
        boost::json::array calls;
        for (const auto& call : message.tool_calls) {
            boost::json::object function;
            function["name"] = call.name;
            function["arguments"] = call.raw_arguments.empty() ? boost::json::serialize(call.arguments) : util::redact_text(call.raw_arguments);
            boost::json::object call_json;
            call_json["id"] = call.id;
            call_json["type"] = "function";
            call_json["function"] = std::move(function);
            calls.push_back(std::move(call_json));
        }
        obj["tool_calls"] = std::move(calls);
    }
    return obj;
}

boost::json::object tool_to_openai(const agent::ToolDefinition& definition) {
    boost::json::object function;
    function["name"] = definition.name;
    function["description"] = definition.description;
    function["parameters"] = definition.parameters;
    boost::json::object tool;
    tool["type"] = "function";
    tool["function"] = std::move(function);
    return tool;
}

std::string required_string(const boost::json::object& object, const char* key) {
    auto* value = object.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string{};
}

util::Result<ChatResponse> parse_chat_response(const std::string& body) {
    boost::system::error_code ec;
    auto parsed = boost::json::parse(body, ec);
    if (ec || !parsed.is_object()) {
        return util::Result<ChatResponse>::failure("invalid provider JSON response");
    }
    auto& root = parsed.as_object();
    auto* choices_value = root.if_contains("choices");
    if (choices_value == nullptr || !choices_value->is_array() || choices_value->as_array().empty()) {
        return util::Result<ChatResponse>::failure("provider response missing choices");
    }
    const auto& first_choice_value = choices_value->as_array().front();
    if (!first_choice_value.is_object()) {
        return util::Result<ChatResponse>::failure("provider choice is not an object");
    }
    const auto& choice = first_choice_value.as_object();
    ChatResponse response;
    response.stop_reason = required_string(choice, "finish_reason");
    auto* message_value = choice.if_contains("message");
    if (message_value == nullptr || !message_value->is_object()) {
        return util::Result<ChatResponse>::failure("provider choice missing message");
    }
    const auto& message = message_value->as_object();
    response.assistant_message.role = agent::Role::Assistant;
    if (auto* content = message.if_contains("content"); content && content->is_string()) {
        response.assistant_message.content = std::string(content->as_string());
    }
    if (auto* tool_calls = message.if_contains("tool_calls"); tool_calls && tool_calls->is_array()) {
        for (const auto& tool_call_value : tool_calls->as_array()) {
            if (!tool_call_value.is_object()) {
                continue;
            }
            const auto& tool_call_object = tool_call_value.as_object();
            agent::ToolCall call;
            call.id = required_string(tool_call_object, "id");
            auto* function_value = tool_call_object.if_contains("function");
            if (function_value == nullptr || !function_value->is_object()) {
                call.arguments_valid = false;
                call.argument_error = "tool call missing function object";
                response.assistant_message.tool_calls.push_back(std::move(call));
                continue;
            }
            const auto& function = function_value->as_object();
            call.name = required_string(function, "name");
            call.raw_arguments = required_string(function, "arguments");
            boost::system::error_code arg_ec;
            auto parsed_args = boost::json::parse(call.raw_arguments.empty() ? "{}" : call.raw_arguments, arg_ec);
            if (arg_ec || !parsed_args.is_object()) {
                call.arguments_valid = false;
                call.argument_error = "invalid JSON arguments for tool call " + call.id;
            } else {
                call.arguments = parsed_args.as_object();
            }
            response.assistant_message.tool_calls.push_back(std::move(call));
        }
    }
    return util::Result<ChatResponse>::success(std::move(response));
}

} // namespace

OpenAIChatClient::OpenAIChatClient(std::shared_ptr<HttpTransport> transport, OpenAIConfig config)
    : transport_(std::move(transport)), config_(std::move(config)) {}

util::Result<ChatResponse> OpenAIChatClient::complete(const ChatRequest& request) {
    auto api_key = resolve_api_key();
    if (!api_key) {
        return util::Result<ChatResponse>::failure(api_key.error());
    }

    boost::json::object body;
    body["model"] = request.model.empty() ? config_.model : request.model;
    boost::json::array messages;
    for (const auto& message : request.messages) {
        messages.push_back(message_to_openai(message));
    }
    body["messages"] = std::move(messages);
    if (!request.tools.empty()) {
        boost::json::array tools;
        for (const auto& tool : request.tools) {
            tools.push_back(tool_to_openai(tool));
        }
        body["tools"] = std::move(tools);
    }

    HttpRequest http;
    http.method = "POST";
    http.url = completions_url();
    http.timeout = config_.timeout;
    http.headers["Authorization"] = "Bearer " + api_key.value();
    http.headers["Content-Type"] = "application/json";
    if (!config_.organization.empty()) {
        http.headers["OpenAI-Organization"] = config_.organization;
    }
    if (!config_.project.empty()) {
        http.headers["OpenAI-Project"] = config_.project;
    }
    http.body = boost::json::serialize(body);

    auto response = transport_->send(http);
    if (!response) {
        return util::Result<ChatResponse>::failure(util::redact_text(response.error()));
    }
    if (response.value().status_code < 200 || response.value().status_code >= 300) {
        std::ostringstream error;
        error << "provider returned HTTP " << response.value().status_code << ": " << util::redact_text(response.value().body);
        return util::Result<ChatResponse>::failure(error.str());
    }
    return parse_chat_response(response.value().body);
}

util::Result<std::string> OpenAIChatClient::resolve_api_key() const {
    if (!config_.api_key.empty()) {
        return util::Result<std::string>::success(config_.api_key);
    }
    if (!config_.api_key_env.empty()) {
        if (const char* value = std::getenv(config_.api_key_env.c_str()); value != nullptr && *value != '\0') {
            return util::Result<std::string>::success(std::string(value));
        }
    }
    return util::Result<std::string>::failure("missing API key; set " + config_.api_key_env + " or pass provider configuration");
}

std::string OpenAIChatClient::completions_url() const {
    std::string base = config_.base_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.size() >= 3 && base.substr(base.size() - 3) == "/v1") {
        return base + "/chat/completions";
    }
    return base + "/v1/chat/completions";
}

} // namespace cch::llm
