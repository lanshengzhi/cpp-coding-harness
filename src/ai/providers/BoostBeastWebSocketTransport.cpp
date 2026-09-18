#include "BoostBeastWebSocketTransport.hpp"

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
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::ai::providers {
namespace {

[[nodiscard]] support::Error cancelled_error() {
    return support::make_error(
        support::ErrorCode::Cancelled,
        "WebSocket transport cancelled",
        "WebSocket operation was cancelled");
}

[[nodiscard]] support::Error transport_error(
    std::string message,
    boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted ? support::ErrorCode::Cancelled
                : ec == boost::beast::error::timeout        ? support::ErrorCode::Timeout
                                                            : support::ErrorCode::Network;
    auto detail = ec ? ec.message() : std::string{};
    return support::make_error(code, std::move(message), std::move(detail));
}

template <typename Socket>
class BeastWebSocketConnection final : public WebSocket {
public:
    BeastWebSocketConnection(
            Socket socket, std::stop_token stop_token, std::optional<std::chrono::milliseconds> idle_timeout)
        : socket_(std::move(socket)), stop_token_(std::move(stop_token)), idle_timeout_(idle_timeout) {}

    ~BeastWebSocketConnection() override {
        close();
    }

    BeastWebSocketConnection(BeastWebSocketConnection&&) = delete;
    BeastWebSocketConnection& operator=(BeastWebSocketConnection&&) = delete;
    BeastWebSocketConnection(const BeastWebSocketConnection&) = delete;
    BeastWebSocketConnection& operator=(const BeastWebSocketConnection&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> async_send(
        std::string_view text) override {
        namespace asio = boost::asio;
        if (stop_token_.stop_requested()) {
            co_return std::unexpected(cancelled_error());
        }
        if (closed_ || closing_) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Network,
                "WebSocket is closed"));
        }
        auto executor = co_await asio::this_coro::executor;
        CancellationSignalBridge cancellation(stop_token_, executor);
        boost::system::error_code write_ec;
        co_await socket_.async_write(
                asio::buffer(text), asio::redirect_error(cancellation.bind(asio::use_awaitable), write_ec));
        if (write_ec) {
            co_return std::unexpected(transport_error(
                "WebSocket send failure", write_ec));
        }
        co_return support::ExpectedVoid{};
    }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<std::string>>> async_receive() override {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        if (stop_token_.stop_requested()) {
            co_return std::unexpected(cancelled_error());
        }
        if (closed_ || closing_) {
            co_return std::optional<std::string>{};
        }
        auto executor = co_await asio::this_coro::executor;
        CancellationSignalBridge cancellation(stop_token_, executor);
        bool idle_timed_out = false;
        std::optional<TransportTimer> idle_timer;
        if (idle_timeout_ && *idle_timeout_ > std::chrono::milliseconds{0}) {
            idle_timer.emplace(transport_executor(executor), *idle_timeout_);
            // The handler captures the coroutine-frame locals `this` and
            // `idle_timed_out` by reference across the read suspension.
            // The frame outlives the handler: the read completion cancels
            // the timer before the coroutine can return. The connection's
            // executor contract is single-threaded, so handler invocation
            // cannot interleave with frame destruction.
            idle_timer->async_wait([&idle_timed_out, this](
                                       boost::system::error_code error) {
                if (!error) {
                    idle_timed_out = true;
                    boost::beast::get_lowest_layer(socket_).cancel();
                }
            });
        }

        boost::system::error_code read_ec;
        co_await socket_.async_read(buffer_, asio::redirect_error(cancellation.bind(asio::use_awaitable), read_ec));
        if (idle_timer) {
            idle_timer->cancel();
        }
        if (idle_timed_out) {
            closed_ = true;
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "WebSocket idle timeout after " +
                    std::to_string(idle_timeout_->count()) + "ms"));
        }
        if (read_ec == beast::websocket::error::closed) {
            closed_ = true;
            co_return std::optional<std::string>{};
        }
        if (read_ec == asio::error::operation_aborted) {
            co_return std::unexpected(cancelled_error());
        }
        if (read_ec) {
            co_return std::unexpected(transport_error(
                "WebSocket receive failure", read_ec));
        }
        auto text = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        co_return std::optional<std::string>{std::move(text)};
    }

    void close() override {
        closing_ = true;
        boost::system::error_code ec;
        boost::beast::get_lowest_layer(socket_).socket().close(ec);
        closed_ = true;
    }

private:
    Socket socket_;
    std::stop_token stop_token_;
    std::optional<std::chrono::milliseconds> idle_timeout_;
    boost::beast::flat_buffer buffer_;
    bool closed_{false};
    bool closing_{false};
};

template <typename Socket>
[[nodiscard]] std::shared_ptr<WebSocket> make_connection(
    Socket socket,
    std::stop_token stop_token,
    std::optional<std::chrono::milliseconds> idle_timeout) {
    return std::make_shared<BeastWebSocketConnection<Socket>>(
        std::move(socket), std::move(stop_token), idle_timeout);
}

