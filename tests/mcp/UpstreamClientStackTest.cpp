// The full MCP Host client stack driven end to end through the one injected
// transport seam (ADR 0065, issue #836). No DTO-only exercise appears here:
// every case scripts the Upstream's answers and then asserts what the client
// stack framed on the wire and what it made of the reply, so framing,
// `_meta`/header injection, the era probe, pagination, and the tool call are
// all covered by one seam.

#include <cch/mcp/UpstreamClient.hpp>
#include "support/ScriptedMcpTransport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kEndpoint{"https://upstream.invalid/mcp"};

[[nodiscard]] std::shared_ptr<tests::ScriptedMcpTransport> make_transport() {
    return std::make_shared<tests::ScriptedMcpTransport>();
}

[[nodiscard]] mcp::UpstreamClientOptions endpoint_options() {
    return mcp::UpstreamClientOptions{.url = std::string(kEndpoint)};
}

} // namespace

TEST_CASE("the client stack probes the era, walks the catalog, and calls a tool over one scripted transport",
        "[mcp][wire][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer_with("tools/list", [](const support::JsonValue& params) {
        const auto& object = params.get_object();
        if (object.count("cursor") == 0) {
            return tests::ScriptedMcpAnswer{
                    .result = tests::tool_list_result({tests::tool_entry("read_issue")}, "page-2"),
            };
        }
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result({tests::tool_entry("close_issue")}, std::nullopt),
        };
    });
    transport->answer("tools/call",
            {
                    .result = tests::tool_call_result(support::JsonValue::array_t{support::JsonValue::object_t{
                            {"type", support::JsonValue("text")},
                            {"text", support::JsonValue("closed ABC-1")},
                    }}),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 2);
    CHECK(catalog->tools[0].name == "read_issue");
    CHECK(catalog->tools[1].name == "close_issue");
    CHECK(catalog->instructions == "Use the codemode tools.");
    CHECK(client.era_name() == "modern");

    mcp::UpstreamToolCall call{
            .tool = catalog->tools.front(),
            .arguments = support::JsonValue::object_t{{"id", support::JsonValue("ABC-1")}},
    };
    auto outcome = tests::drive(client.call_tool(std::move(call)));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);
    CHECK(outcome->diagnostic.empty());
    REQUIRE(outcome->content.get_if<support::JsonValue::array_t>() != nullptr);
    CHECK(outcome->content.get_array().at(0).at("text").get_string() == "closed ABC-1");

    REQUIRE(transport->request_count() == 4);
    auto discover = transport->recorded_method(0);
    auto first_page = transport->recorded_method(1);
    auto second_page = transport->recorded_method(2);
    auto call_method = transport->recorded_method(3);
    REQUIRE(discover.has_value());
    REQUIRE(first_page.has_value());
    REQUIRE(second_page.has_value());
    REQUIRE(call_method.has_value());
    CHECK(*discover == "server/discover");
    CHECK(*first_page == "tools/list");
    CHECK(*second_page == "tools/list");
    CHECK(*call_method == "tools/call");

    auto second_page_params = transport->recorded_params(2);
    REQUIRE(second_page_params.has_value());
    CHECK(second_page_params->at("cursor").get_string() == "page-2");

    for (const auto& request : transport->requests()) {
        CHECK(request.url == kEndpoint);
    }
}

TEST_CASE("the era is selected once per connection and is not re-probed per request",
        "[mcp][wire][era][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});
    transport->answer("tools/call", {.result = tests::tool_call_result(support::JsonValue::array_t{})});

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    CHECK(client.era_name().empty());
    auto probe = tests::drive(client.probe_era());
    REQUIRE(probe.has_value());
    CHECK(probe->name == "executor");
    CHECK(probe->version == "1.4.0");
    CHECK(probe->capabilities == std::vector<std::string>{"tools"});
    CHECK(client.era_name() == "modern");

    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = support::JsonValue::object_t{},
    }));
    REQUIRE(outcome.has_value());

    std::size_t probes = 0;
    for (std::size_t index = 0; index < transport->request_count(); ++index) {
        auto method = transport->recorded_method(index);
        REQUIRE(method.has_value());
        if (*method == "server/discover") {
            ++probes;
        }
    }
    CHECK(probes == 1);
}

TEST_CASE("a reconnect re-probes the era on a fresh connection", "[mcp][wire][era][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list", {.result = tests::tool_list_result({tests::tool_entry("read_issue")})});

    {
        mcp::UpstreamClient first("executor", transport, endpoint_options());
        REQUIRE(tests::drive(first.list_tools()).has_value());
    }
    {
        mcp::UpstreamClient reconnected("executor", transport, endpoint_options());
        CHECK(reconnected.era_name().empty());
        REQUIRE(tests::drive(reconnected.list_tools()).has_value());
        CHECK(reconnected.era_name() == "modern");
    }
    CHECK(transport->request_count() == 4);
}
