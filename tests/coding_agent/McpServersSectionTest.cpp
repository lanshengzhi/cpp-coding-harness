// Spec #882 / ticket #884: the `mcp_servers` system prompt section, diffed
// against pi's `renderServersSection` (`packages/coding-agent/src/extensions/
// mcp/index.ts` at 7c10bd43, v1.0.4). The section is what tells the model which
// servers exist when neither codemode nor tool_search names them, so its
// filters (indirect tools only), ordering, per-reach intro clauses, description
// truncation, and the hard section cap are the acceptance surface. The
// separation cases name what a shape-only check would let through: a renderer
// that lists a `direct` server (its tools ARE declared), one that keeps the
// codemode clause when only `deferred` servers remain, and one that drops
// servers past the cap without the counting line.

#include "coding_agent/mcp/McpServersSection.hpp"
#include "coding_agent/prompt/SystemPromptBuilder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

using coding_agent::mcp::McpConfigEntry;
using coding_agent::mcp::McpExposure;
using coding_agent::mcp::McpServerConfigBase;
using coding_agent::mcp::McpServerListing;
using coding_agent::mcp::McpStdioServerConfig;

[[nodiscard]] McpConfigEntry make_entry(std::string name,
        std::optional<McpExposure> exposure = std::nullopt,
        std::optional<std::string> description = std::nullopt,
        std::vector<std::pair<std::string, McpExposure>> tool_exposure = {}) {
    McpStdioServerConfig config;
    config.name = name;
    config.command = "true";
    config.exposure = exposure;
    config.description = std::move(description);
    config.tool_exposure = std::move(tool_exposure);
    McpConfigEntry entry;
    entry.name = name;
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('\n', begin);
        lines.emplace_back(text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return lines;
}

} // namespace

TEST_CASE("mcp_servers section lists only indirect-exposure enabled servers", "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry codemode = make_entry("alpha", McpExposure::Codemode);
    const McpConfigEntry deferred = make_entry("beta", McpExposure::Deferred);
    const McpConfigEntry direct = make_entry("gamma", McpExposure::Direct);
    const McpConfigEntry hidden = make_entry("delta", McpExposure::Hidden);
    McpConfigEntry disabled = make_entry("epsilon", McpExposure::Codemode);
    disabled.enabled = false;
    const std::vector<McpServerListing> listings{
            McpServerListing{&direct, std::nullopt},
            McpServerListing{&deferred, std::nullopt},
            McpServerListing{&codemode, std::nullopt},
            McpServerListing{&hidden, std::nullopt},
            McpServerListing{&disabled, std::nullopt},
    };

    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    const auto lines = split_lines(*section);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] ==
            "MCP servers whose tools are not declared to you. Call the tools of `codemode` servers from codemode "
            "scripts. Load the tools of `tool_search` servers with `tool_search`.");
    // Sorted by name; `direct`/`hidden`/disabled servers never appear.
    CHECK(lines[1] == "- mcp__alpha (codemode)");
    CHECK(lines[2] == "- mcp__beta (tool_search)");
    CHECK(section->find("gamma") == std::string::npos);
}

