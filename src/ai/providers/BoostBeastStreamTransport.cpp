#include "BoostBeastStreamTransport.hpp"

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

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cch::ai::providers {
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
            "BoostBeastStreamTransport only supports https URLs"));
    }

    auto rest = url.substr(scheme.size());
    auto slash = rest.find('/');
    auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);

    ParsedUrl parsed;
    parsed.target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "invalid HTTPS authority",
            "https URL has invalid host or port"));
    }

    return parsed;
}

[[nodiscard]] std::string_view request_method(const StreamRequest& request) {
    return request.method.empty() ? std::string_view{"POST"} : std::string_view{request.method};
}

[[nodiscard]] support::Error network_error(std::string message, boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted ? support::ErrorCode::Cancelled : support::ErrorCode::Network;
    auto detail = ec ? ec.message() : std::string{};
    return support::make_error(code, std::move(message), std::move(detail));
}

/// Maps a setup-phase failure to the same error the exception-enabled catch
/// produces: the header timer turning a failure into Timeout, and cancellation
/// into Cancelled, otherwise Network.
[[nodiscard]] support::Error stream_setup_error(
    bool response_header_timed_out,
    boost::system::error_code ec) {
    if (response_header_timed_out) {
        return support::make_error(
            support::ErrorCode::Timeout,
            "response header timeout",
            ec.message());
    }
    return network_error("stream transport failure", ec);
}

[[nodiscard]] support::Error cancelled_error() {
    return support::make_error(
        support::ErrorCode::Cancelled,
        "stream transport cancelled",
        "transport operation was cancelled");
}

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
[[nodiscard]] support::Error exception_error(const std::exception& error) {
    std::string detail = error.what();
    auto code = detail.find("timeout") != std::string::npos || detail.find("timed out") != std::string::npos
        ? support::ErrorCode::Timeout
        : support::ErrorCode::Network;
    return support::make_error(code, "stream transport failure", std::move(detail));
}
#endif

} // namespace

