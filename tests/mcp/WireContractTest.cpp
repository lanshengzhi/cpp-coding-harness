// The MCP wire DTO contract for the released 2026-07-28 revision (ADR 0064,
// issue #836).
//
// The released revision's `schema/2026-07-28/schema.json` is not vendored in
// this repository, and inventing a local copy of a schema the build would then
// "validate against" would only let the implementation and the golden drift
// wrong together. These cases therefore pin the concrete DTO shapes the
// client stack actually encodes and decodes, byte for byte, together with the
// negative cases that keep the contract from loosening: a missing required
// field, a wrong capability set, and a protocol-version mismatch.
//
// The request-side goldens are asserted on the bytes a scripted Upstream
// received, not on internal values, so the framing, the reserved `_meta` keys,
// and the request headers are all covered by the same evidence.

#include <cch/mcp/UpstreamClient.hpp>
#include "mcp/EraAdapter.hpp"
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/WireDto.hpp"
#include "support/Json.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using support::JsonValue;

namespace {

/// The exact body the client stack frames for the first `server/discover` of a
/// connection. The `_meta` keys are the reserved `io.modelcontextprotocol/*`
/// pair and the advertised capability set is exactly
/// `elicitation: {form: {}, url: {}}` (ADR 0064).
constexpr std::string_view kDiscoverRequestBody{
        R"({"id":1,"jsonrpc":"2.0","method":"server/discover","params":{"_meta":)"
        R"({"io.modelcontextprotocol/clientCapabilities":{"elicitation":{"form":{},"url":{}}},)"
        R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})"};

/// The exact body the client stack frames for a `tools/call` that mirrors one
/// annotated parameter into an `Mcp-Param-*` header.
constexpr std::string_view kCallRequestBody{
        R"({"id":3,"jsonrpc":"2.0","method":"tools/call","params":)"
        R"({"_meta":{"io.modelcontextprotocol/clientCapabilities":{"elicitation":{"form":{},"url":{}}},)"
        R"("io.modelcontextprotocol/protocolVersion":"2026-07-28"},)"
        R"("arguments":{"channel":"ops"},"name":"post_message"}})"};

[[nodiscard]] JsonValue without(JsonValue object, std::string key) {
    object.get_object().erase(key);
    return object;
}

} // namespace

TEST_CASE("every client request carries exactly the reserved _meta keys and only elicitation capabilities",
        "[mcp][wire][contract][issue836][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});

    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    REQUIRE(tests::drive(client.list_tools()).has_value());
    REQUIRE(transport->request_count() == 2);

    CHECK(transport->requests().at(0).body == kDiscoverRequestBody);
    for (const auto& request : transport->requests()) {
        auto meta = request.body.find(
                R"("io.modelcontextprotocol/clientCapabilities":{"elicitation":{"form":{},"url":{}}})");
        CHECK(meta != std::string::npos);
        CHECK(request.body.find(R"("roots")") == std::string::npos);
        CHECK(request.body.find(R"("sampling")") == std::string::npos);
        CHECK(request.body.find(R"("extensions")") == std::string::npos);
        CHECK(request.headers.at("MCP-Protocol-Version") == "2026-07-28");
        CHECK(request.headers.at("Mcp-Method") != "");
    }
}

