#pragma once

#include "Message.hpp"
#include "Tool.hpp"

#include <boost/json.hpp>

#include <string>

namespace cch::agent {

struct AgentToolCall {
    std::string id;
    std::string name;
    boost::json::object arguments;
    std::string raw_arguments;
    bool arguments_valid{true};
    std::string argument_error;
};

struct AgentToolResult {
    std::string tool_call_id;
    std::string tool_name;
    std::string content;
    bool is_error{false};

    [[nodiscard]] Message to_message() const {
        Message message;
        message.role = Role::Tool;
        message.tool_call_id = tool_call_id;
        message.tool_name = tool_name;
        message.content = content;
        message.is_error = is_error;
        return message;
    }
};

inline AgentToolCall to_agent_tool_call(const ToolCall& call) {
    return {call.id, call.name, call.arguments, call.raw_arguments, call.arguments_valid, call.argument_error};
}

inline ToolCall tool_call_from_agent(const AgentToolCall& call) {
    return {call.id, call.name, call.arguments, call.raw_arguments, call.arguments_valid, call.argument_error};
}

inline AgentToolResult to_agent_tool_result(const ToolCall& call, const ToolExecutionResult& result) {
    return {call.id, call.name, result.content, result.is_error};
}

} // namespace cch::agent
