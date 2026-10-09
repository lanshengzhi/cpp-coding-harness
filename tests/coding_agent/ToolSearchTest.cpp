#include "coding_agent/extensions/tool_search/ToolSearch.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cch;

TEST_CASE("tool search filters inactive deferred and codemode tools", "[coding_agent][tool_search]") {
    std::vector<coding_agent::extensions::ToolSearchCandidate> candidates{
            {.name = "mcp__docs__search",
                    .description = "Search documentation",
                    .namespace_name = "docs",
                    .exposure = coding_agent::mcp::McpExposure::Deferred},
            {.name = "mcp__git__status",
                    .description = "Show status",
                    .namespace_name = "git",
                    .exposure = coding_agent::mcp::McpExposure::Codemode},
            {.name = "mcp__hidden__secret",
                    .description = "Search secret",
                    .namespace_name = "hidden",
                    .exposure = coding_agent::mcp::McpExposure::Hidden},
            {.name = "mcp__direct__search",
                    .description = "Search direct",
                    .namespace_name = "direct",
                    .exposure = coding_agent::mcp::McpExposure::Direct},
            {.name = "mcp__docs__active",
                    .description = "Search already active",
                    .namespace_name = "docs",
                    .exposure = coding_agent::mcp::McpExposure::Deferred,
                    .active = true},
    };
    const auto matches = coding_agent::extensions::rank_tool_search_candidates(std::move(candidates), "search", 10);
    REQUIRE(matches.size() == 1);
    CHECK(matches.front().name == "mcp__docs__search");
}

TEST_CASE("tool search ranks namespace and description matches", "[coding_agent][tool_search]") {
    std::vector<coding_agent::extensions::ToolSearchCandidate> candidates{
            {.name = "mcp__misc__lookup",
                    .description = "Fetch information",
                    .namespace_name = "misc",
                    .exposure = coding_agent::mcp::McpExposure::Deferred},
            {.name = "mcp__calendar__find_events",
                    .description = "Find calendar events",
                    .namespace_name = "calendar",
                    .exposure = coding_agent::mcp::McpExposure::Codemode},
    };
    const auto matches =
            coding_agent::extensions::rank_tool_search_candidates(std::move(candidates), "calendar events", 2);
    REQUIRE(matches.size() == 1);
    CHECK(matches.front().name == "mcp__calendar__find_events");
    const auto definition = coding_agent::extensions::tool_search_definition();
    CHECK(definition.name == "tool_search");
    const auto& schema = definition.parameters.get<support::JsonValue::object_t>();
    CHECK(schema.at("required").get_array().front().get_string() == "query");
    CHECK(schema.at("properties").get<support::JsonValue::object_t>().contains("limit"));
}
