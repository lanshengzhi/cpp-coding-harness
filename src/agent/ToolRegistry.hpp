#pragma once

#include "Tool.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cch::agent {

class ToolRegistry {
public:
    void add(ToolPtr tool) { tools_[tool->definition().name] = std::move(tool); }

    [[nodiscard]] std::vector<ToolDefinition> definitions() const {
        std::vector<ToolDefinition> defs;
        for (const auto& [_, tool] : tools_) {
            defs.push_back(tool->definition());
        }
        return defs;
    }

    [[nodiscard]] Tool* find(const std::string& name) {
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    [[nodiscard]] const Tool* find(const std::string& name) const {
        auto it = tools_.find(name);
        if (it == tools_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

private:
    std::map<std::string, ToolPtr> tools_;
};

} // namespace cch::agent