boost::asio::awaitable<support::Expected<StreamResponse>> BoostBeastStreamTransport::async_stream(
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

    auto response_header_timed_out = std::make_shared<bool>(false);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        http::verb verb = http::string_to_verb(request_method(request));
        if (verb == http::verb::unknown) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "unsupported HTTP method",
                std::string(request_method(request))));
        }

        auto executor = co_await asio::this_coro::executor;
        if (request.stop_token.stop_requested()) {
            co_return std::unexpected(cancelled_error());
        }

        auto cancellation_signal = std::make_shared<asio::cancellation_signal>();
        std::stop_callback cancellation{request.stop_token, [executor, cancellation_signal] {
            asio::post(executor, [cancellation_signal] {
                cancellation_signal->emit(asio::cancellation_type::all);
            });
        }};
        const auto cancellable = [&cancellation_signal](auto completion_token) {
            return asio::bind_cancellation_slot(
                cancellation_signal->slot(),
                std::move(completion_token));
        };
        asio::steady_timer response_header_timer(executor, request.timeout);
        response_header_timer.async_wait(
            [response_header_timed_out, cancellation_signal](
                boost::system::error_code error) {
                if (!error) {
                    *response_header_timed_out = true;
                    cancellation_signal->emit(asio::cancellation_type::all);
                }
            });

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
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Network,
                "TLS SNI setup failed",
                "OpenSSL rejected the host name"));
        }
        stream.set_verify_mode(ssl::verify_peer);
        stream.set_verify_callback(ssl::host_name_verification(parsed->host));

        // Setup awaits report failures through an explicit error code so the
        // strict no-exception build returns Expected instead of reaching the
        // Boost terminate hook. Timeout/cancellation mapping mirrors the
        // exception-enabled catch and the body-read path.
        boost::system::error_code setup_ec;
        auto results = co_await resolver.async_resolve(
            parsed->host,
            parsed->port,
            asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }
        co_await beast::get_lowest_layer(stream).async_connect(
            results,
            asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }
        co_await stream.async_handshake(
            ssl::stream_base::client,
            asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }

        http::request<http::string_body> http_request{verb, parsed->target, 11};
        http_request.set(http::field::host, parsed->host);
        http_request.set(http::field::user_agent, "cpp-coding-harness/0.1");
        for (const auto& [key, value] : request.headers) {
            http_request.set(key, value);
        }
        http_request.body() = request.body;
        http_request.prepare_payload();

        co_await http::async_write(
            stream,
            http_request,
            asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }

        beast::flat_buffer buffer;
        http::response_parser<http::buffer_body> parser;
        parser.body_limit(16 * 1024 * 1024); // 16 MiB
        co_await http::async_read_header(
            stream,
            buffer,
            parser,
            asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }
        // timeoutMs bounds setup through response headers. Streaming body
        // lifetime is governed by caller cancellation, matching pi's SSE path.
        response_header_timer.cancel();
        beast::get_lowest_layer(stream).expires_never();

        StreamResponse response;
        response.head.status_code = static_cast<int>(parser.get().result_int());
        for (const auto& field : parser.get().base()) {
            response.head.headers[std::string(field.name_string())] = std::string(field.value());
        }

        if (response.head.status_code < 200 || response.head.status_code >= 300) {
            std::string error_body;
            while (!parser.is_done()) {
                std::array<char, 4096> err_chunk{};
                parser.get().body().data = err_chunk.data();
                parser.get().body().size = err_chunk.size();
                boost::system::error_code read_ec;
                co_await http::async_read_some(
                    stream,
                    buffer,
                    parser,
                    asio::redirect_error(cancellable(asio::use_awaitable), read_ec));
                const auto remaining = parser.get().body().size;
                const auto produced = err_chunk.size() - remaining;
                if (produced != 0) {
                    error_body.append(err_chunk.data(), produced);
                }
                if (read_ec == http::error::need_buffer) {
                    continue;
                }
                if (read_ec == asio::error::operation_aborted ||
                    request.stop_token.stop_requested()) {
                    co_return std::unexpected(cancelled_error());
                }
                if (read_ec) {
                    break;
                }
            }
            if (request.stop_token.stop_requested()) {
                co_return std::unexpected(cancelled_error());
            }
            response.body = std::move(error_body);
            co_return response;
        }

        const bool stream_to_callback = static_cast<bool>(on_body_chunk);
        while (!parser.is_done()) {
            std::array<char, 8192> chunk{};
            parser.get().body().data = chunk.data();
            parser.get().body().size = chunk.size();

            boost::system::error_code read_ec;
            co_await http::async_read_some(
                stream,
                buffer,
                parser,
                asio::redirect_error(cancellable(asio::use_awaitable), read_ec));

            const auto remaining = parser.get().body().size;
            const auto produced = chunk.size() - remaining;
            if (produced != 0) {
                std::string_view body_chunk(chunk.data(), produced);
                if (!stream_to_callback) {
                    response.body.append(body_chunk.data(), body_chunk.size());
                }
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
        co_await stream.async_shutdown(
            asio::redirect_error(cancellable(asio::use_awaitable), shutdown_ec));
        if (shutdown_ec == asio::error::eof || shutdown_ec == ssl::error::stream_truncated) {
            shutdown_ec = {};
        }
        if (shutdown_ec) {
            co_return std::unexpected(network_error("TLS shutdown failure", shutdown_ec));
        }
        if (request.stop_token.stop_requested()) {
            co_return std::unexpected(cancelled_error());
        }

        co_return response;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const boost::system::system_error& error) {
        if (*response_header_timed_out) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "response header timeout",
                error.code().message()));
        }
        co_return std::unexpected(network_error(
            "stream transport failure",
            error.code()));
    } catch (const std::exception& error) {
        co_return std::unexpected(exception_error(error));
    }
#endif
}

} // namespace cch::ai::providers
