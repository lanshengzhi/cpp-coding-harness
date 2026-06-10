#include "AgentLoop.hpp"

#include "../harness/LocalExecutionEnv.hpp"

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
    emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::AgentStart));

    Message user;
    user.role = Role::User;
    user.content = std::move(user_prompt);
    if (auto appended = append(result.messages, user); !appended) {
        return util::Result<LoopResult>::failure(appended.error());
    }
    emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::UserMessage, user.content));

    for (int turn = 0; turn < options_.max_turns; ++turn) {
        const int turn_number = turn + 1;
        emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::TurnStart, {}, turn_number));
        emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::ModelRequest, "turn " + std::to_string(turn_number), turn_number));
        emit(result.events, "model_request", "turn " + std::to_string(turn_number));

        AgentContext context = AgentContext::from_legacy(result.messages, registry_.definitions(), options_.model);
        llm::ChatRequest request = llm::chat_request_from_ai(context.chat_request());

        auto response = client_.complete(request);
        if (!response) {
            emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::ProviderError, response.error(), turn_number));
            emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::AgentEnd, response.error(), turn_number));
            emit(result.events, "provider_error", response.error());
            return util::Result<LoopResult>::failure(response.error());
        }

        auto ai_response = llm::to_ai_chat_response(response.value());
        Message assistant = message_from_ai(ai_response.assistant_message);
        assistant.role = Role::Assistant;
        if (auto appended = append(result.messages, assistant); !appended) {
            return util::Result<LoopResult>::failure(appended.error());
        }
        emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::AssistantMessage, assistant.content, turn_number));
        emit(result.events, "assistant", assistant.content);

        if (assistant.tool_calls.empty()) {
            result.final_text = assistant.content;
            auto compatible_response = llm::chat_response_from_ai(ai_response);
            result.stop_reason = compatible_response.stop_reason.empty() ? "stop" : compatible_response.stop_reason;
            emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::TurnEnd, result.stop_reason, turn_number));
            emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::AgentEnd, result.stop_reason, turn_number));
            emit(result.events, "completed", result.stop_reason);
            return util::Result<LoopResult>::success(result);
        }

        for (const auto& call : assistant.tool_calls) {
            emit_agent(result.agent_events, AgentEvent::tool_start(call.id, call.name, turn_number));
            emit(result.events, "tool_call", call.name + "#" + call.id);
            Message tool_result = execute_tool_call(call);
            if (auto appended = append(result.messages, tool_result); !appended) {
                return util::Result<LoopResult>::failure(appended.error());
            }
            emit_agent(result.agent_events, AgentEvent::tool_end(call.id, call.name, tool_result.is_error, turn_number));
            emit(result.events, tool_result.is_error ? "tool_error" : "tool_success", tool_result.tool_call_id);
        }
        emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::TurnEnd, "tool_use", turn_number));
    }

    result.stop_reason = "max_turns_exceeded";
    emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::MaxTurns, result.stop_reason, options_.max_turns));
    emit_agent(result.agent_events, AgentEvent::make(AgentEventKind::AgentEnd, result.stop_reason, options_.max_turns));
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

void AgentLoop::emit_agent(std::vector<AgentEvent>& events, AgentEvent event) const {
    events.push_back(event);
    if (options_.on_agent_event) {
        options_.on_agent_event(event);
    }
}

util::Result<void> AgentLoop::append(std::vector<Message>& messages, const Message& message) const {
    Message redacted = redact_message(message);
    messages.push_back(redacted);
    if (options_.on_message) {
        return options_.on_message(redacted);
    }
    return util::Result<void>::success();
}

Message AgentLoop::execute_tool_call(const ToolCall& call) {
    if (!call.arguments_valid) {
        AgentToolResult result;
        result.tool_call_id = call.id;
        result.tool_name = call.name;
        result.is_error = true;
        result.content = call.argument_error.empty() ? "Malformed tool arguments" : call.argument_error;
        return result.to_message();
    }

    Tool* tool = registry_.find(call.name);
    if (tool == nullptr) {
        AgentToolResult result;
        result.tool_call_id = call.id;
        result.tool_name = call.name;
        result.is_error = true;
        result.content = "Unknown tool: " + call.name;
        return result.to_message();
    }

    ToolContext context;
    context.workspace = options_.workspace;
    context.bash_enabled = options_.bash_enabled;
    context.secret_environment_names = options_.secret_environment_names;
    context.execution_env = std::make_shared<harness::LocalExecutionEnv>(
        context.workspace,
        context.bash_enabled,
        context.secret_environment_names);
    ToolExecutionResult execution = tool->execute(call.arguments, context);
    return to_agent_tool_result(call, execution).to_message();
}

} // namespace cch::agent
