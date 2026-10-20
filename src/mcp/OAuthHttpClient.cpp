#include "OAuthHttpClientImpl.hpp"

#include "support/AsyncResultBridge.hpp"

#include "support/CancellationBridge.hpp"
#include "support/TransportExecutor.hpp"
#include "support/TransportShared.hpp"

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

namespace cch::mcp {
namespace {

[[nodiscard]] support::Error network_error(std::string message, boost::system::error_code ec) {
    auto code =
            ec == boost::asio::error::operation_aborted ? support::ErrorCode::Cancelled : support::ErrorCode::Network;
    return support::make_error(code, std::move(message), ec ? ec.message() : std::string{});
}

/// Perform one HTTPS request to `url` through the shared transport and return
/// its status and body. `body` is ignored for a body-less verb. The request
/// observes `stop_token` through the shared cancellation bridge, so an aborted
/// login cancels the in-flight request and reports `Cancelled`.
[[nodiscard]] boost::asio::awaitable<support::Expected<OAuthHttpResponse>> perform_https_request(
        boost::beast::http::verb verb,
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;

    auto parsed = support::parse_transport_url(url, "OAuth HTTP client only supports https URLs");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

    auto executor = support::transport_executor(co_await asio::this_coro::executor);
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "OAuth HTTP request cancelled"));
    }

    support::CancellationSignalBridge cancellation(stop_token, executor);
    const auto cancellable = [&cancellation](
                                     auto completion_token) { return cancellation.bind(std::move(completion_token)); };

    ssl::context ctx(ssl::context::tls_client);
    if (auto ca = support::load_tls_client_ca(ctx); !ca) {
        co_return std::unexpected(ca.error());
    }

    support::TransportResolver resolver(executor);
    support::TransportTlsStream stream(executor, ctx);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{30});

    if (auto tls = support::configure_tls_client_stream(stream, parsed->host); !tls) {
        co_return std::unexpected(tls.error());
    }

    boost::system::error_code setup_ec;
    auto results = co_await resolver.async_resolve(
            parsed->host, parsed->port, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    co_await beast::get_lowest_layer(stream).async_connect(
            results, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    co_await stream.async_handshake(
            ssl::stream_base::client, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }

    http::request<http::string_body> http_request{verb, parsed->target, 11};
    http_request.set(http::field::host, parsed->host);
    http_request.set(http::field::user_agent, "cpp-coding-harness/0.1");
    for (const auto& [key, value] : headers) {
        http_request.set(key, value);
    }
    if (verb != http::verb::get && verb != http::verb::head) {
        http_request.body() = std::move(body);
    }
    http_request.prepare_payload();

    co_await http::async_write(stream, http_request, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    co_await http::async_read(
            stream, buffer, response, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(network_error("OAuth HTTP request failure", setup_ec));
    }
    beast::get_lowest_layer(stream).expires_never();

    boost::system::error_code shutdown_ec;
    co_await stream.async_shutdown(asio::redirect_error(cancellable(asio::use_awaitable), shutdown_ec));
    if (shutdown_ec == asio::error::eof || shutdown_ec == ssl::error::stream_truncated) {
        shutdown_ec = {};
    }
    if (shutdown_ec) {
        co_return std::unexpected(network_error("TLS shutdown failure", shutdown_ec));
    }
    if (stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "OAuth HTTP request cancelled"));
    }

    co_return OAuthHttpResponse{
            .status_code = static_cast<int>(response.result_int()),
            .body = std::move(response.body()),
    };
}

} // namespace

support::AsyncResult<OAuthHttpResponse> BoostBeastOAuthHttpClient::post(std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) {
    return support::detail::make_async_result(
            [url = std::move(url), headers = std::move(headers), body = std::move(body), stop_token]() mutable
                    -> boost::asio::awaitable<support::Expected<OAuthHttpResponse>> {
                co_return co_await perform_https_request(
                        boost::beast::http::verb::post, std::move(url), std::move(headers), std::move(body), stop_token);
            });
}

support::AsyncResult<OAuthHttpResponse> BoostBeastOAuthHttpClient::get(
        std::string url, std::map<std::string, std::string, std::less<>> headers, std::stop_token stop_token) {
    return support::detail::make_async_result(
            [url = std::move(url), headers = std::move(headers), stop_token]() mutable
                    -> boost::asio::awaitable<support::Expected<OAuthHttpResponse>> {
                co_return co_await perform_https_request(
                        boost::beast::http::verb::get, std::move(url), std::move(headers), std::string{}, stop_token);
            });
}

std::shared_ptr<OAuthHttpClient> default_oauth_http_client() {
    return std::make_shared<BoostBeastOAuthHttpClient>();
}

} // namespace cch::mcp
