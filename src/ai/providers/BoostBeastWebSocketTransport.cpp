#include "BoostBeastWebSocketTransport.hpp"

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
#include <boost/system/system_error.hpp>

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

struct ParsedWebSocketUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
    bool tls{true};
};

[[nodiscard]] support::Expected<ParsedWebSocketUrl> parse_websocket_url(
    const std::string& url) {
    std::string_view rest = url;
    bool tls = true;
    if (url.starts_with("wss://")) {
        rest.remove_prefix(6);
    } else if (url.starts_with("ws://")) {
        tls = false;
        rest.remove_prefix(5);
    } else {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "unsupported URL scheme",
            "BoostBeastWebSocketTransport only supports ws and wss URLs"));
    }

    auto slash = rest.find('/');
    auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);

    ParsedWebSocketUrl parsed;
    parsed.tls = tls;
    parsed.port = tls ? "443" : "80";
    parsed.target = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};
    if (authority.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "missing WebSocket host",
            "WebSocket URL is missing host"));
    }

    auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = std::string{authority.substr(0, colon)};
        parsed.port = std::string{authority.substr(colon + 1)};
    } else {
        parsed.host = std::string{authority};
    }

    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "invalid WebSocket authority",
            "WebSocket URL has invalid host or port"));
    }
    return parsed;
}

[[nodiscard]] support::Error cancelled_error() {
    return support::make_error(
        support::ErrorCode::Cancelled,
        "WebSocket transport cancelled",
        "WebSocket operation was cancelled");
}

[[nodiscard]] support::Error transport_error(
    std::string message,
    boost::system::error_code ec) {
    auto code = ec == boost::asio::error::operation_aborted
        ? support::ErrorCode::Cancelled
        : support::ErrorCode::Network;
    auto detail = ec ? ec.message() : std::string{};
    return support::make_error(code, std::move(message), std::move(detail));
}

[[nodiscard]] support::Error exception_error(const std::exception& error) {
    std::string detail = error.what();
    auto code = detail.find("timeout") != std::string::npos ||
                detail.find("timed out") != std::string::npos
        ? support::ErrorCode::Timeout
        : support::ErrorCode::Network;
    return support::make_error(code, "WebSocket transport failure", std::move(detail));
}

template <typename Socket>
class BeastWebSocketConnection final : public WebSocket {
public:
    BeastWebSocketConnection(
        Socket socket,
        std::stop_token stop_token,
        std::optional<std::chrono::milliseconds> idle_timeout)
        : socket_(std::move(socket)),
          stop_token_(std::move(stop_token)),
          idle_timeout_(idle_timeout),
          signal_(std::make_shared<boost::asio::cancellation_signal>()) {}

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
        std::stop_callback cancellation{stop_token_, [executor, signal = signal_] {
            asio::post(executor, [signal] {
                signal->emit(asio::cancellation_type::all);
            });
        }};
        try {
            co_await socket_.async_write(
                asio::buffer(text),
                cancellable(asio::use_awaitable));
        } catch (const boost::system::system_error& error) {
            co_return std::unexpected(transport_error(
                "WebSocket send failure", error.code()));
        } catch (const std::exception& error) {
            co_return std::unexpected(exception_error(error));
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
        try {
            auto executor = co_await asio::this_coro::executor;
            std::stop_callback cancellation{stop_token_, [executor, signal = signal_] {
                asio::post(executor, [signal] {
                    signal->emit(asio::cancellation_type::all);
                });
            }};
            bool idle_timed_out = false;
            std::optional<asio::steady_timer> idle_timer;
            if (idle_timeout_ && *idle_timeout_ > std::chrono::milliseconds{0}) {
                idle_timer.emplace(executor, *idle_timeout_);
                // The handler captures the coroutine-frame locals `this` and
                // `idle_timed_out` by reference across the read suspension.
                // The frame outlives the handler: the read completion cancels
                // the timer before the coroutine can return, and an exception
                // exit destroys the timer with the frame (Asio never invokes
                // a destroyed timer's handler). The connection's executor
                // contract is single-threaded, so handler invocation cannot
                // interleave with frame destruction.
                idle_timer->async_wait([&idle_timed_out, this](
                                           boost::system::error_code error) {
                    if (!error) {
                        idle_timed_out = true;
                        boost::beast::get_lowest_layer(socket_).cancel();
                    }
                });
            }

            boost::system::error_code read_ec;
            co_await socket_.async_read(
                buffer_,
                asio::redirect_error(
                    cancellable(asio::use_awaitable), read_ec));
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
        } catch (const boost::system::system_error& error) {
            if (stop_token_.stop_requested() ||
                error.code() == asio::error::operation_aborted) {
                co_return std::unexpected(cancelled_error());
            }
            co_return std::unexpected(transport_error(
                "WebSocket receive failure", error.code()));
        } catch (const std::exception& error) {
            co_return std::unexpected(exception_error(error));
        }
    }