TEST_CASE(
        "the era adapter rewrites the reserved _meta keys a caller supplied", "[mcp][wire][contract][issue836][spec]") {
    JsonValue params = JsonValue::object_t{
            {"_meta",
                    JsonValue::object_t{
                            {"io.modelcontextprotocol/protocolVersion", JsonValue("2024-11-05")},
                            {"io.modelcontextprotocol/clientCapabilities",
                                    JsonValue::object_t{{"sampling", JsonValue::object_t{}}}},
                            {"trace", JsonValue("caller-value")},
                    }},
    };
    auto adapter = mcp::era::select_era_adapter(tests::discover_result());
    REQUIRE(adapter.has_value());
    (*adapter)->attach_request_meta(params);

    const auto& meta = params.at("_meta");
    CHECK(meta.at("io.modelcontextprotocol/protocolVersion").get_string() == "2026-07-28");
    CHECK(meta.at("io.modelcontextprotocol/clientCapabilities").get_object().size() == 1);
    CHECK(meta.at("io.modelcontextprotocol/clientCapabilities").at("elicitation").get_object().size() == 2);
    CHECK(meta.at("io.modelcontextprotocol/clientCapabilities").at("elicitation").at("form").get_object().empty());
    CHECK(meta.at("io.modelcontextprotocol/clientCapabilities").at("elicitation").at("url").get_object().empty());
    CHECK(meta.at("io.modelcontextprotocol/clientCapabilities").get_object().count("sampling") == 0);
    CHECK(meta.at("trace").get_string() == "caller-value");
}

TEST_CASE("a tools/call is framed byte for byte with its reserved _meta and its Mcp-Name header",
        "[mcp][wire][contract][issue836][spec]") {
    using JsonValue = support::JsonValue;
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result("executor", "1.4.0", "")});
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header",
                    JsonValue::object_t{{"channel",
                            JsonValue::object_t{{"name", JsonValue("Channel")}, {"required", JsonValue(true)}}}}},
    };
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("post_message", {"channel"}, annotations)}),
            });
    transport->answer("tools/call", {.result = tests::tool_call_result(JsonValue::array_t{})});

    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 1);
    REQUIRE(catalog->tools.front().header_parameters.size() == 1);
    CHECK(catalog->tools.front().header_parameters.front().argument_name == "channel");
    CHECK(catalog->tools.front().header_parameters.front().header_name == "Channel");
    CHECK(catalog->tools.front().header_parameters.front().required);

    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = JsonValue::object_t{{"channel", JsonValue("ops")}},
    }));
    REQUIRE(outcome.has_value());

    REQUIRE(transport->request_count() == 3);
    CHECK(transport->requests().at(2).body == kCallRequestBody);
    const auto& headers = transport->requests().at(2).headers;
    CHECK(headers.at("Mcp-Method") == "tools/call");
    CHECK(headers.at("Mcp-Name") == "post_message");
    CHECK(headers.at("Mcp-Param-Channel") == "ops");
}

TEST_CASE("a server/discover result decodes the released 2026-07-28 shape", "[mcp][wire][contract][issue836][spec]") {
    auto info = mcp::dto::read_discover_result(tests::discover_result("executor", "1.4.0", "codemode idioms"));
    REQUIRE(info.has_value());
    CHECK(info->name == "executor");
    CHECK(info->version == "1.4.0");
    CHECK(info->capabilities == std::vector<std::string>{"tools"});
    CHECK(info->instructions == "codemode idioms");
}

TEST_CASE("a server/discover result missing a required field is rejected", "[mcp][wire][contract][issue836][spec]") {
    CHECK_FALSE(mcp::dto::read_discover_result(without(tests::discover_result(), "protocolVersion")).has_value());
    CHECK_FALSE(mcp::dto::read_discover_result(without(tests::discover_result(), "serverInfo")).has_value());
    CHECK_FALSE(mcp::dto::read_discover_result(without(tests::discover_result(), "capabilities")).has_value());
    JsonValue without_name = tests::discover_result();
    without_name.get_object()["serverInfo"] = JsonValue::object_t{{"version", JsonValue("1.4.0")}};
    CHECK_FALSE(mcp::dto::read_discover_result(without_name).has_value());
    JsonValue without_version = tests::discover_result();
    without_version.get_object()["serverInfo"] = JsonValue::object_t{{"name", JsonValue("executor")}};
    CHECK_FALSE(mcp::dto::read_discover_result(without_version).has_value());

    JsonValue wrong_name = tests::discover_result();
    wrong_name.get_object()["serverInfo"] =
            JsonValue::object_t{{"name", JsonValue(7)}, {"version", JsonValue("1.4.0")}};
    CHECK_FALSE(mcp::dto::read_discover_result(wrong_name).has_value());

    JsonValue wrong_instructions = tests::discover_result();
    wrong_instructions.get_object()["instructions"] = JsonValue(7);
    CHECK_FALSE(mcp::dto::read_discover_result(wrong_instructions).has_value());
}

