#include "AgentLoop.hpp"

#include "../util/Redactor.hpp"

#include <sstream>
#include <utility>

namespace cch::agent {

AgentLoop::AgentLoop(llm::ChatClient& client, ToolRegistry registry, LoopOptions options)
    : client_(client), registry_(std::move(registry)), options_(std::move(options)) {}

util::Result<LoopResult> AgentLoop::run(std::string user_prompt) {
    return continue_with({}, std::move(user_prompt));
}

util::Result<LoopResult> AgentLoop::continue_with(std::vector<Message> existing_history, std::string user_prompt) {
    LoopResult result;
    result.messages = std::move(existing_history);

    Message user;
    user.role = Role::User;
    user.content = std::move(user_prompt);
    if (auto appended = append(result.messages, user); !appended) {
        return util::Result<LoopResult>::failure(appended.error());
    }

    for (int turn = 0; turn < options_.max_turns; ++turn) {
        emit(result.events, "model_request", "turn " + std::to_string(turn + 1));
        llm::ChatRequest request;
        request.messages = result.messages;
        request.tools = registry_.definitions();
        request.model = options_.model;

        auto response = client_.complete(request);
        if (!response) {
            emit(result.events, "provider_error", response.error());
            return util::Result<LoopResult>::failure(response.error());
        }

        Message assistant = response.value().assistant_message;
        assistant.role = Role::Assistant;
        if (auto appended = append(result.messages, assistant); !appended) {
            return util::Result<LoopResult>::failure(appended.error());
        }
        emit(result.events, "assistant", assistant.content);

        if (assistant.tool_calls.empty()) {
            result.final_text = assistant.content;
            result.stop_reason = response.value().stop_reason.empty() ? "stop" : response.value().stop_reason;
            emit(result.events, "completed", result.stop_reason);
            return util::Result<LoopResult>::success(result);
        }

        for (const auto& call : assistant.tool_calls) {
            emit(result.events, "tool_call", call.name + "#" + call.id);
            Message tool_result = execute_tool_call(call);
            if (auto appended = append(result.messages, tool_result); !appended) {
                return util::Result<LoopResult>::failure(appended.error());
            }
            emit(result.events, tool_result.is_error ? "tool_error" : "tool_success", tool_result.tool_call_id);
        }
    }

    result.stop_reason = "max_turns_exceeded";
    emit(result.events, "max_turns", result.stop_reason);
    return util::Result<LoopResult>::failure(result.stop_reason);
}

void AgentLoop::emit(std::vector<LoopEvent>& events, std::string type, std::string detail) const {
    LoopEvent event{std::move(type), std::move(detail)};
    events.push_back(event);
    if (options_.on_event) {
        options_.on_event(event);
    }
}

util::Result<void> AgentLoop::append(std::vector<Message>& messages, const Message& message) const {
    Message redacted = message;
    redacted.content = util::redact_text(redacted.content);
    for (auto& call : redacted.tool_calls) {
        call.raw_arguments = util::redact_text(call.raw_arguments);
    }
    messages.push_back(redacted);
    if (options_.on_message) {
        return options_.on_message(redacted);
    }
    return util::Result<void>::success();
}

Message AgentLoop::execute_tool_call(const ToolCall& call) const {
    Message result;
    result.role = Role::Tool;
    result.tool_call_id = call.id;

    if (!call.arguments_valid) {
        result.is_error = true;
        result.content = call.argument_error.empty() ? "Malformed tool arguments" : call.argument_error;
        return result;
    }

    const Tool* tool = registry_.find(call.name);
    if (tool == nullptr) {
        result.is_error = true;
        result.content = "Unknown tool: " + call.name;
        return result;
    }

    ToolContext context;
    context.workspace = options_.workspace;
    context.bash_enabled = options_.bash_enabled;
    ToolExecutionResult execution = const_cast<Tool*>(tool)->execute(call.arguments, context);
    result.content = execution.content;
    result.is_error = execution.is_error;
    return result;
}

} // namespace cch::agent
