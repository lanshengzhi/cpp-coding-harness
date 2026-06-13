#pragma once

#include "Message.hpp"
#include "Tool.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::ai {

struct AiContext {
    std::optional<std::string> system_prompt;
    std::string model;
    std::vector<MessageVariant> messages;
    std::vector<Tool> tools;
};

} // namespace cch::ai
