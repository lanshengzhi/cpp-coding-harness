#include "support/BoostBeastStreamTransport.hpp"

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

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cch::support {
namespace {

[[nodiscard]] std::string_view request_method(const StreamRequest& request) {
    return request.method.empty() ? std::string_view{"POST"} : std::string_view{request.method};
}

[[nodiscard]] Error network_error(std::string message, boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted ? ErrorCode::Cancelled : ErrorCode::Network;
    auto detail = ec ? ec.message() : std::string{};
    return make_error(code, std::move(message), std::move(detail));
}

/// Maps a setup-phase failure to the same error the exception-enabled catch
/// produces: the header timer turning a failure into Timeout, and cancellation
/// into Cancelled, otherwise Network.
[[nodiscard]] Error stream_setup_error(bool response_header_timed_out, boost::system::error_code ec) {
    if (response_header_timed_out) {
        return make_error(ErrorCode::Timeout, "response header timeout", ec.message());
    }
    return network_error("stream transport failure", ec);
}

[[nodiscard]] Error cancelled_error() {
    return make_error(ErrorCode::Cancelled, "stream transport cancelled", "transport operation was cancelled");
}

template <typename Stream, typename CancellableFn>
boost::asio::awaitable<Expected<StreamResponse>> perform_http_exchange(Stream& stream,
        const StreamRequest& request,
        const ParsedUrl& parsed,
        boost::beast::http::verb verb,
        const std::shared_ptr<bool>& response_header_timed_out,
        const CancellableFn& cancellable,
        TransportTimer& response_header_timer,
        BodyChunkHandler on_body_chunk) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;

    boost::system::error_code setup_ec;
    http::request<http::string_body> http_request{verb, parsed.target, 11};
    http_request.set(http::field::host, parsed.host);
    http_request.set(http::field::user_agent, "magpie-pike/0.1");
    for (const auto& [key, value] : request.headers) {
        http_request.set(key, value);
    }
    http_request.body() = request.body;
    http_request.prepare_payload();

    co_await http::async_write(stream, http_request, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
    }

    beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(16 * 1024 * 1024); // 16 MiB
    co_await http::async_read_header(
            stream, buffer, parser, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
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
                    stream, buffer, parser, asio::redirect_error(cancellable(asio::use_awaitable), read_ec));
            const auto remaining = parser.get().body().size;
            const auto produced = err_chunk.size() - remaining;
            if (produced != 0) {
                error_body.append(err_chunk.data(), produced);
            }
            if (read_ec == http::error::need_buffer) {
                continue;
            }
            if (read_ec == asio::error::operation_aborted || request.stop_token.stop_requested()) {
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
                stream, buffer, parser, asio::redirect_error(cancellable(asio::use_awaitable), read_ec));

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

    if constexpr (requires {
                      stream.async_shutdown(asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
                  }) {
        boost::system::error_code shutdown_ec;
        co_await stream.async_shutdown(asio::redirect_error(cancellable(asio::use_awaitable), shutdown_ec));
        if (shutdown_ec == asio::error::eof || shutdown_ec == ssl::error::stream_truncated) {
            shutdown_ec = {};
        }
        if (shutdown_ec) {
            co_return std::unexpected(network_error("TLS shutdown failure", shutdown_ec));
        }
    } else {
        boost::system::error_code shutdown_ec;
        beast::get_lowest_layer(stream).socket().shutdown(asio::ip::tcp::socket::shutdown_both, shutdown_ec);
    }
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(cancelled_error());
    }

    co_return response;
}

} // namespace

boost::asio::awaitable<Expected<StreamResponse>> BoostBeastStreamTransport::async_stream(
        const StreamRequest& request, BodyChunkHandler on_body_chunk) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = boost::beast::http;
    namespace ssl = boost::asio::ssl;

    auto parsed = parse_transport_url(request.url, "BoostBeastStreamTransport only supports http and https URLs");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }

    auto response_header_timed_out = std::make_shared<bool>(false);
    http::verb verb = http::string_to_verb(request_method(request));
    if (verb == http::verb::unknown) {
        co_return std::unexpected(
                make_error(ErrorCode::Validation, "unsupported HTTP method", std::string(request_method(request))));
    }

    const auto executor = transport_executor(co_await asio::this_coro::executor);
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(cancelled_error());
    }

    CancellationSignalBridge cancellation(request.stop_token, executor);
    const auto cancellable = [&cancellation](
                                     auto completion_token) { return cancellation.bind(std::move(completion_token)); };
    TransportTimer response_header_timer(executor, request.timeout);
    response_header_timer.async_wait([response_header_timed_out, cancellation_signal = cancellation.signal_ptr()](
                                             boost::system::error_code error) {
        if (!error) {
            *response_header_timed_out = true;
            cancellation_signal->emit(asio::cancellation_type::all);
        }
    });

    TransportResolver resolver(executor);
    boost::system::error_code setup_ec;
    auto results = co_await resolver.async_resolve(
            parsed->host, parsed->port, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
    }

    if (parsed->is_tls) {
        ssl::context ctx(ssl::context::tls_client);
        if (auto ca = load_tls_client_ca(ctx); !ca) {
            co_return std::unexpected(ca.error());
        }

        TransportTlsStream stream(executor, ctx);
        beast::get_lowest_layer(stream).expires_after(request.timeout);

        if (auto tls = configure_tls_client_stream(stream, parsed->host); !tls) {
            co_return std::unexpected(tls.error());
        }

        co_await beast::get_lowest_layer(stream).async_connect(
                results, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }
        co_await stream.async_handshake(
                ssl::stream_base::client, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
        if (setup_ec) {
            co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
        }

        co_return co_await perform_http_exchange(stream,
                request,
                *parsed,
                verb,
                response_header_timed_out,
                cancellable,
                response_header_timer,
                std::move(on_body_chunk));
    }

    beast::tcp_stream stream(executor);
    stream.expires_after(request.timeout);

    co_await stream.async_connect(results, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
    if (setup_ec) {
        co_return std::unexpected(stream_setup_error(*response_header_timed_out, setup_ec));
    }

    co_return co_await perform_http_exchange(stream,
            request,
            *parsed,
            verb,
            response_header_timed_out,
            cancellable,
            response_header_timer,
            std::move(on_body_chunk));
}

} // namespace cch::support
