#pragma once

#include "AgentTool.hpp"
#include "../ai/Context.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::agent {

struct AsyncAgentOptions {
    int max_turns{8};
    std::string model;
    std::optional<BeforeToolCallHook> before_tool_call;
    std::optional<AfterToolCallHook> after_tool_call;

    AsyncAgentOptions() = default;
    AsyncAgentOptions(AsyncAgentOptions&&) = default;
    AsyncAgentOptions& operator=(AsyncAgentOptions&&) = default;
    AsyncAgentOptions(const AsyncAgentOptions&) = delete;
    AsyncAgentOptions& operator=(const AsyncAgentOptions&) = delete;

    AsyncAgentOptions(int max_turns_, std::string model_)
        : max_turns(max_turns_), model(std::move(model_)) {}
};

struct AgentState {
    std::vector<ai::MessageVariant> messages;
    std::optional<ai::AssistantMessage> streaming_message;
    std::vector<std::string> active_tool_names;
    std::vector<std::string> pending_tool_call_ids;
    std::string model;
    std::string thinking_level;
};

struct AsyncAgentRunResult {
    ai::AiContext context;
    ai::AssistantStopReason stop_reason{ai::AssistantStopReason::Unknown};
    int turns{0};
    AgentState state;
};

} // namespace cch::agent
