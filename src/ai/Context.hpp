#pragma once

#include "Message.hpp"
#include "Tool.hpp"

#include <string>
#include <vector>

namespace cch::ai {

struct Context {
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::string model;
};

} // namespace cch::ai
