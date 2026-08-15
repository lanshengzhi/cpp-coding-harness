#include "OAuthHttpClient.hpp"

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

struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

[[nodiscard]] support::Expected<ParsedUrl> parse_https_url(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "unsupported URL scheme",
            "OAuth HTTP client only supports https URLs"));
    }
    auto rest = url.substr(scheme.size());
    auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    ParsedUrl parsed;
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "invalid HTTPS authority",
            "https URL has invalid host or port"));
    }
    return parsed;
}

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
    using tcp = boost::asio::ip::tcp;

    auto parsed = parse_https_url(url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

    try {
        auto executor = co_await asio::this_coro::executor;
        if (stop_token.stop_requested()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "OAuth HTTP request cancelled"));
        }

        auto cancellation_signal = std::make_shared<asio::cancellation_signal>();
        std::stop_callback cancellation{stop_token, [executor, cancellation_signal] {
            asio::post(executor, [cancellation_signal] {
                cancellation_signal->emit(asio::cancellation_type::all);
            });
        }};
        const auto cancellable = [&cancellation_signal](auto completion_token) {
            return asio::bind_cancellation_slot(
                cancellation_signal->slot(),
                std::move(completion_token));
        };

        ssl::context ctx(ssl::context::tls_client);
        boost::system::error_code ec;
        ctx.set_default_verify_paths(ec);
        if (ec) {
            co_return std::unexpected(network_error("CA loading failure", ec));
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{30});

        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str())) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Network,
                "TLS SNI setup failed",
                "OpenSSL rejected the host name"));
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed->host));

        auto results = co_await resolver.async_resolve(
            parsed->host,
            parsed->port,
            cancellable(asio::use_awaitable));
        co_await beast::get_lowest_layer(stream).async_connect(
            results,
            cancellable(asio::use_awaitable));
        co_await stream.async_handshake(
            ssl::stream_base::client,
            cancellable(asio::use_awaitable));

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
            cancellable(asio::use_awaitable));

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        co_await http::async_read(
            stream,
            buffer,
            response,
            cancellable(asio::use_awaitable));
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
    } catch (const boost::system::system_error& error) {
        co_return std::unexpected(network_error(
            "OAuth HTTP request failure",
            error.code()));
    } catch (const std::exception& error) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Network,
            "OAuth HTTP request failure",
            error.what()));
    }
}

} // namespace cch::ai::auth
