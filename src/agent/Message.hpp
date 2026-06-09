#pragma once

#include <boost/json.hpp>

#include "../util/Redactor.hpp"
#include "../util/Result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::agent {

enum class Role { System, User, Assistant, Tool };

inline std::string to_string(Role role) {
    switch (role) {
    case Role::System:
        return "system";
    case Role::User:
        return "user";
    case Role::Assistant:
        return "assistant";
    case Role::Tool:
        return "tool";
    }
    return "user";
}

inline std::optional<Role> role_from_string(const std::string& role) {
    if (role == "system") {
        return Role::System;
    }
    if (role == "user") {
        return Role::User;
    }
    if (role == "assistant") {
        return Role::Assistant;
    }
    if (role == "tool") {
        return Role::Tool;
    }
    return std::nullopt;
}

struct ToolCall {
    std::string id;
    std::string name;
    boost::json::object arguments;
    std::string raw_arguments;
    bool arguments_valid{true};
    std::string argument_error;
};

struct Message {
    Role role{Role::User};
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
    bool is_error{false};
};

inline ToolCall redact_tool_call(ToolCall call) {
    call.arguments = util::redact_json_object(call.arguments);
    call.raw_arguments = call.raw_arguments.empty() ? std::string{} : util::redact_json_text(call.raw_arguments);
    call.argument_error = util::redact_text(call.argument_error);
    return call;
}

inline Message redact_message(Message message) {
    message.content = util::redact_text(message.content);
    for (auto& call : message.tool_calls) {
        call = redact_tool_call(std::move(call));
    }
    return message;
}

inline boost::json::object tool_call_to_json(const ToolCall& call) {
    boost::json::object obj;
    obj["id"] = call.id;
    obj["name"] = call.name;
    obj["arguments"] = call.arguments;
    obj["raw_arguments"] = call.raw_arguments;
    obj["arguments_valid"] = call.arguments_valid;
    obj["argument_error"] = call.argument_error;
    return obj;
}

inline boost::json::object message_to_json(const Message& message) {
    boost::json::object obj;
    obj["role"] = to_string(message.role);
    obj["content"] = message.content;
    if (!message.tool_call_id.empty()) {
        obj["tool_call_id"] = message.tool_call_id;
    }
    obj["is_error"] = message.is_error;
    boost::json::array calls;
    for (const auto& call : message.tool_calls) {
        calls.push_back(tool_call_to_json(call));
    }
    obj["tool_calls"] = calls;
    return obj;
}

inline ToolCall tool_call_from_json(const boost::json::object& obj) {
    ToolCall call;
    if (auto* v = obj.if_contains("id"); v && v->is_string()) {
        call.id = std::string(v->as_string());
    }
    if (auto* v = obj.if_contains("name"); v && v->is_string()) {
        call.name = std::string(v->as_string());
    }
    if (auto* v = obj.if_contains("arguments"); v && v->is_object()) {
        call.arguments = v->as_object();
    }
    if (auto* v = obj.if_contains("raw_arguments"); v && v->is_string()) {
        call.raw_arguments = std::string(v->as_string());
    }
    if (auto* v = obj.if_contains("arguments_valid"); v && v->is_bool()) {
        call.arguments_valid = v->as_bool();
    }
    if (auto* v = obj.if_contains("argument_error"); v && v->is_string()) {
        call.argument_error = std::string(v->as_string());
    }
    return call;
}

inline util::Result<Message> message_from_json(const boost::json::object& obj) {
    auto* role_value = obj.if_contains("role");
    if (role_value == nullptr || !role_value->is_string()) {
        return util::Result<Message>::failure("message role is missing");
    }
    auto role = role_from_string(std::string(role_value->as_string()));
    if (!role) {
        return util::Result<Message>::failure("unknown message role");
    }
    Message message;
    message.role = *role;
    if (auto* v = obj.if_contains("content"); v && v->is_string()) {
        message.content = std::string(v->as_string());
    }
    if (auto* v = obj.if_contains("tool_call_id"); v && v->is_string()) {
        message.tool_call_id = std::string(v->as_string());
    }
    if (auto* v = obj.if_contains("is_error"); v && v->is_bool()) {
        message.is_error = v->as_bool();
    }
    if (auto* v = obj.if_contains("tool_calls"); v && v->is_array()) {
        for (const auto& item : v->as_array()) {
            if (item.is_object()) {
                message.tool_calls.push_back(tool_call_from_json(item.as_object()));
            }
        }
    }
    return util::Result<Message>::success(message);
}

} // namespace cch::agent
