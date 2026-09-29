// The defensive matrix (ADR 0064, ADR 0008; spec #833 stories 23 and 32;
// issue #836). A non-conformant Upstream fails loudly but locally: a
// -32021 capability error, an unrecognized `resultType`, and an unexpected
// Multi Round-Trip `input_required` result each fail exactly one tool call,
// and a sibling ordinary call on the same connection still succeeds.
//
// The cases run over the one injected transport seam, so each one also proves
// that the failure costs the Upstream no reconnection and no re-listing.

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

[[nodiscard]] std::shared_ptr<tests::ScriptedMcpTransport> make_transport() {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.result = tests::discover_result()});
    transport->answer("tools/list",
            {
                    .result = tests::tool_list_result(
                            {tests::tool_entry("read_issue"), tests::tool_entry("close_issue")}),
            });
    return transport;
}

[[nodiscard]] mcp::UpstreamClientOptions endpoint_options() {
    return mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"};
}

[[nodiscard]] JsonValue text_content(std::string text) {
    return JsonValue::array_t{JsonValue::object_t{{"type", JsonValue("text")}, {"text", JsonValue(std::move(text))}}};
}

} // namespace

TEST_CASE("a -32021 capability error fails one tool call and the next ordinary call still succeeds",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    auto served = std::make_shared<std::size_t>(0);
    transport->answer_with("tools/call", [served](const support::JsonValue&) {
        *served += 1;
        if (*served == 1) {
            return tests::ScriptedMcpAnswer{
                    .error_code = -32021,
                    .error_message = "the client must advertise the tasks capability",
            };
        }
        return tests::ScriptedMcpAnswer{.result = tests::tool_call_result(text_content("closed ABC-1"))};
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());

    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.find("-32021") != std::string::npos);
    CHECK(failed->diagnostic.find("client capability") != std::string::npos);

    auto recovered = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[1],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(recovered.has_value());
    CHECK_FALSE(recovered->is_error);
    CHECK(recovered->diagnostic.empty());

    // One discover, one tools/list, two tools/call: the failure cost no
    // reconnection and no re-listing.
    CHECK(transport->request_count() == 4);
}

TEST_CASE("an unrecognized resultType fails one tool call and the next ordinary call still succeeds",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    auto served = std::make_shared<std::size_t>(0);
    transport->answer_with("tools/call", [served](const support::JsonValue&) {
        *served += 1;
        if (*served == 1) {
            return tests::ScriptedMcpAnswer{
                    .result = JsonValue::object_t{{"resultType", JsonValue("streamed_effect")}},
            };
        }
        return tests::ScriptedMcpAnswer{.result = tests::tool_call_result(text_content("ok"))};
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());

    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.find("streamed_effect") != std::string::npos);
    CHECK(failed->diagnostic.find("resultType") != std::string::npos);

    auto recovered = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[1],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(recovered.has_value());
    CHECK_FALSE(recovered->is_error);
    CHECK(transport->request_count() == 4);
}

TEST_CASE("a tools/call result with no resultType fails one tool call", "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/call",
            {
                    .result = JsonValue::object_t{{"content", JsonValue::array_t{}}},
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.find("resultType") != std::string::npos);
}

TEST_CASE("an unexpected input_required result fails one tool call and the next ordinary call still succeeds",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    auto served = std::make_shared<std::size_t>(0);
    transport->answer_with("tools/call", [served](const support::JsonValue&) {
        *served += 1;
        if (*served == 1) {
            return tests::ScriptedMcpAnswer{
                    .result =
                            JsonValue::object_t{
                                    {"resultType", JsonValue("input_required")},
                                    {"inputRequests",
                                            JsonValue::array_t{JsonValue::object_t{
                                                    {"id", JsonValue("req-1")},
                                                    {"type", JsonValue("elicitation")},
                                            }}},
                            },
            };
        }
        return tests::ScriptedMcpAnswer{.result = tests::tool_call_result(text_content("ok"))};
    });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());

    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.find("input_required") != std::string::npos);

    auto recovered = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[1],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(recovered.has_value());
    CHECK_FALSE(recovered->is_error);
    CHECK(transport->request_count() == 4);
}

TEST_CASE("an Upstream-reported tool error is a failed call that keeps the server's own content",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/call",
            {
                    .result = tests::tool_call_result(text_content("issue ABC-1 was not found"), true),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.empty());
    CHECK(failed->content.get_array().at(0).at("text").get_string() == "issue ABC-1 was not found");
}

TEST_CASE("an ordinary JSON-RPC error fails one tool call with its code and message",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/call",
            {
                    .error_code = -32602,
                    .error_message = "the call was rejected as invalid",
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto failed = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(failed.has_value());
    CHECK(failed->is_error);
    CHECK(failed->diagnostic.find("-32602") != std::string::npos);
    CHECK(failed->diagnostic.find("rejected as invalid") != std::string::npos);
}

TEST_CASE("a transport failure stays an operation error rather than becoming a failed tool call",
        "[mcp][defensive][cancellation][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/call",
            {
                    .transport_error = support::make_error(
                            support::ErrorCode::Cancelled, "the call was cancelled", "the session stop token fired"),
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    auto outcome = tests::drive(client.call_tool(mcp::UpstreamToolCall{
            .tool = catalog->tools[0],
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Cancelled);
}

TEST_CASE("an Upstream refusal on tools/list is an operation error, not a tool call",
        "[mcp][defensive][issue836][spec]") {
    auto transport = make_transport();
    transport->answer("tools/list",
            {
                    .error_code = -32001,
                    .error_message = "the catalog is unavailable",
            });

    mcp::UpstreamClient client("executor", transport, endpoint_options());
    auto catalog = tests::drive(client.list_tools());
    REQUIRE_FALSE(catalog.has_value());
    CHECK(catalog.error().code == support::ErrorCode::Validation);
    CHECK(catalog.error().detail.find("-32001") != std::string::npos);
}
