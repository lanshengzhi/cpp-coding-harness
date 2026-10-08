// Spec #882, ticket #884: the MCP protocol handshake diffs against the frozen
// pi-v1.0.4 evidence bundle (`fixtures/pi-ai/v1.0.4/mcp-codemode/`,
// `mcp-protocol-surface.json`). pi's client sends LATEST_PROTOCOL_VERSION and
// rejects a negotiated version outside SUPPORTED_PROTOCOL_VERSIONS with pi's
// verbatim message; Pike's initialize params and validation are checked
// field by field against that bundle, not against self-captured values.

#include "coding_agent/mcp/McpProtocol.hpp"

#include "support/Json.hpp"

#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace cch;

namespace {

[[nodiscard]] support::JsonValue bundle_protocol() {
    const std::filesystem::path path =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-protocol-surface.json";
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return support::read_json(text.str()).value();
}

[[nodiscard]] support::JsonValue valid_initialize_result(std::string_view protocol_version) {
    return support::JsonValue{support::JsonValue::object_t{
            {"protocolVersion", std::string{protocol_version}},
            {"capabilities", support::JsonValue::object_t{}},
            {"serverInfo",
                    support::JsonValue::object_t{
                            {"name", "fixture"},
                            {"version", "1.0"},
                    }},
    }};
}

} // namespace

TEST_CASE("initialize requests the bundle's LATEST protocol version and advertises roots",
        "[coding_agent][mcp][issue884][spec]") {
    const auto latest = bundle_protocol().get_object().at("protocol").get_object().at("latest").get_string();
    const auto params = coding_agent::mcp::detail::initialize_params();
    CHECK(params.get_object().at("protocolVersion").get_string() == latest);
    const auto* capabilities = params.get_object().at("capabilities").get_if<support::JsonValue::object_t>();
    REQUIRE(capabilities != nullptr);
    CHECK(capabilities->contains("roots"));
}

TEST_CASE("initialize validation accepts every version the bundle supports", "[coding_agent][mcp][issue884][spec]") {
    const auto supported = bundle_protocol().get_object().at("protocol").get_object().at("supported").get_array();
    REQUIRE(!supported.empty());
    for (const auto& version : supported) {
        const auto valid = coding_agent::mcp::detail::validate_initialize_result(
                "fixture", valid_initialize_result(version.get_string()));
        CHECK(valid.has_value());
    }
}

TEST_CASE("initialize validation rejects an unsupported negotiated version with pi's message",
        "[coding_agent][mcp][issue884][spec]") {
    const auto valid =
            coding_agent::mcp::detail::validate_initialize_result("fixture", valid_initialize_result("1999-01-01"));
    REQUIRE(!valid.has_value());
    CHECK(valid.error().message == "MCP server selected unsupported protocol version 1999-01-01");
}

TEST_CASE("initialize validation requires pi's serverInfo identity shape", "[coding_agent][mcp][issue884][spec]") {
    // pi `validateInitializeResult`: serverInfo.name and serverInfo.version
    // must be strings, and a present instructions must be a string.
    auto missing_name = valid_initialize_result("2025-11-25");
    missing_name.get_object().at("serverInfo").get_object().erase("name");
    CHECK_FALSE(coding_agent::mcp::detail::validate_initialize_result("fixture", missing_name).has_value());

    auto numeric_version = valid_initialize_result("2025-11-25");
    numeric_version.get_object().at("serverInfo").get_object().at("version") = support::JsonValue{1.0};
    CHECK_FALSE(coding_agent::mcp::detail::validate_initialize_result("fixture", numeric_version).has_value());

    auto numeric_instructions = valid_initialize_result("2025-11-25");
    numeric_instructions.get_object().emplace("instructions", support::JsonValue{1.0});
    CHECK_FALSE(coding_agent::mcp::detail::validate_initialize_result("fixture", numeric_instructions).has_value());
}