TEST_CASE("mcp_servers section drops the codemode clause when only deferred servers remain",
        "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry deferred = make_entry("beta", McpExposure::Deferred);
    const std::vector<McpServerListing> listings{McpServerListing{&deferred, std::nullopt}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    CHECK(*section == "MCP servers whose tools are not declared to you. Load the tools of `tool_search` servers with "
                      "`tool_search`.\n- mcp__beta (tool_search)");
}

TEST_CASE("mcp_servers section counts a server as codemode when any toolExposure says so",
        "[coding_agent][mcp][issue884][spec]") {
    // The server's own exposure is `deferred`, but one tool is `codemode`, so the
    // configured exposures include both and the codemode clause is emitted.
    const McpConfigEntry mixed =
            make_entry("mixed", McpExposure::Deferred, std::nullopt, {{"dangerous", McpExposure::Codemode}});
    const auto exposures = coding_agent::mcp::mcp_configured_exposures(mixed);
    CHECK(std::find(exposures.begin(), exposures.end(), McpExposure::Codemode) != exposures.end());
    CHECK(std::find(exposures.begin(), exposures.end(), McpExposure::Deferred) != exposures.end());

    const std::vector<McpServerListing> listings{McpServerListing{&mixed, std::nullopt}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    CHECK(section->find("Call the tools of `codemode` servers") != std::string::npos);
    CHECK(section->find("- mcp__mixed (codemode)") != std::string::npos);
}

TEST_CASE("mcp_servers section namespaces hyphens and summarizes the first description line",
        "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry entry = make_entry("my-server", McpExposure::Codemode, "  Reads files\nsecond line ignored");
    const std::vector<McpServerListing> listings{McpServerListing{&entry, std::nullopt}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    CHECK(*section ==
            "MCP servers whose tools are not declared to you. Call the tools of `codemode` servers from codemode "
            "scripts.\n- mcp__my_server (codemode): Reads files");
}

TEST_CASE("mcp_servers section falls back to the connection instructions", "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry entry = make_entry("plain", McpExposure::Codemode);
    const std::vector<McpServerListing> listings{McpServerListing{&entry, std::string{"Server instructions\nmore"}}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    CHECK(section->find("- mcp__plain (codemode): Server instructions") != std::string::npos);
}

TEST_CASE("mcp_servers section caps a description at 250 characters with an ellipsis",
        "[coding_agent][mcp][issue884][spec]") {
    const std::string long_description(1000, 'x');
    const McpConfigEntry entry = make_entry("big", McpExposure::Codemode, long_description);
    const std::vector<McpServerListing> listings{McpServerListing{&entry, std::nullopt}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    const auto marker = section->find("- mcp__big (codemode): ");
    REQUIRE(marker != std::string::npos);
    const std::string summary = section->substr(marker + std::string{"- mcp__big (codemode): "}.size());
    // 249 content characters plus the three-byte ellipsis.
    CHECK(summary == std::string(249, 'x') + "\xE2\x80\xA6");
}

TEST_CASE("mcp_servers section stays under the cap and counts the omitted servers",
        "[coding_agent][mcp][issue884][spec]") {
    std::vector<McpConfigEntry> entries;
    std::vector<McpServerListing> listings;
    for (int index = 0; index < 60; ++index) {
        entries.push_back(make_entry(
                std::string("server_") + std::string(60, 'a') + std::to_string(index), McpExposure::Codemode));
    }
    for (const auto& entry : entries) {
        listings.push_back(McpServerListing{&entry, std::nullopt});
    }
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());
    CHECK(section->size() <= coding_agent::mcp::kMcpMaxServersSectionChars);

    const auto lines = split_lines(*section);
    const std::string closing = lines.back();
    const std::string suffix = " more servers; find their tools with searchTools()";
    REQUIRE(closing.starts_with("- \xE2\x80\xA6 "));
    REQUIRE(closing.ends_with(suffix));
    const std::string count_text = closing.substr(std::string{"- \xE2\x80\xA6 "}.size(),
            closing.size() - std::string{"- \xE2\x80\xA6 "}.size() - suffix.size());
    const std::size_t omitted = static_cast<std::size_t>(std::stoul(count_text));
    const auto present = static_cast<std::size_t>(std::count_if(
            lines.begin(), lines.end(), [](const std::string& line) { return line.starts_with("- mcp__"); }));
    CHECK(present + omitted == listings.size());
    CHECK(omitted > 0);
}

TEST_CASE("mcp_servers section is absent when no server has indirect tools", "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry direct = make_entry("gamma", McpExposure::Direct);
    const std::vector<McpServerListing> listings{McpServerListing{&direct, std::nullopt}};
    CHECK_FALSE(coding_agent::mcp::render_mcp_servers_section(listings).has_value());
    CHECK_FALSE(coding_agent::mcp::render_mcp_servers_section({}).has_value());
}

TEST_CASE("the prompt builder renders the mcp_servers section after cwd", "[coding_agent][mcp][issue884][spec]") {
    const McpConfigEntry entry = make_entry("alpha", McpExposure::Codemode, "does alpha things");
    const std::vector<McpServerListing> listings{McpServerListing{&entry, std::nullopt}};
    const auto section = coding_agent::mcp::render_mcp_servers_section(listings);
    REQUIRE(section.has_value());

    coding_agent::prompt::BuildSystemPromptOptions options;
    options.cwd = "/workspace";
    options.mcpServersSection = *section;
    const auto sections = coding_agent::prompt::buildSystemPromptSections(options);
    REQUIRE(sections.size() >= 2);
    CHECK(sections.back().name == "mcp_servers");
    CHECK(sections.back().text ==
            "<mcp_servers>\nMCP servers whose tools are not declared to you. Call the tools of `codemode` servers "
            "from codemode scripts.\n- mcp__alpha (codemode): does alpha things\n</mcp_servers>");
    CHECK(sections[sections.size() - 2].name == "cwd");
}

TEST_CASE("the prompt builder omits an empty mcp_servers section", "[coding_agent][mcp][issue884][spec]") {
    coding_agent::prompt::BuildSystemPromptOptions options;
    options.cwd = "/workspace";
    options.mcpServersSection = std::string{};
    const auto sections = coding_agent::prompt::buildSystemPromptSections(options);
    CHECK(std::none_of(sections.begin(), sections.end(), [](const coding_agent::prompt::SystemPromptSection& section) {
        return section.name == "mcp_servers";
    }));
}
