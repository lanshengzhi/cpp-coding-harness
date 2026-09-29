// Catalog containment for a buggy or malicious Upstream MCP Server (spec #833
// story 24, issue #836). Every case is driven through the one injected
// transport seam, so the limits are proved against the real request/response
// chain rather than against a decoder in isolation.

#include <cch/mcp/UpstreamClient.hpp>
#include "mcp/Protocol.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;
using support::JsonValue;

namespace {

[[nodiscard]] std::shared_ptr<tests::ScriptedMcpTransport> make_transport() {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result()});
    return transport;
}

[[nodiscard]] mcp::UpstreamClientOptions endpoint_options() {
    return mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"};
}

[[nodiscard]] std::vector<JsonValue> distinct_tools(std::size_t count) {
    std::vector<JsonValue> tools;
    tools.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        tools.push_back(tests::tool_entry("tool_" + std::to_string(index)));
    }
    return tools;
}

/// One `tools/list` answer carrying `page_size` fresh tools and the cursor of
/// the page after it.
[[nodiscard]] tests::ScriptedMcpAnswer catalog_page(std::size_t first_index, std::size_t page_size) {
    std::vector<JsonValue> tools;
    tools.reserve(page_size);
    for (std::size_t index = 0; index < page_size; ++index) {
        tools.push_back(tests::tool_entry("tool_" + std::to_string(first_index + index)));
    }
    return tests::ScriptedMcpAnswer{
            .result = tests::tool_list_result(std::move(tools), "cursor-" + std::to_string(first_index + page_size)),
    };
}

} // namespace

TEST_CASE("a catalog at the per-server tool cap is admitted whole", "[mcp][catalog][limits][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result =
                            tests::tool_list_result(distinct_tools(mcp::protocol::kMaxToolsPerUpstream), std::nullopt),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    CHECK(catalog->tools.size() == mcp::protocol::kMaxToolsPerUpstream);
}

TEST_CASE("a catalog past the per-server tool cap is rejected", "[mcp][catalog][limits][issue836][spec]") {
    auto transport = make_transport();
    transport->answer_with("tools/list", [](const support::JsonValue& params) {
        const auto& object = params.get_object();
        if (object.count("cursor") == 0) {
            return catalog_page(0, mcp::protocol::kMaxToolsPerUpstream);
        }
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result({tests::tool_entry("one_too_many")}, std::nullopt),
        };
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().code == support::ErrorCode::ResourceLimit);
}

TEST_CASE("a pagination walk at the cursor cap is admitted and one page past it is rejected",
        "[mcp][catalog][limits][issue836][spec]") {
    SECTION("a walk that ends exactly at the cap succeeds") {
        auto transport = make_transport();
        auto served = std::make_shared<std::size_t>(0);
        transport->answer_with("tools/list", [served](const support::JsonValue&) {
            const auto page = *served;
            *served += 1;
            if (page + 1 == mcp::protocol::kMaxListPagesPerUpstream) {
                return tests::ScriptedMcpAnswer{
                        .result = tests::tool_list_result({tests::tool_entry("last_tool")}, std::nullopt),
                };
            }
            return catalog_page(page, 1);
        });

        mcp::UpstreamClient client("executor", transport, endpoint_options());
        auto catalog = tests::drive(client.list_tools());
        REQUIRE(catalog.has_value());
        CHECK(catalog->tools.size() == mcp::protocol::kMaxListPagesPerUpstream);
        CHECK(transport->request_count() == mcp::protocol::kMaxListPagesPerUpstream + 1);
    }

    SECTION("a walk that needs one page more is rejected") {
        auto transport = make_transport();
        auto served = std::make_shared<std::size_t>(0);
        transport->answer_with("tools/list", [served](const support::JsonValue&) {
            const auto page = *served;
            *served += 1;
            return catalog_page(page, 1);
        });

        mcp::UpstreamClient client("executor", transport, endpoint_options());
        auto catalog = tests::drive(client.list_tools());
        REQUIRE_FALSE(catalog.has_value());
        CHECK(catalog.error().code == support::ErrorCode::ResourceLimit);
        CHECK(transport->request_count() == mcp::protocol::kMaxListPagesPerUpstream + 1);
    }
}

