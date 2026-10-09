#pragma once

#include <cch/ai/Tool.hpp>
#include <cch/support/JsonValue.hpp>

#include "coding_agent/mcp/McpExposure.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

struct ToolSearchCandidate {
    std::string name;
    std::string description;
    std::string namespace_name;
    mcp::McpExposure exposure{mcp::McpExposure::Codemode};
    bool active{false};
};

[[nodiscard]] std::vector<ToolSearchCandidate> rank_tool_search_candidates(
        std::vector<ToolSearchCandidate> candidates, std::string_view query, std::size_t limit);

[[nodiscard]] ai::Tool tool_search_definition();

} // namespace cch::coding_agent::extensions
