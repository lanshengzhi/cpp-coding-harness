// The v1 policy for `tools/list_changed` (ADR 0064; spec #833 story 12 and
// the v1 `subscriptions/listen` decision; issue #836). pike does not subscribe
// to `subscriptions/listen`, so the notification is safely ignored wherever it
// arrives: it never reconnects the connection and never refreshes the catalog,
// and catalog freshness stays driven by the server-provided `ttlMs` hint.

#include <cch/mcp/UpstreamClient.hpp>
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

/// A storm of `tools/list_changed` notifications, as a flaky Upstream emits
/// when its catalog churns.
[[nodiscard]] std::vector<JsonValue> list_changed_storm(std::size_t count) {
    std::vector<JsonValue> storm;
    storm.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        storm.push_back(tests::tools_list_changed_notification());
    }
    return storm;
}

} // namespace

TEST_CASE("a tools/list_changed storm ahead of a response is ignored without reconnecting or refreshing",
        "[mcp][notifications][issue836][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result({tests::tool_entry("read_issue")}),
            });
    transport->answer("tools/call",
            {
                    .result = tests::tool_call_result(JsonValue::array_t{JsonValue::object_t{
                            {"type", JsonValue("text")},
                            {"text", JsonValue("ABC-1")},
                    }}),
                    .leading_messages = list_changed_storm(25),
            });

    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    const auto after_listing = transport->request_count();

    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools.front(),
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);
    CHECK(outcome->content.get_array().at(0).at("text").get_string() == "ABC-1");

    // One discover, one tools/list, one tools/call: the storm cost no extra
    // exchange of either kind, so it can never become a reconnect or refresh
    // storm.
    CHECK(transport->request_count() == after_listing + 1);
    std::size_t listings = 0;
    for (std::size_t index = 0; index < transport->request_count(); ++index) {
        auto method = transport->recorded_method(index);
        REQUIRE(method.has_value());
        if (*method == "tools/list") {
            ++listings;
        }
    }
    CHECK(listings == 1);
}

TEST_CASE("a notification ahead of a paginated tools/list page does not restart the walk",
        "[mcp][notifications][issue836][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result()});
    auto served = std::make_shared<std::size_t>(0);
    transport->answer_with("tools/list", [served](const support::JsonValue&) {
        const auto page = *served;
        *served += 1;
        if (page == 0) {
            return tests::ScriptedMcpAnswer{
                    .result = tests::tool_list_result({tests::tool_entry("read_issue")}, "page-2"),
                    .leading_messages = list_changed_storm(3),
            };
        }
        return tests::ScriptedMcpAnswer{
                .result = tests::tool_list_result({tests::tool_entry("close_issue")}, std::nullopt),
        };
    });

    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 2);
    CHECK(catalog->tools[0].name == "read_issue");
    CHECK(catalog->tools[1].name == "close_issue");
    CHECK(transport->request_count() == 3);
}
