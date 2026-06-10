#include <cch/ai/providers/BoostBeastStreamTransport.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace cch::ai::providers {
namespace {

struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

[[nodiscard]] util::Expected<ParsedUrl> parse_https_url(const std::string& url) {
    constexpr std::string_view scheme = "https://";
    if (url.rfind(std::string(scheme), 0) != 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "unsupported URL scheme",
            "BoostBeastStreamTransport only supports https URLs"));
    }

    auto rest = url.substr(scheme.size());
    auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);

    ParsedUrl parsed;
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "missing HTTPS host",
            "https URL is missing host"));
    }

    auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }

    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "invalid HTTPS authority",
            "https URL has invalid host or port"));
    }

    return parsed;
}

[[nodiscard]] std::string_view request_method(const StreamRequest& request) {
    return request.method.empty() ? std::string_view{"POST"} : std::string_view{request.method};
}

[[nodiscard]] util::Error network_error(std::string message, boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted ? util::ErrorCode::Cancelled : util::ErrorCode::Network;
    auto detail = ec ? ec.message() : std::string{};
    return util::make_error(code, std::move(message), std::move(detail));
}

[[nodiscard]] util::Error exception_error(const std::exception& error) {
    std::string detail = error.what();
    auto code = detail.find("timeout") != std::string::npos || detail.find("timed out") != std::string::npos
        ? util::ErrorCode::Timeout
        : util::ErrorCode::Network;
    return util::make_error(code, "stream transport failure", std::move(detail));
}

} // namespace

boost::asio::awaitable<util::Expected<StreamResponse>> BoostBeastStreamTransport::async_stream(
    const StreamRequest& request,
    BodyChunkHandler on_body_chunk) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    auto parsed = parse_https_url(request.url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

    try {
        http::verb verb = http::string_to_verb(request_method(request));
        if (verb == http::verb::unknown) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "unsupported HTTP method",
                std::string(request_method(request))));
        }

        auto executor = co_await asio::this_coro::executor;

        ssl::context ctx(ssl::context::tls_client);
        boost::system::error_code ec;
        ctx.set_default_verify_paths(ec);
        if (ec) {
            co_return std::unexpected(network_error("CA loading failure", ec));
        }

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);
        beast::get_lowest_layer(stream).expires_after(request.timeout);

        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str())) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Network,
                "TLS SNI setup failed",
                "OpenSSL rejected the host name"));
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed->host));

        auto results = co_await resolver.async_resolve(parsed->host, parsed->port, asio::use_awaitable);
        co_await beast::get_lowest_layer(stream).async_connect(results, asio::use_awaitable);
        co_await stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);

        http::request<http::string_body> http_request{verb, parsed->target, 11};
        http_request.set(http::field::host, parsed->host);
        http_request.set(http::field::user_agent, "cpp-coding-harness/0.1");
        for (const auto& [key, value] : request.headers) {
            http_request.set(key, value);
        }
        http_request.body() = request.body;
        http_request.prepare_payload();

        co_await http::async_write(stream, http_request, asio::use_awaitable);

        beast::flat_buffer buffer;
        http::response_parser<http::buffer_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
        co_await http::async_read_header(stream, buffer, parser, asio::use_awaitable);

        StreamResponse response;
        response.head.status_code = static_cast<int>(parser.get().result_int());
        for (const auto& field : parser.get().base()) {
            response.head.headers[std::string(field.name_string())] = std::string(field.value());
        }

        if (response.head.status_code < 200 || response.head.status_code >= 300) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Provider,
                "provider returned non-success HTTP status",
                std::to_string(response.head.status_code)));
        }

        while (!parser.is_done()) {
            std::array<char, 8192> chunk{};
            parser.get().body().data = chunk.data();
            parser.get().body().size = chunk.size();

            boost::system::error_code read_ec;
            co_await http::async_read_some(
                stream,
                buffer,
                parser,
                asio::redirect_error(asio::use_awaitable, read_ec));

            const auto remaining = parser.get().body().size;
            const auto produced = chunk.size() - remaining;
            if (produced != 0) {
                std::string_view body_chunk(chunk.data(), produced);
                response.body.append(body_chunk.data(), body_chunk.size());
                if (on_body_chunk) {
                    auto handled = on_body_chunk(body_chunk);
                    if (!handled) {
                        co_return std::unexpected(handled.error());
                    }
                }
            }

            if (read_ec == http::error::need_buffer) {
                continue;
            }
            if (read_ec) {
                co_return std::unexpected(network_error("HTTP body read failure", read_ec));
            }
        }

        boost::system::error_code shutdown_ec;
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, shutdown_ec));
        if (shutdown_ec == asio::error::eof || shutdown_ec == ssl::error::stream_truncated) {
            shutdown_ec = {};
        }
        if (shutdown_ec) {
            co_return std::unexpected(network_error("TLS shutdown failure", shutdown_ec));
        }

        co_return response;
    } catch (const std::exception& error) {
        co_return std::unexpected(exception_error(error));
    }
}

} // namespace cch::ai::providers
