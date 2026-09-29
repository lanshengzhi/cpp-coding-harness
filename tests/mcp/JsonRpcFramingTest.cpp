// JSON-RPC framing inside `cch_mcp` (issue #836). A Streamable HTTP response
// may place a server notification ahead of the response it belongs to, so the
// client stack reads a body positionally rather than whole-body; these cases
// pin that framing and the envelope validation it enforces.

#include <cch/mcp/UpstreamClient.hpp>
#include "mcp/JsonRpc.hpp"
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

[[nodiscard]] JsonValue notification(std::string_view method) {
    return JsonValue::object_t{{"jsonrpc", JsonValue("2.0")}, {"method", JsonValue(std::string(method))}};
}

[[nodiscard]] std::string frame(const support::JsonValue& value) {
    const auto text = support::write_json(value);
    return text ? *text : std::string{};
}

} // namespace

TEST_CASE("a request body is framed as one JSON-RPC 2.0 request", "[mcp][framing][issue836][spec]") {
    const auto request = mcp::jsonrpc::encode_request(7, "tools/list", JsonValue::object_t{{"cursor", JsonValue("c")}});
    auto encoded = support::write_json(request);
    REQUIRE(encoded.has_value());
    CHECK(*encoded == R"({"id":7,"jsonrpc":"2.0","method":"tools/list","params":{"cursor":"c"}})");
}

TEST_CASE("a body carrying several framed messages is split in order", "[mcp][framing][issue836][spec]") {
    const std::string body =
            frame(notification("notifications/tools/list_changed")) +
            frame(mcp::jsonrpc::encode_result(1, JsonValue::object_t{{"tools", JsonValue::array_t{}}})) +
            frame(notification("notifications/progress")) + frame(mcp::jsonrpc::encode_result(2, JsonValue("second")));
    auto messages = mcp::jsonrpc::split_messages(body);
    REQUIRE(messages.has_value());
    REQUIRE(messages->size() == 4);

    auto first = mcp::jsonrpc::decode_message(messages->at(0));
    auto second = mcp::jsonrpc::decode_message(messages->at(1));
    auto third = mcp::jsonrpc::decode_message(messages->at(2));
    auto fourth = mcp::jsonrpc::decode_message(messages->at(3));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    REQUIRE(fourth.has_value());
    CHECK(first->is_notification());
    CHECK(first->method == "notifications/tools/list_changed");
    CHECK_FALSE(second->is_notification());
    CHECK(second->id == 1.0);
    CHECK(third->is_notification());
    CHECK(fourth->id == 2.0);
}

TEST_CASE("a brace inside a string does not end a frame early", "[mcp][framing][issue836][spec]") {
    const JsonValue nested = JsonValue::object_t{{"text", JsonValue("a}b{c[")}};
    const std::string body =
            frame(mcp::jsonrpc::encode_result(1, nested)) + frame(mcp::jsonrpc::encode_result(2, JsonValue("next")));
    auto messages = mcp::jsonrpc::split_messages(body);
    REQUIRE(messages.has_value());
    REQUIRE(messages->size() == 2);
    auto first = mcp::jsonrpc::decode_message(messages->at(0));
    REQUIRE(first.has_value());
    CHECK(first->result.at("text").get_string() == "a}b{c[");
}

TEST_CASE("an unbalanced or truncated body is a framing violation", "[mcp][framing][issue836][spec]") {
    CHECK_FALSE(mcp::jsonrpc::split_messages(R"({"id":1,"jsonrpc":)").has_value());
    CHECK_FALSE(mcp::jsonrpc::split_messages("not json at all").has_value());
    CHECK_FALSE(mcp::jsonrpc::split_messages(R"JSON({"id":1} trailing)JSON").has_value());
    CHECK(mcp::jsonrpc::split_messages("").has_value());
}

TEST_CASE("a message that is not JSON-RPC 2.0 is rejected", "[mcp][framing][issue836][spec]") {
    CHECK_FALSE(mcp::jsonrpc::decode_message(JsonValue::array_t{}).has_value());
    CHECK_FALSE(mcp::jsonrpc::decode_message(JsonValue::object_t{{"method", JsonValue("tools/list")}}).has_value());
    CHECK_FALSE(mcp::jsonrpc::decode_message(
            JsonValue::object_t{{"id", JsonValue(1.0)}, {"jsonrpc", JsonValue("1.0")}, {"result", JsonValue{}}})
                    .has_value());
    CHECK_FALSE(mcp::jsonrpc::decode_message(
            JsonValue::object_t{{"id", JsonValue(1.5)}, {"jsonrpc", JsonValue("2.0")}, {"result", JsonValue{}}})
                    .has_value());
    CHECK_FALSE(mcp::jsonrpc::decode_message(JsonValue::object_t{{"id", JsonValue(1.0)}, {"jsonrpc", JsonValue("2.0")}})
                    .has_value());
    CHECK_FALSE(mcp::jsonrpc::decode_message(JsonValue::object_t{
                                                     {"id", JsonValue(1.0)},
                                                     {"jsonrpc", JsonValue("2.0")},
                                                     {"result", JsonValue{}},
                                                     {"error", JsonValue::object_t{}},
                                             })
                    .has_value());
}

TEST_CASE("a response carrying neither a matching id nor a response envelope is a protocol failure",
        "[mcp][framing][issue836][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover",
            {
                    .raw_body = frame(mcp::jsonrpc::encode_result(999, tests::discover_result())),
            });
    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    auto probe = tests::drive(client.probe_era());
    REQUIRE_FALSE(probe.has_value());
    CHECK(probe.error().code == support::ErrorCode::Validation);
    CHECK(probe.error().detail.find("no JSON-RPC response") != std::string::npos);
}

TEST_CASE("a non-success HTTP status is reported as a protocol failure with the status",
        "[mcp][framing][issue836][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    transport->answer("server/discover", {.status_code = 503});
    mcp::UpstreamClient client(
            "executor", transport, mcp::UpstreamClientOptions{.url = "https://upstream.invalid/mcp"});
    auto probe = tests::drive(client.probe_era());
    REQUIRE_FALSE(probe.has_value());
    CHECK(probe.error().code == support::ErrorCode::Validation);
    CHECK(probe.error().detail.find("503") != std::string::npos);
}
