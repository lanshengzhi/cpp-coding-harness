// #1101 MJson: an actual `tools/call` through the public cch_mcp client and
// the application conversion must preserve arbitrary JSON `structuredContent`.
// The fixture is a real stdio child; no scripted connection or schema is a
// stand-in for the value observed on the wire. Expected values come from the
// frozen pi v1.3.0 audit scenarios recorded in
// `fixtures/pi-v1.3.0/audit-1101/structured-content.json`.

#include "coding_agent/mcp/McpExtensionToolSource.hpp"

#include "support/JsonCompare.hpp"
#include "support/RuntimeFixture.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

namespace app_mcp = cch::coding_agent::mcp;
namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

[[nodiscard]] app_mcp::McpStdioServerConfig structured_server_config() {
    app_mcp::McpStdioServerConfig config;
    config.name = "structured";
    config.command = "python3";
    config.args = {fixture_path("structured_content_server.py")};
    return config;
}

[[nodiscard]] support::JsonValue call_arguments(std::string kind) {
    return support::JsonValue{support::JsonValue::object_t{{"kind", std::move(kind)}}};
}

[[nodiscard]] std::vector<extensions::ExtensionTool> load_structured_tools(tests::RuntimeFixture& runtime) {
    auto source =
            tests::run_awaitable(runtime, app_mcp::McpExtensionToolSource::connect_stdio(structured_server_config()));
    REQUIRE(source.has_value());
    REQUIRE(source.value() != nullptr);
    auto tools = source.value()->load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    CHECK(tools->front().definition.name == "mcp__structured__structured");
    return std::move(*tools);
}

[[nodiscard]] std::expected<extensions::ExtensionToolResult, support::Error> execute_structured(
        tests::RuntimeFixture& runtime, extensions::ExtensionTool& tool, std::string kind) {
    return runtime.run(tool.context_execute(
            call_arguments(std::move(kind)), extensions::ExtensionToolContext{}, std::stop_token{}));
}

void check_script_result(
        const support::JsonValue& expected_structured_content, const support::JsonValue& actual_details) {
    const support::JsonValue expected_details{support::JsonValue::object_t{
            {"content", support::JsonValue::array_t{}},
            {"structuredContent", expected_structured_content},
    }};
    const auto mismatch = tests::json_mismatch(expected_details, actual_details);
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
}

} // namespace

TEST_CASE("an MCP tools/call object structuredContent survives conversion with content absent",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);

    auto executed = execute_structured(runtime, tools.front(), "object");
    REQUIRE(executed.has_value());

    REQUIRE(executed->content.size() == 1);
    const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "{\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ],\n  \"ok\": true\n}");
    CHECK_FALSE(executed->is_error);

    REQUIRE(executed->details.has_value());
    check_script_result(
            support::JsonValue::object_t{
                    {"nested", support::JsonValue::array_t{true, nullptr, "x"}},
                    {"ok", true},
            },
            *executed->details);
}

TEST_CASE("an MCP tools/call preserves every primitive and array structuredContent when content is absent",
        "[coding_agent][mcp][issue1101][spec]") {
    struct StructuredCase {
        std::string kind;
        std::string expected_text;
        support::JsonValue expected_value;
    };
    const std::vector<StructuredCase> cases{
            {"array",
                    "[\n  1,\n  {\n    \"x\": true\n  },\n  null\n]",
                    support::JsonValue::array_t{1, support::JsonValue::object_t{{"x", true}}, nullptr}},
            {"empty-array", "[]", support::JsonValue::array_t{}},
            {"string", "\"text\"", "text"},
            {"empty-string", "\"\"", ""},
            {"false", "false", false},
            {"zero", "0", 0},
            {"null", "null", nullptr},
    };

    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    for (const auto& structured_case : cases) {
        INFO("structured kind: " << structured_case.kind);
        auto executed = execute_structured(runtime, tools.front(), structured_case.kind);
        REQUIRE(executed.has_value());

        REQUIRE(executed->content.size() == 1);
        const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
        REQUIRE(text != nullptr);
        CHECK(text->text == structured_case.expected_text);
        CHECK_FALSE(executed->is_error);
        REQUIRE(executed->details.has_value());
        check_script_result(structured_case.expected_value, *executed->details);
    }
}

TEST_CASE("an MCP tools/call distinguishes missing structuredContent from JSON null",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);

    auto missing = execute_structured(runtime, tools.front(), "missing");
    REQUIRE(missing.has_value());
    CHECK(missing->content.empty());
    CHECK_FALSE(missing->is_error);
    REQUIRE(missing->details.has_value());
    const auto* missing_details = missing->details->get_if<support::JsonValue::object_t>();
    REQUIRE(missing_details != nullptr);
    CHECK(missing_details->contains("content"));
    CHECK(missing_details->at("content").get_array().empty());
    CHECK_FALSE(missing_details->contains("structuredContent"));

    auto explicit_null = execute_structured(runtime, tools.front(), "null");
    REQUIRE(explicit_null.has_value());
    REQUIRE(explicit_null->content.size() == 1);
    const auto* null_text = std::get_if<ai::TextContent>(&explicit_null->content.front());
    REQUIRE(null_text != nullptr);
    CHECK(null_text->text == "null");
    const auto* null_details = explicit_null->details->get_if<support::JsonValue::object_t>();
    REQUIRE(null_details != nullptr);
    const auto structured = null_details->find("structuredContent");
    REQUIRE(structured != null_details->end());
    CHECK(structured->second.holds<support::JsonValue::null_t>());
}

TEST_CASE("an MCP tools/call rejects wrong content and malformed results at the public caller",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    for (const char* kind : {"wrong-content", "null-content", "malformed"}) {
        INFO("malformed kind: " << kind);
        auto executed = execute_structured(runtime, tools.front(), kind);
        REQUIRE_FALSE(executed.has_value());
        CHECK(executed.error().code == support::ErrorCode::Process);
        CHECK(executed.error().message.find("invalid tools/call result") != std::string::npos);
    }
}
