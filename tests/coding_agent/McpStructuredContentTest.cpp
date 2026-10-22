// #1101 MJson: an actual `tools/call` through the public cch_mcp client and
// the application conversion must preserve arbitrary JSON `structuredContent`.
// The fixture is a real stdio child; no scripted connection or schema is a
// stand-in for the value observed on the wire. Positive expected values come
// from the frozen pi v1.3.0 audit scenarios in
// `fixtures/pi-v1.3.0/audit-1101/structured-content.json` and the review-order
// artifact `fixtures/pi-v1.3.0/audit-1101/structured-content-order.json`;
// the rejection assertions are C++-side controls for the explicit
// `ErrorCode::Process` / `-32600` mapping, not frozen client observations.

#include "coding_agent/mcp/McpExtensionToolSource.hpp"

#include "support/JsonCompare.hpp"
#include "support/RuntimeFixture.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <fstream>
#include <iterator>
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
    CHECK(text->text == "{\n  \"ok\": true,\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ]\n}");
    CHECK_FALSE(executed->is_error);

    REQUIRE(executed->details.has_value());
    check_script_result(
            support::JsonValue::object_t{
                    {"nested", support::JsonValue::array_t{true, nullptr, "x"}},
                    {"ok", true},
            },
            *executed->details);
}

TEST_CASE(
        "the frozen review artifact matches every actual fixture observation", "[coding_agent][mcp][issue1101][spec]") {
    const std::string artifact_path =
            std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1101/structured-content-order.json";
    std::ifstream input(artifact_path, std::ios::binary);
    REQUIRE(input);
    const std::string artifact_text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto artifact = support::read_json(artifact_text);
    REQUIRE(artifact.has_value());
    const auto* root = artifact->get_if<support::JsonValue::object_t>();
    REQUIRE(root != nullptr);
    const auto scenarios = root->find("scenarios");
    REQUIRE(scenarios != root->end());
    const auto* scenario_list = scenarios->second.get_if<support::JsonValue::array_t>();
    REQUIRE(scenario_list != nullptr);

    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    for (const auto& scenario : *scenario_list) {
        const auto* scenario_object = scenario.get_if<support::JsonValue::object_t>();
        REQUIRE(scenario_object != nullptr);
        const auto inputs = scenario_object->find("inputs");
        const auto expected = scenario_object->find("expected");
        REQUIRE(inputs != scenario_object->end());
        REQUIRE(expected != scenario_object->end());
        const auto* input_object = inputs->second.get_if<support::JsonValue::object_t>();
        const auto* expected_object = expected->second.get_if<support::JsonValue::object_t>();
        REQUIRE(input_object != nullptr);
        REQUIRE(expected_object != nullptr);
        const auto tool = input_object->find("tool");
        REQUIRE(tool != input_object->end());
        INFO("artifact scenario tool: " << tool->second.get_string());

        const auto converted = expected_object->find("converted");
        REQUIRE(converted != expected_object->end());
        const auto* converted_object = converted->second.get_if<support::JsonValue::object_t>();
        REQUIRE(converted_object != nullptr);
        const auto content = converted_object->find("content");
        REQUIRE(content != converted_object->end());
        const auto* blocks = content->second.get_if<support::JsonValue::array_t>();
        REQUIRE(blocks != nullptr);
        REQUIRE(blocks->size() == 1);
        const auto* expected_block = blocks->front().get_if<support::JsonValue::object_t>();
        REQUIRE(expected_block != nullptr);
        const auto text = expected_block->find("text");
        REQUIRE(text != expected_block->end());

        auto executed = execute_structured(runtime, tools.front(), tool->second.get_string());
        REQUIRE(executed.has_value());
        REQUIRE(executed->content.size() == 1);
        const auto* actual_text = std::get_if<ai::TextContent>(&executed->content.front());
        REQUIRE(actual_text != nullptr);
        CHECK(actual_text->text == text->second.get_string());
    }
}

TEST_CASE("an MCP tools/call preserves actual wire order for reverse, nested and integer-key objects",
        "[coding_agent][mcp][issue1101][spec]") {
    struct OrderCase {
        std::string kind;
        std::string expected_text;
        std::string map_sorted_text;
        support::JsonValue expected_value;
    };
    const std::vector<OrderCase> cases{
            {"object-reverse",
                    "{\n  \"z\": \"last\",\n  \"a\": \"first\",\n  \"m\": \"middle\"\n}",
                    "{\n  \"a\": \"first\",\n  \"m\": \"middle\",\n  \"z\": \"last\"\n}",
                    support::JsonValue::object_t{
                            {"z", "last"},
                            {"a", "first"},
                            {"m", "middle"},
                    }},
            {"object-nested",
                    "{\n  \"outer\": {\n    \"z\": 1,\n    \"a\": 2\n  },\n  \"first\": true\n}",
                    "{\n  \"first\": true,\n  \"outer\": {\n    \"a\": 2,\n    \"z\": 1\n  }\n}",
                    support::JsonValue::object_t{
                            {"outer", support::JsonValue::object_t{{"z", 1}, {"a", 2}}},
                            {"first", true},
                    }},
            {"object-integer-keys",
                    "{\n  \"2\": \"two\",\n  \"10\": \"ten\",\n  \"b\": true,\n  \"a\": false\n}",
                    "{\n  \"10\": \"ten\",\n  \"2\": \"two\",\n  \"a\": false,\n  \"b\": true\n}",
                    support::JsonValue::object_t{
                            {"10", "ten"},
                            {"2", "two"},
                            {"b", true},
                            {"a", false},
                    }},
    };

    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    for (const auto& order_case : cases) {
        INFO("structured order kind: " << order_case.kind);
        auto executed = execute_structured(runtime, tools.front(), order_case.kind);
        REQUIRE(executed.has_value());
        REQUIRE(executed->content.size() == 1);
        const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
        REQUIRE(text != nullptr);
        CHECK(text->text == order_case.expected_text);
        // Negative control: a map/sorted implementation emits this, so the
        // assertion above is discriminating rather than order-insensitive.
        CHECK(text->text != order_case.map_sorted_text);
        CHECK_FALSE(executed->is_error);
        REQUIRE(executed->details.has_value());
        check_script_result(order_case.expected_value, *executed->details);
    }
}