TEST_CASE("a server/discover result with an undeclared capability set is rejected",
        "[mcp][wire][contract][issue836][spec]") {
    for (const auto& capability : std::vector<std::string>{"resources", "prompts", "roots", "sampling", "extensions"}) {
        JsonValue result = tests::discover_result();
        result.get_object()["capabilities"] =
                JsonValue::object_t{{capability, JsonValue::object_t{}}, {"tools", JsonValue::object_t{}}};
        auto decoded = mcp::dto::read_discover_result(result);
        CHECK_FALSE(decoded.has_value());
    }
    JsonValue undeclared = tests::discover_result();
    undeclared.get_object()["capabilities"] =
            JsonValue::object_t{{"tools", JsonValue::object_t{{"listChanged", JsonValue("yes")}}}};
    CHECK_FALSE(mcp::dto::read_discover_result(undeclared).has_value());
}

TEST_CASE("a server/discover result with a mismatched protocol version is rejected",
        "[mcp][wire][contract][issue836][spec]") {
    for (const auto& revision : std::vector<std::string>{"2025-11-25", "2024-11-05", "2026-07-29", ""}) {
        JsonValue result = tests::discover_result();
        result.get_object()["protocolVersion"] = JsonValue(revision);
        CHECK_FALSE(mcp::dto::read_discover_result(result).has_value());
        CHECK_FALSE(mcp::era::select_era_adapter(result).has_value());
    }
}

TEST_CASE("a tools/list result decodes the released shape and preserves the server's order",
        "[mcp][wire][contract][issue836][spec]") {
    auto page = mcp::dto::read_tool_list_page(
            tests::tool_list_result({tests::tool_entry("zeta"), tests::tool_entry("alpha")}, "cursor-2"));
    REQUIRE(page.has_value());
    REQUIRE(page->tools.size() == 2);
    CHECK(page->tools[0].name == "zeta");
    CHECK(page->tools[1].name == "alpha");
    CHECK(page->tools[0].description == "an upstream tool");
    CHECK(page->next_cursor.has_value());
    CHECK(*page->next_cursor == "cursor-2");

    auto terminal = mcp::dto::read_tool_list_page(tests::tool_list_result({}));
    REQUIRE(terminal.has_value());
    CHECK_FALSE(terminal->next_cursor.has_value());
}

TEST_CASE("a tools/list result missing its tools member is rejected", "[mcp][wire][contract][issue836][spec]") {
    JsonValue no_tools = JsonValue::object_t{{"nextCursor", JsonValue("c")}};
    CHECK_FALSE(mcp::dto::read_tool_list_page(no_tools).has_value());
    JsonValue not_an_array = JsonValue::object_t{{"tools", JsonValue::object_t{}}};
    CHECK_FALSE(mcp::dto::read_tool_list_page(not_an_array).has_value());
    CHECK_FALSE(mcp::dto::read_tool_list_page(tests::tool_list_result({}, "")).has_value());
    JsonValue nameless = tests::tool_list_result({JsonValue::object_t{{"description", JsonValue("nameless")}}});
    CHECK_FALSE(mcp::dto::read_tool_list_page(nameless).has_value());
}

TEST_CASE("a tools/call result decodes the released call_result shape", "[mcp][wire][contract][issue836][spec]") {
    auto outcome = mcp::dto::read_tool_call_result(
            tests::tool_call_result(JsonValue::array_t{JsonValue::object_t{{"type", JsonValue("text")}}}, true));
    REQUIRE(outcome.has_value());
    CHECK(outcome->is_error);
    REQUIRE(outcome->content.get_if<JsonValue::array_t>() != nullptr);
    CHECK(outcome->content.get_array().size() == 1);
}
