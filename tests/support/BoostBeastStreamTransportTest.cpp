#include "support/BoostBeastStreamTransport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <optional>
#include <stop_token>
#include <string>

using namespace cch;

namespace {

support::Expected<support::StreamResponse> run_stream(support::StreamRequest request) {
    boost::asio::io_context io;
    std::optional<support::Expected<support::StreamResponse>> result;

    support::BoostBeastStreamTransport transport;
    boost::asio::co_spawn(
            io,
            [&transport, request = std::move(request), &result]() mutable -> boost::asio::awaitable<void> {
                result = co_await transport.async_stream(
                        request, [](std::string_view) { return support::ExpectedVoid{}; });
                co_return;
            },
            boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

[[nodiscard]] boost::asio::awaitable<std::string> serve_one_request_and_capture_user_agent(
        boost::asio::ip::tcp::acceptor& acceptor) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;

    asio::ip::tcp::socket socket(co_await asio::this_coro::executor);
    boost::system::error_code error;
    co_await acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return std::string{};
    }

    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    co_await http::async_read(socket, buffer, request, asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        co_return std::string{};
    }
    std::string user_agent{request[http::field::user_agent]};

    http::response<http::string_body> response{http::status::ok, request.version()};
    response.set(http::field::content_type, "application/json");
    response.keep_alive(false);
    response.body() = R"({"ok":true})";
    response.prepare_payload();
    co_await http::async_write(socket, response, asio::redirect_error(asio::use_awaitable, error));
    boost::system::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    co_return user_agent;
}

} // namespace

TEST_CASE("stream transport rejects unsupported URLs before network", "[support][transport][spec]") {
    support::StreamRequest request;
    request.url = "ftp://example.com/v1/chat/completions";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == support::ErrorCode::Validation);
    CHECK(response.error().detail.find("http") != std::string::npos);
}

TEST_CASE("stream transport rejects missing host before network", "[support][transport][spec]") {
    support::StreamRequest request;
    request.url = "https:///v1/chat/completions";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == support::ErrorCode::Validation);
    CHECK(response.error().detail.find("host") != std::string::npos);
}

TEST_CASE("stream transport observes cancellation before network work", "[support][transport][abort][spec]") {
    std::stop_source stop_source;
    CHECK(stop_source.request_stop());

    support::StreamRequest request;
    request.url = "https://example.com/v1/chat/completions";
    request.stop_token = stop_source.get_token();

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == support::ErrorCode::Cancelled);
    CHECK(response.error().message == "stream transport cancelled");
}

TEST_CASE("stream transport rejects unsupported HTTP methods before network", "[support][transport][spec]") {
    support::StreamRequest request;
    request.url = "https://example.com/v1/chat/completions";
    request.method = "BREW";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == support::ErrorCode::Validation);
    CHECK(response.error().message == "unsupported HTTP method");
}

TEST_CASE("stream transport sends the Magpie Pike User-Agent", "[support][transport][magpie][spec]") {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
            io, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();

    std::optional<std::string> observed_user_agent;
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                observed_user_agent = co_await serve_one_request_and_capture_user_agent(acceptor);
                co_return;
            },
            boost::asio::detached);

    std::optional<support::Expected<support::StreamResponse>> result;
    support::BoostBeastStreamTransport transport;
    support::StreamRequest request;
    request.method = "GET";
    request.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/models";
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                result = co_await transport.async_stream(
                        request, [](std::string_view) { return support::ExpectedVoid{}; });
                co_return;
            },
            boost::asio::detached);

    io.run();
    REQUIRE(observed_user_agent.has_value());
    CHECK(*observed_user_agent == "magpie-pike/0.1");
    REQUIRE(result.has_value());
    REQUIRE(*result);
    CHECK((*result)->head.status_code == 200);
}
