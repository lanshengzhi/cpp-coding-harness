#pragma once

#include <boost/json.hpp>

#include "../util/Result.hpp"
#include "../ai/Tool.hpp"
#include "Message.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cch::agent {

struct ToolDefinition {
    std::string name;
    std::string description;
    boost::json::object parameters;
};

struct ToolContext {
    std::filesystem::path workspace;
    bool bash_enabled{false};
    std::vector<std::string> secret_environment_names;
};

struct ToolExecutionResult {
    std::string content;
    bool is_error{false};
};

class Tool {
public:
    virtual ~Tool() = default;
    [[nodiscard]] virtual ToolDefinition definition() const = 0;
    [[nodiscard]] virtual ToolExecutionResult execute(const boost::json::object& arguments, const ToolContext& context) = 0;
};

using ToolPtr = std::shared_ptr<Tool>;

inline ai::ToolDefinition to_ai_tool_definition(const ToolDefinition& definition) {
    ai::ToolDefinition converted;
    converted.name = definition.name;
    converted.description = definition.description;
    converted.parameters = definition.parameters;
    return converted;
}

inline ToolDefinition tool_definition_from_ai(const ai::ToolDefinition& definition) {
    ToolDefinition converted;
    converted.name = definition.name;
    converted.description = definition.description;
    converted.parameters = definition.parameters;
    return converted;
}

} // namespace cch::agent
