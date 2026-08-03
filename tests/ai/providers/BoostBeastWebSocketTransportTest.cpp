#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "ai/providers/BoostBeastWebSocketTransport.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <stop_token>
#include <string>

using namespace cch;

namespace {

util::Expected<std::shared_ptr<ai::providers::WebSocket>> run_connect(
    ai::providers::WebSocketConnectRequest request) {
    boost::asio::io_context io;
    std::optional<util::Expected<std::shared_ptr<ai::providers::WebSocket>>> result;

    ai::providers::BoostBeastWebSocketTransport transport;
    boost::asio::co_spawn(
        io,
        [&transport, request = std::move(request), &result]() mutable
        -> boost::asio::awaitable<void> {
            result = co_await transport.async_connect(request);
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE(
    "WebSocket transport rejects unsupported URLs before network",
    "[ai][provider][transport][issue342]") {
    ai::providers::WebSocketConnectRequest request;
    request.url = "https://chatgpt.com/backend-api/codex/responses";

    auto connection = run_connect(std::move(request));

    REQUIRE_FALSE(connection);
    CHECK(connection.error().code == util::ErrorCode::Validation);
    CHECK(connection.error().detail.find("ws") != std::string::npos);
}

TEST_CASE(
    "WebSocket transport rejects missing host before network",
    "[ai][provider][transport][issue342]") {
    ai::providers::WebSocketConnectRequest request;
    request.url = "wss:///codex/responses";

    auto connection = run_connect(std::move(request));

    REQUIRE_FALSE(connection);
    CHECK(connection.error().code == util::ErrorCode::Validation);
    CHECK(connection.error().detail.find("host") != std::string::npos);
}

TEST_CASE(
    "WebSocket transport observes cancellation before network work",
    "[ai][provider][transport][issue342]") {
    std::stop_source stop_source;
    CHECK(stop_source.request_stop());

    ai::providers::WebSocketConnectRequest request;
    request.url = "wss://chatgpt.com/backend-api/codex/responses";
    request.stop_token = stop_source.get_token();

    auto connection = run_connect(std::move(request));

    REQUIRE_FALSE(connection);
    CHECK(connection.error().code == util::ErrorCode::Cancelled);
    CHECK(connection.error().message == "WebSocket transport cancelled");
}
