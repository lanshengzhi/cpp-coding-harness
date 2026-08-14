#pragma once

#include <cch/agent/AgentTool.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::agent {

/// Prompt-visible tool metadata (pi `ToolDefinition.promptSnippet` /
/// `promptGuidelines`, normalized exactly like pi's `_normalizePromptSnippet`
/// / `_normalizePromptGuidelines` in `agent-session.ts`). The System Prompt
/// builder consumes this per active tool.
struct ToolPromptMetadata {
    std::string name{};
    /// One-line snippet; absent when the tool has none (or only whitespace).
    std::optional<std::string> snippet{};
    /// Trimmed, deduped guideline bullets in declaration order.
    std::vector<std::string> guidelines{};
};

/// Move-only registry of passive Agent Tools keyed by tool name.
class ToolRegistry {
public:
    ToolRegistry() = default;
    ToolRegistry(ToolRegistry&&) noexcept = default;
    ToolRegistry& operator=(ToolRegistry&&) noexcept = default;
    ToolRegistry(const ToolRegistry&) = delete;
    ToolRegistry& operator=(const ToolRegistry&) = delete;

    [[nodiscard]] support::ExpectedVoid add(Tool tool) {
        if (!tool.execute) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot add a tool without an execute operation"));
        }
        if (tool.definition.name.empty()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "cannot add a tool without a name"));
        }
        tools_[tool.definition.name] = std::move(tool);
        return {};
    }

    [[nodiscard]] Tool* find(const std::string& name) {
        auto it = tools_.find(name);
        return it == tools_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const Tool* find(const std::string& name) const {
        auto it = tools_.find(name);
        return it == tools_.end() ? nullptr : &it->second;
    }

    /// Prompt metadata for one registered tool, normalized like pi
    /// `agent-session.ts` `_normalizePromptSnippet`/`_normalizePromptGuidelines`
    /// (the `_toolPromptSnippets`/`_toolPromptGuidelines` sources for
    /// `_rebuildSystemPrompt`). `std::nullopt` for an unknown tool name.
    [[nodiscard]] std::optional<ToolPromptMetadata> prompt_metadata(
        const std::string& name) const {
        const auto* tool = find(name);
        if (tool == nullptr) {
            return std::nullopt;
        }
        ToolPromptMetadata metadata;
        metadata.name = name;
        if (tool->prompt_snippet) {
            // pi `_normalizePromptSnippet`: line runs and whitespace runs
            // become one space, then trim.
            std::string one_line;
            one_line.reserve(tool->prompt_snippet->size());
            bool pending_space = false;
            for (const char ch : *tool->prompt_snippet) {
                if (std::isspace(static_cast<unsigned char>(ch))) {
                    pending_space = true;
                } else {
                    if (pending_space && !one_line.empty()) {
                        one_line += ' ';
                    }
                    pending_space = false;
                    one_line += ch;
                }
            }
            if (!one_line.empty()) {
                metadata.snippet = std::move(one_line);
            }
        }
        // pi `_normalizePromptGuidelines`: trim each bullet, drop empties,
        // dedupe preserving first-occurrence order.
        for (const auto& guideline : tool->prompt_guidelines) {
            auto begin = guideline.begin();
            while (begin != guideline.end() &&
                   std::isspace(static_cast<unsigned char>(*begin))) {
                ++begin;
            }
            auto end = guideline.end();
            while (end != begin &&
                   std::isspace(static_cast<unsigned char>(*(end - 1)))) {
                --end;
            }
            if (begin == end) {
                continue;
            }
            const std::string normalized{begin, end};
            const bool duplicate =
                std::find(
                    metadata.guidelines.begin(),
                    metadata.guidelines.end(),
                    normalized) != metadata.guidelines.end();
            if (!duplicate) {
                metadata.guidelines.push_back(normalized);
            }
        }
        return metadata;
    }

    [[nodiscard]] std::vector<ai::Tool> definitions() const {
        std::vector<ai::Tool> result;
        result.reserve(tools_.size());
        for (const auto& [_, tool] : tools_) {
            result.push_back(tool.definition);
        }
        std::sort(result.begin(), result.end(), [](const ai::Tool& left, const ai::Tool& right) {
            return left.name < right.name;
        });
        return result;
    }

private:
    std::unordered_map<std::string, Tool> tools_;
};

} // namespace cch::agent
