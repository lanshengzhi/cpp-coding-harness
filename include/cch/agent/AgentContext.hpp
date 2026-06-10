#pragma once

#include <cch/ai/Context.hpp>

#include <string>

namespace cch::agent {

struct AsyncAgentOptions {
    int max_turns{8};
    std::string model;
};

struct AsyncAgentRunResult {
    ai::AiContext context;
    ai::AssistantStopReason stop_reason{ai::AssistantStopReason::Unknown};
    int turns{0};
};

} // namespace cch::agent
