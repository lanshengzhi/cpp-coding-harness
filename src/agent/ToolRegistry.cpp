#include <cch/agent/ToolRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::agent {

namespace {

/// pi `_normalizePromptSnippet`: line runs and whitespace runs become one
/// space, then trim.
[[nodiscard]] std::string normalize_prompt_snippet(std::string_view snippet) {
    std::string one_line;
    one_line.reserve(snippet.size());
    bool pending_space = false;
    for (const char ch : snippet) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !one_line.empty()) {
            one_line += ' ';
        }
        pending_space = false;
        one_line += ch;
    }
    return one_line;
}

} // namespace

std::optional<ToolPromptMetadata> ToolRegistry::prompt_metadata(const std::string& name) const {
    const auto* tool = find(name);
    if (tool == nullptr) {
        return std::nullopt;
    }
    ToolPromptMetadata metadata;
    metadata.name = name;
    if (tool->prompt_snippet) {
        auto one_line = normalize_prompt_snippet(*tool->prompt_snippet);
        if (!one_line.empty()) {
            metadata.snippet = std::move(one_line);
        }
    }
    // pi `_normalizePromptGuidelines`: trim each bullet, drop empties,
    // dedupe preserving first-occurrence order.
    for (const auto& guideline : tool->prompt_guidelines) {
        auto begin = guideline.begin();
        while (begin != guideline.end() && std::isspace(static_cast<unsigned char>(*begin))) {
            ++begin;
        }
        auto end = guideline.end();
        while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
            --end;
        }
        if (begin == end) {
            continue;
        }
        const std::string normalized{begin, end};
        const bool duplicate = std::find(metadata.guidelines.begin(), metadata.guidelines.end(), normalized) !=
                               metadata.guidelines.end();
        if (!duplicate) {
            metadata.guidelines.push_back(normalized);
        }
    }
    return metadata;
}

} // namespace cch::agent
