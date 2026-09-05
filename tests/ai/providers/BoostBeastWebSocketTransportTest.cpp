#include "ai/providers/BoostBeastWebSocketTransport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

using namespace cch;

namespace {

support::Expected<std::shared_ptr<ai::providers::WebSocket>> run_connect(
    ai::providers::WebSocketConnectRequest request) {
    boost::asio::io_context io;
    std::optional<support::Expected<std::shared_ptr<ai::providers::WebSocket>>> result;

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

boost::asio::awaitable<void> run_delayed_websocket_server(std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    using tcp = asio::ip::tcp;

    auto executor = co_await asio::this_coro::executor;
    tcp::socket peer(executor);
    boost::system::error_code error;
    co_await acceptor->async_accept(peer, asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    beast::websocket::stream<tcp::socket> socket(std::move(peer));
    co_await socket.async_accept(asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    beast::flat_buffer request;
    co_await socket.async_read(request, asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    asio::steady_timer delay(executor, std::chrono::milliseconds{250});
    co_await delay.async_wait(asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return;
    }

    socket.text(true);
    const std::string response{"delayed response"};
    co_await socket.async_write(asio::buffer(response), asio::redirect_error(asio::use_awaitable, error));
}

support::Expected<std::optional<std::string>> run_delayed_receive(std::uint16_t port) {
    namespace asio = boost::asio;

    asio::io_context io;
    auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(io);
    boost::system::error_code error;
    acceptor->open(asio::ip::tcp::v4(), error);
    REQUIRE_FALSE(error);
    acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), error);
    REQUIRE_FALSE(error);
    acceptor->bind({asio::ip::tcp::v4(), port}, error);
    REQUIRE_FALSE(error);
    const auto endpoint = acceptor->local_endpoint(error);
    REQUIRE_FALSE(error);
    acceptor->listen(asio::socket_base::max_listen_connections, error);
    REQUIRE_FALSE(error);
    const auto listen_port = endpoint.port();

    ai::providers::BoostBeastWebSocketTransport transport;
    std::optional<support::Expected<std::optional<std::string>>> result;
    asio::co_spawn(io, run_delayed_websocket_server(acceptor), asio::detached);
    asio::co_spawn(
            io,
            [&transport, listen_port, &result]() -> asio::awaitable<void> {
                ai::providers::WebSocketConnectRequest request;
                request.url = "ws://127.0.0.1:" + std::to_string(listen_port) + "/";
                request.connect_timeout = std::chrono::milliseconds{100};
                auto connection = co_await transport.async_connect(request);
                if (!connection) {
                    result = std::unexpected(connection.error());
                    co_return;
                }
                auto sent = co_await (*connection)->async_send("ping");
                if (!sent) {
                    result = std::unexpected(sent.error());
                    (*connection)->close();
                    co_return;
                }
                result = co_await (*connection)->async_receive();
                (*connection)->close();
            },
            asio::detached);

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
    CHECK(connection.error().code == support::ErrorCode::Validation);
    CHECK(connection.error().detail.find("ws") != std::string::npos);
}

TEST_CASE(
    "WebSocket transport rejects missing host before network",
    "[ai][provider][transport][issue342]") {
    ai::providers::WebSocketConnectRequest request;
    request.url = "wss:///codex/responses";

    auto connection = run_connect(std::move(request));

    REQUIRE_FALSE(connection);
    CHECK(connection.error().code == support::ErrorCode::Validation);
    CHECK(connection.error().detail.find("host") != std::string::npos);
}

TEST_CASE("WebSocket transport does not retain the connect timeout after the handshake",
        "[ai][provider][transport][issue342]") {
    auto received = run_delayed_receive(0);

    REQUIRE(received);
    REQUIRE(received->has_value());
    CHECK(**received == "delayed response");
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
    CHECK(connection.error().code == support::ErrorCode::Cancelled);
    CHECK(connection.error().message == "WebSocket transport cancelled");
}
