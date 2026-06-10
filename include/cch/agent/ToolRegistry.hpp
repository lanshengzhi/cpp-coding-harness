#pragma once

#include "AgentTool.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cch::agent {

class AsyncToolRegistry {
public:
    void add(std::shared_ptr<AsyncAgentTool> tool) {
        if (!tool) {
            return;
        }
        tools_[tool->definition().name] = std::move(tool);
    }

    [[nodiscard]] AsyncAgentTool* find(const std::string& name) const {
        auto it = tools_.find(name);
        return it == tools_.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] std::vector<ai::Tool> definitions() const {
        std::vector<ai::Tool> result;
        result.reserve(tools_.size());
        for (const auto& [_, tool] : tools_) {
            result.push_back(tool->definition());
        }
        return result;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<AsyncAgentTool>> tools_;
};

} // namespace cch::agent
