// Ticket #1118 (spec #1044, ADR 0075): the application codemode catalog. The
// frozen `pi-v1.3.0` artifact (`fixtures/pi-v1.3.0/audit-1053/application-catalog.json`,
// captured from pi `71023aec`) is the acceptance authority for the description
// and loadout shapes.
//
// Acceptance discipline: the exact differential scenarios pin the rendering, and
// the separation cases name what an existence-only check would let through — an
// empty catalog that matches the frozen empty text while a callable tool exists,
// a catalog that lists a deferred or model-only tool, and a `models` line that
// advertises a namespace the sandbox never binds.

#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"

#include "support/Json.hpp"

#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::filesystem::path artifact_path() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1053" / "application-catalog.json";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue read_artifact() {
    auto parsed = support::read_json(read_text(artifact_path()));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const support::JsonValue& scenario(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& candidate : artifact.get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == name) return candidate;
    }
    REQUIRE(false);
    return artifact;
}

[[nodiscard]] std::string expected_text(const support::JsonValue& artifact, std::string_view name) {
    return scenario(artifact, name).get_object().at("expected").get_string();
}

[[nodiscard]] support::JsonValue json_object(std::string_view text) {
    auto parsed = support::read_json(text);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// The artifact's `applicationTool(name, description, outputSchema?, parameters?)`
/// helper: `{ type: "object", properties: { query: { type: "string" } } }` unless
/// a parameters object is given.
[[nodiscard]] extensions::LoadoutTool application_tool(std::string name,
        std::string description,
        std::optional<std::string_view> output_schema = std::nullopt,
        std::optional<std::string_view> parameters = std::nullopt) {
    extensions::LoadoutTool tool;
    tool.definition.name = std::move(name);
    tool.definition.description = std::move(description);
    tool.definition.parameters =
            json_object(parameters.value_or(R"({"type":"object","properties":{"query":{"type":"string"}}})"));
    if (output_schema.has_value()) {
        tool.definition.output_schema = json_object(*output_schema);
    }
    return tool;
}

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("the empty codemode catalog matches the frozen pi-v1.3.0 description",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    const auto artifact = read_artifact();
    const auto expected = expected_text(
            artifact, "the empty codemode catalog still declares globals, discovery and the options grammar");

    const auto description = extensions::create_codemode_description({}, extensions::CodemodeCatalogSettings{});
    INFO(description);
    CHECK(description == expected);

    // The empty text is exactly what the tool definition starts from, so a
    // session that never refreshes the catalog still advertises only the
    // globals the sandbox binds.
    CHECK(extensions::codemode_tool_definition().description == expected);
    CHECK_FALSE(contains(description, "Nested tools:"));
    CHECK_FALSE(contains(description, "`models`"));
}

TEST_CASE("a listed tool section carries its declaration, output type and guidelines",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    const auto artifact = read_artifact();
    const auto expected =
            expected_text(artifact, "a listed tool section carries its declaration, output type and guidelines");

    auto tool = application_tool("mcp__docs__search",
            "Search docs",
            R"({"type":"object","properties":{"results":{"type":"array","items":{"type":"string"}}},"required":["results"]})");
    tool.exposure = extensions::ToolExposure::Codemode;
    tool.guidelines = {"Search before reading."};

    const auto description = extensions::create_codemode_description({tool}, extensions::CodemodeCatalogSettings{});
    INFO(description);
    CHECK(description == expected);
    CHECK(contains(description, "### `mcp__docs__search`"));
    CHECK(contains(description, "Promise<{ results: Array<string>; }>;"));
    CHECK(contains(description, "Search docs\n\n- Search before reading."));
}

TEST_CASE("namespaces group sections and the inline budget omits tools with an explicit marker",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    const auto artifact = read_artifact();
    const auto expected = expected_text(
            artifact, "namespaces group sections and the inline budget omits tools with an explicit marker");

    auto read = application_tool("read", "Read a file");
    auto search = application_tool("mcp__docs__search", "Search docs");
    search.exposure = extensions::ToolExposure::Codemode;
    search.name_space = extensions::ToolNamespace{
            .name = "docs",
            .aliases = {},
            .description = "Documentation tools",
            .instructions = {},
    };

    auto settings = extensions::CodemodeCatalogSettings{};
    settings.inline_budget = 40;
    const auto description = extensions::create_codemode_description({read, search}, settings);
    INFO(description);
    CHECK(description == expected);
    // The discriminating half: the omitted tool is the one that did not fit,
    // not every namespaced tool.
    CHECK(contains(description, "### `read`"));
    CHECK(contains(description, "## docs (tools not listed)\nDocumentation tools"));
    CHECK_FALSE(contains(description, "### `mcp__docs__search`"));
}

TEST_CASE("deferred tools are never listed and do not change the description",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    auto alpha = application_tool("alpha", "Alpha");
    auto beta = application_tool("beta", "Beta");
    beta.exposure = extensions::ToolExposure::Deferred;

    const auto with_deferred =
            extensions::create_codemode_description({alpha, beta}, extensions::CodemodeCatalogSettings{});
    const auto alpha_only = extensions::create_codemode_description({alpha}, extensions::CodemodeCatalogSettings{});
    CHECK_FALSE(contains(with_deferred, "### `beta`"));
    CHECK(with_deferred == alpha_only);

    auto only_deferred = application_tool("alpha", "Alpha");
    only_deferred.exposure = extensions::ToolExposure::Deferred;
    CHECK(extensions::create_codemode_description({only_deferred}, extensions::CodemodeCatalogSettings{}) ==
            extensions::create_codemode_description({}, extensions::CodemodeCatalogSettings{}));
}

TEST_CASE("the models declaration line follows the bound capability",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    auto without = extensions::CodemodeCatalogSettings{};
    auto with = extensions::CodemodeCatalogSettings{};
    with.bound_globals.push_back("models");

    const auto absent = extensions::create_codemode_description({}, without);
    const auto present = extensions::create_codemode_description({}, with);
    CHECK_FALSE(contains(absent, "`models`"));
    CHECK(contains(present, "`models`"));
    CHECK(contains(present, "classifiers and image generation"));
    // The gate is the bound-globals set, not a formatting flag: production
    // binds the discovery globals only, so the line cannot appear.
    CHECK(extensions::codemode_discovery_globals() ==
            std::vector<std::string>{"searchTools", "describeTool", "describeNamespace"});
    CHECK_FALSE(extensions::CodemodeCatalogSettings{}.models());
}

TEST_CASE("the callable tool set excludes codemode itself", "[coding_agent][codemode][catalog][issue1118][spec]") {
    auto read = application_tool("read", "Read");
    auto self = application_tool("codemode", "Self");
    auto search = application_tool("mcp__docs__search", "Search");

    std::vector<std::string> names;
    for (const auto& tool : extensions::callable_tools({read, self, search})) {
        names.push_back(tool.definition.name);
    }
    CHECK(names == std::vector<std::string>{"read", "mcp__docs__search"});
    // A catalog built from a tool set that still contains codemode must not
    // list it either.
    CHECK_FALSE(contains(extensions::create_codemode_description({read, self}, extensions::CodemodeCatalogSettings{}),
            "### `codemode`"));
}

TEST_CASE("the on loadout lists only non-direct callable tools and adds script-call descriptions",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    const auto artifact = read_artifact();
    const auto expected =
            scenario(artifact, "the on loadout lists only non-direct callable tools and adds script-call descriptions")
                    .get_object()
                    .at("expected");

    auto read = application_tool("read", "Read a file");
    read.exposure = extensions::ToolExposure::Direct;
    auto search = application_tool("mcp__docs__search",
            "Search docs",
            R"({"type":"object","properties":{"results":{"type":"array","items":{"type":"string"}}},"required":["results"]})");
    search.exposure = extensions::ToolExposure::Codemode;
    search.name_space = extensions::ToolNamespace{
            .name = "docs",
            .aliases = {},
            .description = "Documentation tools",
            .instructions = {},
    };
    search.guidelines = {"Use docs."};

    extensions::CodemodeLoadout loadout;
    loadout.declared = {read.definition, search.definition};
    loadout.callable = {read, search};
    auto settings = extensions::CodemodeCatalogSettings{};
    settings.mode = "on";
    const auto changes = extensions::prepare_codemode_loadout(loadout, settings);

    CHECK(changes.hidden_declarations.empty());
    CHECK(changes.descriptions.at("read") == expected.get_object().at("readDescription").get_string());
    CHECK(changes.descriptions.at("mcp__docs__search") == expected.get_object().at("searchDescription").get_string());
    INFO(changes.descriptions.at("codemode"));
    CHECK(changes.descriptions.at("codemode") == expected.get_object().at("codemodeDescription").get_string());
    CHECK_FALSE(contains(changes.descriptions.at("codemode"), "### `read`"));
}

TEST_CASE("the only loadout lists every callable tool and hides direct declarations",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    const auto artifact = read_artifact();
    const auto expected = scenario(artifact, "the only loadout lists every callable tool and hides direct declarations")
                                  .get_object()
                                  .at("expected");

    auto read = application_tool("read", "Read a file");
    read.exposure = extensions::ToolExposure::Direct;
    auto search = application_tool("mcp__docs__search", "Search docs");
    search.exposure = extensions::ToolExposure::Codemode;
    search.name_space = extensions::ToolNamespace{.name = "docs", .aliases = {}, .description = {}, .instructions = {}};

    extensions::CodemodeLoadout loadout;
    loadout.declared = {read.definition, search.definition};
    loadout.callable = {read, search};
    auto settings = extensions::CodemodeCatalogSettings{};
    settings.mode = "only";
    const auto changes = extensions::prepare_codemode_loadout(loadout, settings);

    CHECK(changes.hidden_declarations == std::vector<std::string>{"read"});
    std::vector<std::string> described;
    for (const auto& [name, _] : changes.descriptions)
        described.push_back(name);
    CHECK(described == std::vector<std::string>{"codemode"});
    INFO(changes.descriptions.at("codemode"));
    CHECK(changes.descriptions.at("codemode") == expected.get_object().at("codemodeDescription").get_string());
    CHECK(contains(changes.descriptions.at("codemode"), "### `read`"));
    CHECK(contains(changes.descriptions.at("codemode"), "### `mcp__docs__search`"));
}

TEST_CASE("a callable tool the catalog cannot execute is not silently declared",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    // pi's callable set is exposure-driven: `model-only` and `hidden` tools are
    // never listed, because a script cannot call them. A catalog that listed
    // them would advertise a callable surface the sandbox rejects.
    auto tool_search = application_tool("tool_search", "Search for tools");
    tool_search.exposure = extensions::ToolExposure::ModelOnly;
    auto hidden = application_tool("mcp__gone__tool", "Withdrawn");
    hidden.exposure = extensions::ToolExposure::Hidden;
    auto deferred = application_tool("mcp__docs__search", "Deferred");
    deferred.exposure = extensions::ToolExposure::Deferred;
    auto codemode = application_tool("codemode", "Self");
    codemode.exposure = extensions::ToolExposure::ModelOnly;

    const auto description = extensions::create_codemode_description(
            {tool_search, hidden, deferred, codemode}, extensions::CodemodeCatalogSettings{});
    CHECK_FALSE(contains(description, "### `tool_search`"));
    CHECK_FALSE(contains(description, "### `mcp__gone__tool`"));
    CHECK_FALSE(contains(description, "### `mcp__docs__search`"));
    CHECK_FALSE(contains(description, "### `codemode`"));
    // Nothing callable remains, so the description is the frozen empty text —
    // the same output a broken empty catalog would produce. The session test
    // below is what separates "empty because nothing is callable" from "empty
    // because the catalog was never refreshed".
    CHECK(description == extensions::create_codemode_description({}, extensions::CodemodeCatalogSettings{}));
}

TEST_CASE("the inline budget defaults to pi's 3000 estimated tokens in a loadout",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    // pi `prepareCodemodeLoadout` always passes `getInlineBudget() ??
    // DEFAULT_CODEMODE_INLINE_BUDGET`, while `createCodemodeDescription` alone
    // leaves the budget unset. A very long catalog therefore differs between
    // the two entry points only when the default is applied.
    std::vector<extensions::LoadoutTool> tools;
    for (int index = 0; index < 40; ++index) {
        auto tool = application_tool("tool_" + std::to_string(index), std::string(400, 'x'));
        tool.exposure = extensions::ToolExposure::Codemode;
        tools.push_back(std::move(tool));
    }
    const auto unlimited = extensions::create_codemode_description(tools, extensions::CodemodeCatalogSettings{});
    extensions::CodemodeLoadout loadout;
    loadout.callable = tools;
    const auto budgeted = extensions::prepare_codemode_loadout(loadout, extensions::CodemodeCatalogSettings{})
                                  .descriptions.at("codemode");
    CHECK(unlimited.size() > budgeted.size());
    CHECK(contains(budgeted, "Nested tools:"));
    CHECK_FALSE(contains(budgeted, "### `tool_39`"));
}