    void close() override {
        closing_ = true;
        try {
            boost::system::error_code ec;
            boost::beast::get_lowest_layer(socket_).socket().close(ec);
        } catch (...) {
        }
        closed_ = true;
    }

private:
    [[nodiscard]] auto cancellable(auto completion_token) {
        return boost::asio::bind_cancellation_slot(
            signal_->slot(), std::move(completion_token));
    }

    Socket socket_;
    std::stop_token stop_token_;
    std::optional<std::chrono::milliseconds> idle_timeout_;
    std::shared_ptr<boost::asio::cancellation_signal> signal_;
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
[[nodiscard]] boost::asio::awaitable<void> perform_websocket_handshake(
    Socket& socket,
    const ParsedWebSocketUrl& parsed,
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
    co_await socket.async_handshake(
        parsed.host, parsed.target, cancellable(asio::use_awaitable));
}

} // namespace

boost::asio::awaitable<support::Expected<std::shared_ptr<WebSocket>>>
BoostBeastWebSocketTransport::async_connect(
    const WebSocketConnectRequest& request) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    auto parsed = parse_websocket_url(request.url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(cancelled_error());
    }

    auto executor = co_await asio::this_coro::executor;
    auto signal = std::make_shared<asio::cancellation_signal>();
    std::stop_callback cancellation{request.stop_token, [executor, signal] {
        asio::post(executor, [signal] {
            signal->emit(asio::cancellation_type::all);
        });
    }};
    const auto cancellable = [&signal](auto completion_token) {
        return asio::bind_cancellation_slot(
            signal->slot(), std::move(completion_token));
    };

    asio::steady_timer connect_timer(executor, request.connect_timeout);
    bool connect_timed_out = false;
    connect_timer.async_wait([&connect_timed_out, &signal](
                                 boost::system::error_code error) {
        if (!error) {
            connect_timed_out = true;
            signal->emit(asio::cancellation_type::all);
        }
    });

    try {
        tcp::resolver resolver(executor);
        if (parsed->tls) {
            ssl::context ctx(ssl::context::tls_client);
            boost::system::error_code ec;
            ctx.set_default_verify_paths(ec);
            if (ec) {
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Network,
                    "CA loading failure", ec.message()));
            }
            beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);
            beast::get_lowest_layer(stream).expires_after(request.connect_timeout);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str())) {
                co_return std::unexpected(support::make_error(
                    support::ErrorCode::Network,
                    "TLS SNI setup failed",
                    "OpenSSL rejected the host name"));
            }
            stream.set_verify_mode(ssl::verify_peer);
            stream.set_verify_callback(ssl::host_name_verification(parsed->host));

            auto results = co_await resolver.async_resolve(
                parsed->host, parsed->port, cancellable(asio::use_awaitable));
            co_await beast::get_lowest_layer(stream).async_connect(
                results, cancellable(asio::use_awaitable));
            co_await stream.async_handshake(
                ssl::stream_base::client, cancellable(asio::use_awaitable));

            beast::websocket::stream<
                beast::ssl_stream<beast::tcp_stream>>
                socket(std::move(stream));
            co_await perform_websocket_handshake(
                socket, *parsed, request.headers, signal);
            connect_timer.cancel();
            co_return make_connection(
                std::move(socket), request.stop_token, request.idle_timeout);
        }

        beast::tcp_stream stream(executor);
        stream.expires_after(request.connect_timeout);
        auto results = co_await resolver.async_resolve(
            parsed->host, parsed->port, cancellable(asio::use_awaitable));
        co_await stream.async_connect(results, cancellable(asio::use_awaitable));

        beast::websocket::stream<beast::tcp_stream> socket(std::move(stream));
        co_await perform_websocket_handshake(
            socket, *parsed, request.headers, signal);
        connect_timer.cancel();
        co_return make_connection(
            std::move(socket), request.stop_token, request.idle_timeout);
    } catch (const boost::system::system_error& error) {
        if (connect_timed_out) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "WebSocket connect timeout after " +
                    std::to_string(request.connect_timeout.count()) + "ms"));
        }
        if (request.stop_token.stop_requested() ||
            error.code() == asio::error::operation_aborted) {
            co_return std::unexpected(cancelled_error());
        }
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Network,
            "WebSocket connect failure",
            error.code().message()));
    } catch (const std::exception& error) {
        if (connect_timed_out) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Timeout,
                "WebSocket connect timeout after " +
                    std::to_string(request.connect_timeout.count()) + "ms"));
        }
        co_return std::unexpected(exception_error(error));
    }
}

} // namespace cch::ai::providers
