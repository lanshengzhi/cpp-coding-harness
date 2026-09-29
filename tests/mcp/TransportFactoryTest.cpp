// The production transport seam the MCP Host's owner uses (issue #841).
//
// `cch_mcp` keeps the Streamable HTTP transport implementation private, and
// ADR 0065 keeps Boost out of the Owner Interface, so the only way the MCP
// Host's owner can obtain a production transport is the
// `make_streamable_http_transport()` factory this file exercises. The factory
// returns a late-bound transport: it takes the serialized execution domain of
// the exchange that drives it, exactly as the `McpTransport` executor contract
// states, which is what lets the owner wire one connection per configured
// Upstream MCP Server without naming an executor type.

#include <cch/mcp/McpTransport.hpp>
#include "mcp/transport/BoostBeastStreamableHttpTransport.hpp"
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

/// One exchange, as a caller frames it. The transport's own request
/// contract — the URL, the reserved headers, and the framed body — is
/// covered by the transport cases; this file only cares where the exchange
/// runs.
[[nodiscard]] mcp::McpRequest exchange_to(std::string url) {
    return mcp::McpRequest{
            .url = std::move(url),
            .headers = {},
            .body = R"({"jsonrpc":"2.0","id":1,"method":"server/discover"})",
            .timeout = std::chrono::seconds{2},
    };
}

/// A port nothing listens on, so a bound transport fails on the wire quickly
/// instead of waiting out its deadline.
constexpr std::string_view kDeadEndpoint{"https://127.0.0.1:1/mcp"};

/// Whether the failure is the "no serialized execution domain" refusal,
/// which is the one behaviour a late-bound transport adds.
[[nodiscard]] bool is_unbound_domain_error(const support::Error& error) {
    return error.message.find("serialized execution domain") != std::string::npos;
}

} // namespace

TEST_CASE("the production transport factory answers the owners that may not name an Asio type", "[mcp][issue841]") {
    auto transport = mcp::make_streamable_http_transport();
    REQUIRE(transport != nullptr);

    boost::asio::io_context loop;
    auto outcome = std::make_shared<support::Expected<mcp::McpResponse>>(std::unexpected(support::make_error(
            support::ErrorCode::Busy, "the exchange never completed", "the fixture drives it on a real domain")));
    // The exchange is framed from inside the operation, at initiation: a
    // late-bound transport takes the domain of the exchange that drives it,
    // which is exactly where the client stack frames its requests.
    boost::asio::co_spawn(loop,
            [transport, outcome]() -> boost::asio::awaitable<void> {
                auto operation = support::detail::make_async_result(
                        [transport]() -> boost::asio::awaitable<support::Expected<mcp::McpResponse>> {
                            co_return co_await support::detail::await_async_result(
                                    transport->send(exchange_to(std::string{kDeadEndpoint})));
                        });
                *outcome = co_await support::detail::await_async_result(std::move(operation));
            },
            boost::asio::detached);
    loop.run();

    // A dead endpoint is a transport failure; a transport that never bound
    // itself would have refused the exchange before touching a socket.
    REQUIRE_FALSE(outcome->has_value());
    CHECK_FALSE(is_unbound_domain_error(outcome->error()));
}

TEST_CASE("an unbound production transport refuses an exchange outside a serialized domain", "[mcp][issue841]") {
    // Constructed without an executor and driven by a plain `start()` call:
    // there is no coroutine to inherit an executor from, so the transport
    // says so instead of guessing one.
    mcp::transport::BoostBeastStreamableHttpTransport unbound{mcp::transport::StreamableHttpTransportOptions{}};
    auto outcome = support::Expected<mcp::McpResponse>{std::unexpected(support::make_error(
            support::ErrorCode::Busy, "the exchange never completed", "the transport refuses it inline"))};
    unbound.send(exchange_to(std::string{kDeadEndpoint}))
            .start([&outcome](std::expected<mcp::McpResponse, support::Error> result) mutable noexcept {
                outcome = std::move(result);
            });
    REQUIRE_FALSE(outcome.has_value());
    CHECK(is_unbound_domain_error(outcome.error()));
}