TEST_CASE("an MCP tools/call renders structuredContent numbers exactly as JSON.stringify",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    auto executed = execute_structured(runtime, tools.front(), "object-numbers");
    REQUIRE(executed.has_value());
    REQUIRE(executed->content.size() == 1);
    const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "{\n  \"integer\": 42,\n  \"negative\": -7,\n  \"float\": 1.5,\n  \"large\": "
                        "9007199254740992,\n  \"exponent\": 1e+21\n}");
    CHECK_FALSE(executed->is_error);
}

TEST_CASE("an MCP tools/call renders every JSON number shape exactly as JSON.stringify",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);
    auto executed = execute_structured(runtime, tools.front(), "object-number-shapes");
    REQUIRE(executed.has_value());
    REQUIRE(executed->content.size() == 1);
    const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "{\n  \"zero\": 0,\n  \"negativeZero\": 0,\n  \"small\": 0.000001,\n  \"smaller\": 1e-7,\n  "
                        "\"large\": 100000000000000000000,\n  \"exponent\": 1e+21,\n  \"fraction\": 123.45,\n  "
                        "\"precise\": 1.2345678901234567e+30\n}");
    CHECK_FALSE(executed->is_error);
}

TEST_CASE("an MCP tools/call keeps non-empty content separate from structuredContent",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);

    auto executed = execute_structured(runtime, tools.front(), "content-text-structured");
    REQUIRE(executed.has_value());
    REQUIRE(executed->content.size() == 1);
    const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "alpha");
    CHECK_FALSE(executed->is_error);
    REQUIRE(executed->details.has_value());
    const auto* details = executed->details->get_if<support::JsonValue::object_t>();
    REQUIRE(details != nullptr);
    const auto content = details->find("content");
    REQUIRE(content != details->end());
    REQUIRE(content->second.get_array().size() == 1);
    CHECK(content->second.get_array().front().at("text").get_string() == "alpha");
    const auto structured = details->find("structuredContent");
    REQUIRE(structured != details->end());
    CHECK(structured->second.at("ok").get_boolean());
}

TEST_CASE("an MCP tools/call falls back for empty content and adds pi's isError text",
        "[coding_agent][mcp][issue1101][spec]") {
    tests::RuntimeFixture runtime;
    auto tools = load_structured_tools(runtime);

    auto empty_content = execute_structured(runtime, tools.front(), "empty-content-structured");
    REQUIRE(empty_content.has_value());
    REQUIRE(empty_content->content.size() == 1);
    const auto* fallback = std::get_if<ai::TextContent>(&empty_content->content.front());
    REQUIRE(fallback != nullptr);
    CHECK(fallback->text == "{\n  \"ok\": true\n}");
    CHECK_FALSE(empty_content->is_error);

    for (const auto& [kind, content_blocks] : std::vector<std::pair<std::string, std::size_t>>{
                 {"missing-content-isError", 0}, {"empty-text-isError", 1}}) {
        INFO("isError kind: " << kind);
        auto error = execute_structured(runtime, tools.front(), kind);
        REQUIRE(error.has_value());
        REQUIRE(error->content.size() == content_blocks + 1);
        const auto* existing = std::get_if<ai::TextContent>(&error->content.front());
        REQUIRE(existing != nullptr);
        CHECK(existing->text == (content_blocks == 0 ? "MCP tool structured/structured returned an error" : ""));
        if (content_blocks == 1) {
            const auto* fallback = std::get_if<ai::TextContent>(&error->content.back());
            REQUIRE(fallback != nullptr);
            CHECK(fallback->text == "MCP tool structured/structured returned an error");
        }
        CHECK(error->is_error);
        REQUIRE(error->details.has_value());
        const auto* details = error->details->get_if<support::JsonValue::object_t>();
        REQUIRE(details != nullptr);
        const auto is_error = details->find("isError");
        REQUIRE(is_error != details->end());
        CHECK(is_error->second.get_boolean());
        const auto content = details->find("content");
        REQUIRE(content != details->end());
        CHECK(content->second.get_array().size() == content_blocks);
    }
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
        CHECK(executed.error().message == "Invalid MCP tools/call result");
        CHECK(executed.error().detail == "code -32600: Invalid MCP tools/call result");
        CHECK(executed.error().context ==
                std::optional<std::string>{"MCP server 'structured' sent an invalid tools/call result"});
    }
}
