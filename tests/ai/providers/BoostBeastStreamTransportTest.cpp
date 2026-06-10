#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <cch/ai/providers/BoostBeastStreamTransport.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <string>

using namespace cch;

namespace {

util::Expected<ai::providers::StreamResponse> run_stream(ai::providers::StreamRequest request) {
    boost::asio::io_context io;
    std::optional<util::Expected<ai::providers::StreamResponse>> result;

    ai::providers::BoostBeastStreamTransport transport;
    boost::asio::co_spawn(
        io,
        [&transport, request = std::move(request), &result]() mutable -> boost::asio::awaitable<void> {
            result = co_await transport.async_stream(request, [](std::string_view) { return util::ExpectedVoid{}; });
            co_return;
        },
        boost::asio::detached);

    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("stream transport rejects unsupported URLs before network", "[ai][provider][transport][u3]") {
    ai::providers::StreamRequest request;
    request.url = "http://example.com/v1/chat/completions";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == util::ErrorCode::Validation);
    CHECK(response.error().detail.find("https") != std::string::npos);
}

TEST_CASE("stream transport rejects missing host before network", "[ai][provider][transport][u3]") {
    ai::providers::StreamRequest request;
    request.url = "https:///v1/chat/completions";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == util::ErrorCode::Validation);
    CHECK(response.error().detail.find("host") != std::string::npos);
}

TEST_CASE("stream transport rejects unsupported HTTP methods before network", "[ai][provider][transport][u3]") {
    ai::providers::StreamRequest request;
    request.url = "https://example.com/v1/chat/completions";
    request.method = "BREW";

    auto response = run_stream(std::move(request));

    REQUIRE_FALSE(response);
    CHECK(response.error().code == util::ErrorCode::Validation);
    CHECK(response.error().message == "unsupported HTTP method");
}
