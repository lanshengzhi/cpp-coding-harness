#pragma once

#include "AgentTool.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cch::agent {

class AsyncToolRegistry {
public:
    AsyncToolRegistry() = default;
    AsyncToolRegistry(AsyncToolRegistry&&) noexcept = default;
    AsyncToolRegistry& operator=(AsyncToolRegistry&&) noexcept = default;
    AsyncToolRegistry(const AsyncToolRegistry&) = delete;
    AsyncToolRegistry& operator=(const AsyncToolRegistry&) = delete;

    void add(std::unique_ptr<AsyncAgentTool> tool) {
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
        std::sort(result.begin(), result.end(), [](const ai::Tool& left, const ai::Tool& right) {
            return left.name < right.name;
        });
        return result;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<AsyncAgentTool>> tools_;
};

} // namespace cch::agent