TEST_CASE("a duplicate tool name across pages is rejected", "[mcp][catalog][limits][issue836][spec]") {
    auto transport = make_transport();
    transport->answer_with("tools/list", [](const support::JsonValue& params) {
        const auto& object = params.get_object();
        if (object.count("cursor") == 0) {
            return tests::ScriptedMcpAnswer{
                    .result = tests::tool_list_result({tests::tool_entry("read_issue")}, "page-2"),
            };
        }
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result({tests::tool_entry("read_issue")}, std::nullopt),
        };
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().code == support::ErrorCode::Validation);
    CHECK(catalog.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("a pagination cursor the Upstream already returned is rejected instead of followed",
        "[mcp][catalog][limits][issue836][spec]") {
    auto transport = make_transport();
    transport->answer_with("tools/list", [](const support::JsonValue& params) {
        const auto& object = params.get_object();
        const std::string cursor = object.count("cursor") == 0 ? "" : object.at("cursor").get_string();
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result(
                        {tests::tool_entry(cursor.empty() ? "first" : "second")}, "always-the-same"),
        };
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().code == support::ErrorCode::Validation);
    CHECK(catalog.error().message.find("cursor") != std::string::npos);
    CHECK(transport->request_count() == 3);
}

TEST_CASE("a tool with an invalid x-mcp-header annotation is dropped from the catalog",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const std::vector<JsonValue> rejected{
            // Mirrors a parameter the tool never declared.
            JsonValue::object_t{{"x-mcp-header",
                    JsonValue::object_t{{"undeclared", JsonValue::object_t{{"name", JsonValue("Channel")}}}}}},
            // Claims a transport-reserved header name.
            JsonValue::object_t{{"x-mcp-header",
                    JsonValue::object_t{{"channel", JsonValue::object_t{{"name", JsonValue("Mcp-Method")}}}}}},
            // Claims the Mcp-Param- namespace the mirroring writes into.
            JsonValue::object_t{{"x-mcp-header",
                    JsonValue::object_t{{"channel", JsonValue::object_t{{"name", JsonValue("Mcp-Param-Channel")}}}}}},
            // A header name that is not a field-name token.
            JsonValue::object_t{{"x-mcp-header",
                    JsonValue::object_t{{"channel", JsonValue::object_t{{"name", JsonValue("bad header")}}}}}},
            // A missing header name.
            JsonValue::object_t{{"x-mcp-header", JsonValue::object_t{{"channel", JsonValue::object_t{}}}}},
            // A non-boolean `required`.
            JsonValue::object_t{{"x-mcp-header",
                    JsonValue::object_t{{"channel",
                            JsonValue::object_t{{"name", JsonValue("Channel")}, {"required", JsonValue("yes")}}}}}},
            // A non-object descriptor.
            JsonValue::object_t{{"x-mcp-header", JsonValue::object_t{{"channel", JsonValue("Channel")}}}},
            // A non-object annotation.
            JsonValue::object_t{{"x-mcp-header", JsonValue("Channel")}},
    };

    for (const auto& annotations : rejected) {
        auto transport = make_transport();
        transport->answer("tools/list",
                {
                        .result =
                                tests::tool_list_result({tests::tool_entry("post_message", {"channel"}, annotations)}),
                });
        mcp::UpstreamClient client("executor", transport, endpoint_options());
        auto catalog = tests::drive(client.list_tools());
        // The tool is rejected on its own: an invalid annotation drops that
        // tool from the catalog rather than costing the server its whole page.
        REQUIRE(catalog.has_value());
        CHECK(catalog->tools.empty());
    }
}

TEST_CASE("a tool mirroring one header name twice is dropped from the catalog",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header",
                    JsonValue::object_t{{"channel", JsonValue::object_t{{"name", JsonValue("Channel")}}},
                            {"target", JsonValue::object_t{{"name", JsonValue("channel")}}}}},
    };
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result(
                            {tests::tool_entry("post_message", {"channel", "target"}, annotations)}),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    CHECK(catalog->tools.empty());
}

TEST_CASE("an invalid x-mcp-header annotation is dropped while a valid annotated sibling survives",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const JsonValue invalid = JsonValue::object_t{
            {"x-mcp-header", JsonValue::object_t{{"channel", JsonValue::object_t{{"name", JsonValue("Mcp-Method")}}}}},
    };
    const JsonValue valid = JsonValue::object_t{
            {"x-mcp-header",
                    JsonValue::object_t{{"channel",
                            JsonValue::object_t{{"name", JsonValue("Channel")}, {"required", JsonValue(true)}}}}},
    };
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("plain_tool"),
                            tests::tool_entry("broken_tool", {"channel"}, invalid),
                            tests::tool_entry("annotated_tool", {"channel"}, valid)}),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 2);
    CHECK(catalog->tools[0].name == "plain_tool");
    CHECK(catalog->tools[1].name == "annotated_tool");
    CHECK(catalog->tools[1].header_parameters.size() == 1);
}

TEST_CASE("a required mirrored argument that the call omits fails the call before it reaches the Upstream",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header",
                    JsonValue::object_t{{"channel",
                            JsonValue::object_t{{"name", JsonValue("Channel")}, {"required", JsonValue(true)}}}}},
    };
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("post_message", {"channel"}, annotations)}),
            });
    transport->answer("tools/call", {.result = tests::tool_call_result(JsonValue::array_t{})});

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Validation);
    CHECK(transport->request_count() == 2);
}

TEST_CASE("a header-unsafe mirrored value is sent under the base64 sentinel",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header", JsonValue::object_t{{"note", JsonValue::object_t{{"name", JsonValue("Note")}}}}},
    };
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("post_message", {"note"}, annotations)}),
            });
    transport->answer("tools/call", {.result = tests::tool_call_result(JsonValue::array_t{})});

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = JsonValue::object_t{{"note", JsonValue(std::string("a\nb"))}},
    }));
    REQUIRE(outcome.has_value());
    const auto& headers = transport->requests().back().headers;
    CHECK(headers.at("Mcp-Param-Note") == "base64:YQpi");
}

TEST_CASE("a mirrored value that would collide with the base64 sentinel is encoded",
        "[mcp][catalog][header-annotation][issue836][spec]") {
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header", JsonValue::object_t{{"note", JsonValue::object_t{{"name", JsonValue("Note")}}}}},
    };
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("post_message", {"note"}, annotations)}),
            });
    transport->answer("tools/call", {.result = tests::tool_call_result(JsonValue::array_t{})});

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = JsonValue::object_t{{"note", JsonValue("base64:YWxi")}},
    }));
    REQUIRE(outcome.has_value());
    const auto& headers = transport->requests().back().headers;
    CHECK(headers.at("Mcp-Param-Note") == "base64:YmFzZTY0OllXeGk=");
}
