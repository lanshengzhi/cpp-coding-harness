// Spec #882 / ticket #884: the write half of `mcp.json` persistence, matching
// pi v1.0.4 `extensions/mcp/config.ts` (`updateMcpServerConfig`,
// `addMcpServerConfig`, `removeMcpServerConfig`, `editMcpServers`). The frozen
// pi-v1.0.4 bundle declares the patch shape and the override keys
// (`mcp-config-surface.json` `declarations.serverConfigPatch` / `overrideKeys`);
// the file-shape behaviour is pinned here against pi's rules: default-key
// deletion, override entries keeping defaults, and an indentation-preserving
// single-file rewrite that leaves unrelated content alone.

#include "coding_agent/mcp/McpConfigWrite.hpp"

#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

using namespace cch;

namespace {

using JsonObject = support::JsonValue::object_t;

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue parse_file(const std::filesystem::path& path) {
    auto parsed = support::read_json(read_file(path));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

[[nodiscard]] const JsonObject& servers_of(const support::JsonValue& document) {
    const auto& root = document.get_object();
    const auto found = root.find("mcpServers");
    REQUIRE(found != root.end());
    const auto* servers = found->second.get_if<JsonObject>();
    REQUIRE(servers != nullptr);
    return *servers;
}

[[nodiscard]] const JsonObject& entry_of(const support::JsonValue& document, std::string_view name) {
    const auto& servers = servers_of(document);
    const auto found = servers.find(std::string{name});
    REQUIRE(found != servers.end());
    const auto* entry = found->second.get_if<JsonObject>();
    REQUIRE(entry != nullptr);
    return *entry;
}

[[nodiscard]] bool has_key(const JsonObject& object, std::string_view key) { return object.contains(std::string{key}); }

[[nodiscard]] coding_agent::mcp::McpServerConfigPatch patch_of(
        std::optional<bool> enabled, std::optional<coding_agent::mcp::McpExposure> exposure) {
    coding_agent::mcp::McpServerConfigPatch patch;
    patch.enabled = enabled;
    patch.exposure = exposure;
    return patch;
}

} // namespace

TEST_CASE("updating a plain entry writes a non-default value and deletes the default",
        "[coding_agent][mcp][issue884][spec]") {
    using coding_agent::mcp::McpExposure;
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    workspace.write("mcp.json", R"({"mcpServers": {"srv": {"command": "node"}}})");

    REQUIRE(coding_agent::mcp::update_mcp_server_config(path, "srv", patch_of(false, McpExposure::Hidden)).has_value());
    {
        const auto document = parse_file(path);
        const auto& entry = entry_of(document, "srv");
        REQUIRE(has_key(entry, "enabled"));
        CHECK_FALSE(entry.at("enabled").get_boolean());
        REQUIRE(has_key(entry, "exposure"));
        CHECK(entry.at("exposure").get_string() == "hidden");
        CHECK(entry.at("command").get_string() == "node");
    }

    // `enabled: true` and `exposure: "codemode"` are the defaults, so the keys
    // are removed rather than written back.
    REQUIRE(coding_agent::mcp::update_mcp_server_config(path, "srv", patch_of(true, McpExposure::Codemode))
                    .has_value());
    {
        const auto document = parse_file(path);
        const auto& entry = entry_of(document, "srv");
        CHECK_FALSE(has_key(entry, "enabled"));
        CHECK_FALSE(has_key(entry, "exposure"));
    }
}

TEST_CASE("updating an override entry keeps default values", "[coding_agent][mcp][issue884][spec]") {
    using coding_agent::mcp::McpExposure;
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    // No `command`/`url`/`type`: this entry overrides a global server, so a
    // default value must be written, not deleted, or the override would vanish.
    workspace.write("mcp.json", R"({"mcpServers": {"srv": {"enabled": false}}})");

    REQUIRE(coding_agent::mcp::update_mcp_server_config(path, "srv", patch_of(true, McpExposure::Codemode))
                    .has_value());
    const auto document = parse_file(path);
    const auto& entry = entry_of(document, "srv");
    REQUIRE(has_key(entry, "enabled"));
    CHECK(entry.at("enabled").get_boolean());
    REQUIRE(has_key(entry, "exposure"));
    CHECK(entry.at("exposure").get_string() == "codemode");
}

TEST_CASE("update with override adds a missing entry as an override", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    workspace.write("mcp.json", R"({"mcpServers": {}})");

    // Without `override` a missing server is an error and the file is untouched.
    auto refused = coding_agent::mcp::update_mcp_server_config(
            path, "srv", patch_of(false, std::nullopt), /* override */ false);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("does not define MCP server") != std::string::npos);
    CHECK(servers_of(parse_file(path)).empty());

    REQUIRE(coding_agent::mcp::update_mcp_server_config(path, "srv", patch_of(false, std::nullopt), /* override */ true)
                    .has_value());
    const auto document = parse_file(path);
    const auto& entry = entry_of(document, "srv");
    CHECK(has_key(entry, "enabled"));
    CHECK_FALSE(entry.at("enabled").get_boolean());
    CHECK_FALSE(has_key(entry, "command"));
}