template <typename Socket>
[[nodiscard]] boost::asio::awaitable<boost::system::error_code> perform_websocket_handshake(Socket& socket,
        const ParsedUrl& parsed,
        const std::map<std::string, std::string, std::less<>>& headers,
        std::shared_ptr<boost::asio::cancellation_signal> signal) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    const auto cancellable = [&signal](auto completion_token) {
        return asio::bind_cancellation_slot(
            signal->slot(), std::move(completion_token));
    };
    socket.set_option(beast::websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    socket.set_option(beast::websocket::stream_base::decorator(
        [parsed, headers](beast::websocket::request_type& upgrade) {
            upgrade.set(beast::http::field::host, parsed.host);
            for (const auto& [key, value] : headers) {
                upgrade.set(key, value);
            }
        }));
    boost::system::error_code handshake_ec;
    co_await socket.async_handshake(
        parsed.host,
        parsed.target,
        asio::redirect_error(cancellable(asio::use_awaitable), handshake_ec));
    co_return handshake_ec;
}

} // namespace

boost::asio::awaitable<support::Expected<std::shared_ptr<WebSocket>>>
BoostBeastWebSocketTransport::async_connect(
    const WebSocketConnectRequest& request) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace ssl = boost::asio::ssl;

    // Client transports are TLS-only (ADR 0054).
    auto parsed = parse_transport_url(
            request.url, "wss://", "WebSocket", "WebSocket", "BoostBeastWebSocketTransport only supports wss URLs");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(cancelled_error());
    }

    const auto executor = transport_executor(co_await asio::this_coro::executor);
    CancellationSignalBridge cancellation(request.stop_token, executor);
    const auto cancellable = [&cancellation](
                                     auto completion_token) { return cancellation.bind(std::move(completion_token)); };

    TransportTimer connect_timer(executor, request.connect_timeout);
    bool connect_timed_out = false;
    connect_timer.async_wait([&connect_timed_out, signal = cancellation.signal_ptr()](boost::system::error_code error) {
        if (!error) {
            connect_timed_out = true;
            signal->emit(asio::cancellation_type::all);
        }
    });

    const auto setup_failure = [&](boost::system::error_code ec) -> support::Error {
        if (connect_timed_out || ec == boost::beast::error::timeout) {
            return support::make_error(
                support::ErrorCode::Timeout,
                "WebSocket connect timeout after " +
                    std::to_string(request.connect_timeout.count()) + "ms");
        }
        if (request.stop_token.stop_requested() ||
            ec == asio::error::operation_aborted) {
            return cancelled_error();
        }
        return support::make_error(
            support::ErrorCode::Network,
            "WebSocket connect failure",
            ec.message());
    };

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    // The staged build still permits setup exceptions (e.g. the throwing
    // `set_verify_mode` / `set_option` surface); convert them before the
    // no-exception completion contract takes over.
    try {
#endif
        TransportResolver resolver(executor);
        {
            ssl::context ctx(ssl::context::tls_client);
            // The optional extra CA is the test-only trust injection
            // (WebSocketConnectRequest contract); production never sets it.
            if (auto ca = load_tls_client_ca(ctx, request.trusted_ca_certificate_pem); !ca) {
                co_return std::unexpected(ca.error());
            }
            TransportTlsStream stream(executor, ctx);
            if (auto tls = configure_tls_client_stream(stream, parsed->host); !tls) {
                co_return std::unexpected(tls.error());
            }

            boost::system::error_code setup_ec;
            auto results = co_await resolver.async_resolve(
                    parsed->host, parsed->port, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
            if (setup_ec) {
                co_return std::unexpected(setup_failure(setup_ec));
            }
            beast::get_lowest_layer(stream).expires_after(request.connect_timeout);
            co_await beast::get_lowest_layer(stream).async_connect(
                    results, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
            if (setup_ec) {
                co_return std::unexpected(setup_failure(setup_ec));
            }
            // The connect timer above covers the remaining TLS and WebSocket
            // handshake. The TCP stream timeout must not survive into the
            // established WebSocket, whose receive idle timeout is managed by
            // BeastWebSocketConnection.
            beast::get_lowest_layer(stream).expires_never();
            co_await stream.async_handshake(
                    ssl::stream_base::client, asio::redirect_error(cancellable(asio::use_awaitable), setup_ec));
            if (setup_ec) {
                co_return std::unexpected(setup_failure(setup_ec));
            }

            beast::websocket::stream<TransportTlsStream> socket(std::move(stream));
            setup_ec =
                    co_await perform_websocket_handshake(socket, *parsed, request.headers, cancellation.signal_ptr());
            if (setup_ec) {
                co_return std::unexpected(setup_failure(setup_ec));
            }
            connect_timer.cancel();
            co_return make_connection(std::move(socket), request.stop_token, request.idle_timeout);
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const boost::system::system_error& error) {
        co_return std::unexpected(setup_failure(error.code()));
    } catch (const std::exception& error) {
        if (connect_timed_out) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "WebSocket connect timeout after " +
                    std::to_string(request.connect_timeout.count()) + "ms"));
        }
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Network,
            "WebSocket connect failure",
            error.what()));
    }
#endif
}

} // namespace cch::ai::providers
