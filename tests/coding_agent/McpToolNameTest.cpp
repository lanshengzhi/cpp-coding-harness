// Spec #882: Agent-visible MCP tool names follow pi `createMcpToolName`
// (`extensions/mcp/tools.ts`, `core/mcp-servers.ts`): the sanitized
// `mcp__<server>__<tool>` name stands when it fits pi's 64-char bound and no
// other tool owns it; otherwise the name is the first
// `MAX_TOOL_NAME_LENGTH - 9` characters plus `_<8-hex-sha256>` over the RAW
// `${server}\0${tool}` pair. The long-name and collision examples below are
// read from the frozen bundle (`mcp-tool-surface.json`), not restated.

#include "coding_agent/mcp/McpExtensionToolSource.hpp"

#include <cch/support/JsonValue.hpp>

#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] support::JsonValue bundle_tool_name() {
    const std::filesystem::path path =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-tool-surface.json";
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return support::read_json(text.str()).value().get_object().at("toolName");
}

} // namespace

TEST_CASE("tool names within pi's length bound keep the sanitized form", "[coding_agent][mcp][issue884][spec]") {
    CHECK(coding_agent::mcp::mcp_tool_name("filesystem", "read_file") == "mcp__filesystem__read_file");
    // pi's bundle example: punctuation outside [A-Za-z0-9_] becomes _.
    const auto examples = bundle_tool_name().get_object().at("examples").get_array();
    const auto& short_example = examples.front().get_object();
    CHECK(coding_agent::mcp::mcp_tool_name(short_example.at("server").get_string(),
                    short_example.at("tool").get_string()) == short_example.at("name").get_string());
}

TEST_CASE("a tool name over pi's 64-char bound takes the sha256 suffix from the bundle example",
        "[coding_agent][mcp][issue884][spec]") {
    const auto examples = bundle_tool_name().get_object().at("examples").get_array();
    const auto& long_example = examples.back().get_object();
    CHECK(long_example.at("name").get_string().size() == static_cast<std::size_t>(
                bundle_tool_name().get_object().at("maxLength").get_number()));
    CHECK(coding_agent::mcp::mcp_tool_name(long_example.at("server").get_string(),
                    long_example.at("tool").get_string()) == long_example.at("name").get_string());
}

TEST_CASE("a taken tool name takes the sha256 suffix from the bundle collision example",
        "[coding_agent][mcp][issue884][spec]") {
    const auto collision = bundle_tool_name().get_object().at("collision").get_object();
    // The bundle's second raw tool sanitizes to the first's plain name; with
    // the name taken, pi hashes the RAW `${server}\0${tool}` pair.
    CHECK(coding_agent::mcp::mcp_tool_name("srv", "a_b") == collision.at("first").get_string());
    CHECK(coding_agent::mcp::mcp_tool_name("srv", "a_b", [](const std::string&) { return true; }) ==
            collision.at("secondWhenTaken").get_string());
}

TEST_CASE("colliding tool names are assigned order-independently, every colliding tool hashed",
        "[coding_agent][mcp][issue884][spec]") {
    // pi `index.ts`: `plain` counts the sanitized names, and a candidate is
    // taken when its plain name is duplicated — so two tools that sanitize to
    // one name BOTH take the hash suffix, in either tools/list order.
    const auto forward = coding_agent::mcp::assign_mcp_tool_names("srv", {"a-b", "a_b"});
    REQUIRE(forward.size() == 2);
    CHECK(forward[0] != "mcp__srv__a_b");
    CHECK(forward[1] != "mcp__srv__a_b");
    CHECK(forward[0] != forward[1]);

    const auto backward = coding_agent::mcp::assign_mcp_tool_names("srv", {"a_b", "a-b"});
    CHECK(backward == forward);
}

TEST_CASE("distinct tool names under the bound are assigned unchanged", "[coding_agent][mcp][issue884][spec]") {
    const auto assigned = coding_agent::mcp::assign_mcp_tool_names("filesystem", {"read_file", "write_file"});
    CHECK(assigned == std::vector<std::string>{"mcp__filesystem__read_file", "mcp__filesystem__write_file"});
}