TEST_CASE("an mcp.json rewrite preserves indentation and unrelated content", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    // Four-space indentation and an unrelated top-level key.
    workspace.write("mcp.json",
            "{\n"
            "    \"unrelated\": {\"keep\": true},\n"
            "    \"mcpServers\": {\n"
            "        \"srv\": {\"command\": \"node\"}\n"
            "    }\n"
            "}\n");

    REQUIRE(coding_agent::mcp::update_mcp_server_config(
            path, "srv", patch_of(std::nullopt, coding_agent::mcp::McpExposure::Direct))
                    .has_value());
    const std::string text = read_file(path);
    CHECK(text.find("\n    \"unrelated\"") != std::string::npos);
    CHECK(text.find("\n        \"srv\"") != std::string::npos);
    CHECK(text.find("\n            \"command\"") != std::string::npos);
    CHECK(text.ends_with("}\n"));
    // The unrelated key survived the rewrite.
    const auto document = parse_file(path);
    CHECK(document.get_object().at("unrelated").get_object().at("keep").get_boolean());
}

TEST_CASE("adding a server creates the file and reports replacement", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "nested" / "mcp.json";

    support::JsonValue config{JsonObject{{"command", "node"}, {"args", support::JsonValue::array_t{"server.mjs"}}}};
    auto added = coding_agent::mcp::add_mcp_server_config(path, "srv", config);
    REQUIRE(added.has_value());
    CHECK_FALSE(*added);
    REQUIRE(std::filesystem::exists(path));
    {
        const auto document = parse_file(path);
        CHECK(entry_of(document, "srv").at("command").get_string() == "node");
    }

    // Adding the same name again replaces it and reports so.
    auto replaced = coding_agent::mcp::add_mcp_server_config(
            path, "srv", support::JsonValue{JsonObject{{"url", "https://example.com/mcp"}}});
    REQUIRE(replaced.has_value());
    CHECK(*replaced);
    const auto document = parse_file(path);
    const auto& entry = entry_of(document, "srv");
    CHECK_FALSE(has_key(entry, "command"));
    CHECK(entry.at("url").get_string() == "https://example.com/mcp");
}

TEST_CASE("removing a server reports whether the file defined it", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";

    // A missing file is not an error and nothing was removed.
    auto absent = coding_agent::mcp::remove_mcp_server_config(path, "srv");
    REQUIRE(absent.has_value());
    CHECK_FALSE(*absent);

    workspace.write("mcp.json", R"({"mcpServers": {"srv": {"command": "node"}, "other": {"command": "node"}}})");
    auto unknown = coding_agent::mcp::remove_mcp_server_config(path, "missing");
    REQUIRE(unknown.has_value());
    CHECK_FALSE(*unknown);

    auto removed = coding_agent::mcp::remove_mcp_server_config(path, "srv");
    REQUIRE(removed.has_value());
    CHECK(*removed);
    const auto document = parse_file(path);
    const auto& servers = servers_of(document);
    CHECK_FALSE(servers.contains("srv"));
    CHECK(servers.contains("other"));
}

TEST_CASE("an mcp.json rewrite preserves key insertion order", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    // pi's `JSON.stringify(JSON.parse(text), null, indent)` keeps the file's
    // member order, so a read-modify-write must not reorder anything. A
    // `std::map`-backed rewrite would sort these to `alpha`/`zulu` and
    // `autoEnableCodemode`/`mcpServers`.
    workspace.write("mcp.json",
            "{\n"
            "  \"mcpServers\": {\n"
            "    \"zulu\": {\"url\": \"https://z.example/mcp\", \"exposure\": \"direct\"},\n"
            "    \"alpha\": {\"command\": \"node\", \"args\": [\"server.mjs\"]}\n"
            "  },\n"
            "  \"autoEnableCodemode\": true\n"
            "}\n");

    REQUIRE(coding_agent::mcp::update_mcp_server_config(
            path, "alpha", patch_of(std::nullopt, coding_agent::mcp::McpExposure::Hidden))
                    .has_value());
    const std::string text = read_file(path);
    CHECK(text.find("\"zulu\"") < text.find("\"alpha\""));
    CHECK(text.find("\"mcpServers\"") < text.find("\"autoEnableCodemode\""));
    // `alpha`'s own member order survives, and the patch's new key appends last.
    CHECK(text.find("\"command\"") < text.find("\"args\""));
    // `alpha`'s existing members keep their order and the patch's new key
    // appends last within the same entry.
    const std::string alpha_block = text.substr(text.find("\"alpha\""));
    CHECK(alpha_block.find("\"command\"") < alpha_block.find("\"args\""));
    CHECK(alpha_block.find("\"args\"") < alpha_block.find("\"exposure\""));
}

TEST_CASE("an mcp.json rewrite leaves no temporary file behind", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "mcp.json";
    workspace.write("mcp.json", R"({"mcpServers": {"srv": {"command": "node"}}})");

    REQUIRE(coding_agent::mcp::update_mcp_server_config(path, "srv", patch_of(false, std::nullopt)).has_value());
    // The write half replaces the file through a temp name + rename; the temp
    // name must not survive (a reader that opened it would see a torn file).
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}
