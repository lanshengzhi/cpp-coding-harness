#include "OAuthHttpClient.hpp"

#include "ai/CancellationBridge.hpp"
#include "ai/TransportExecutor.hpp"
#include "ai/providers/TransportShared.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>

#include <openssl/ssl.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::auth {
namespace {

[[nodiscard]] support::Error network_error(std::string message, boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted
        ? support::ErrorCode::Cancelled
        : support::ErrorCode::Network;
    return support::make_error(
        code,
        std::move(message),
        ec ? ec.message() : std::string{});
}

} // namespace

boost::asio::awaitable<support::Expected<OAuthHttpResponse>>
BoostBeastOAuthHttpClient::post(
    std::string url,
    std::map<std::string, std::string, std::less<>> headers,
    std::string body,
    std::stop_token stop_token) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;

    auto parsed = providers::parse_transport_url(
            url, "https://", "HTTPS", "https", "OAuth HTTP client only supports https URLs");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

        auto executor = transport_executor(co_await asio::this_coro::executor);
        if (stop_token.stop_requested()) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "OAuth HTTP request cancelled"));
        }

        CancellationSignalBridge cancellation(stop_token, executor);
        const auto cancellable = [&cancellation](auto completion_token) {
            return cancellation.bind(std::move(completion_token));
        };

        ssl::context ctx(ssl::context::tls_client);
        if (auto ca = providers::load_tls_client_ca(ctx); !ca) {
            co_return std::unexpected(ca.error());
        }

    TransportResolver resolver(executor);
    TransportTlsStream stream(executor, ctx);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{30});

    if (auto tls = providers::configure_tls_client_stream(stream, parsed->host); !tls) {
        co_return std::unexpected(tls.error());
    }

    boost::system::error_code setup_ec;
    auto results = co_await resolver.async_resolve(
        parsed->host,
        parsed->port,
        asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    co_await beast::get_lowest_layer(stream).async_connect(
        results,
        asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    co_await stream.async_handshake(
        ssl::stream_base::client,
        asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }

    http::request<http::string_body> http_request{http::verb::post, parsed->target, 11};
    http_request.set(http::field::host, parsed->host);
    http_request.set(http::field::user_agent, "cpp-coding-harness/0.1");
    for (const auto& [key, value] : headers) {
        http_request.set(key, value);
    }
    http_request.body() = std::move(body);
    http_request.prepare_payload();

    co_await http::async_write(
        stream,
        http_request,
        asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(
        stream,
        buffer,
        response,
        asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    beast::get_lowest_layer(stream).expires_never();

    boost::system::error_code shutdown_ec;
    co_await stream.async_shutdown(
        asio::redirect_error(cancellable(asio::use_awaitable), shutdown_ec));
    if (shutdown_ec == asio::error::eof ||
        shutdown_ec == ssl::error::stream_truncated) {
        shutdown_ec = {};
    }
    if (shutdown_ec) {
        co_return std::unexpected(network_error("TLS shutdown failure", shutdown_ec));
    }
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "OAuth HTTP request cancelled"));
    }

    co_return OAuthHttpResponse{
            .status_code = static_cast<int>(response.result_int()),
            .body = std::move(response.body()),
    };
}

} // namespace cch::ai::auth
