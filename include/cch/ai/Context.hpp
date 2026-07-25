#pragma once

#include "Message.hpp"
#include "Tool.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cch::ai {

/// Model-facing conversation state for one request. Model identity is not
/// carried here; it lives on StreamChatRequest::model (ADR 0019).
struct AiContext {
    std::optional<std::string> system_prompt;
    std::vector<MessageVariant> messages;
    std::vector<Tool> tools;
};

} // namespace cch::ai
