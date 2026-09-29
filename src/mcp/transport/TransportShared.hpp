#pragma once

#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/execution.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::transport {

/// Concrete executor spelling for the Streamable HTTP transport (ADR 0054).
/// Keying Beast stream machinery on the concrete `io_context::executor_type` —
/// the executor the Runtime actually runs — rather than the
/// `beast::tcp_stream` default keeps the type-erased executor out of the
/// distribution binary, exactly as it is kept out of the `cch_ai` client
/// transports.
using TransportExecutor = boost::asio::io_context::executor_type;
using TransportTlsStream =
        boost::beast::ssl_stream<boost::beast::basic_stream<boost::asio::ip::tcp, TransportExecutor>>;
using TransportResolver = boost::asio::ip::basic_resolver<boost::asio::ip::tcp, TransportExecutor>;
using TransportTimer = boost::asio::basic_waitable_timer<std::chrono::steady_clock,
        boost::asio::wait_traits<std::chrono::steady_clock>,
        TransportExecutor>;

/// The concrete executor behind a transport coroutine's ambient executor. The
/// transport contract requires a single-threaded `io_context` executor, which
/// is the execution domain the connection runs on; the cast asserts that
/// contract and never converts.
[[nodiscard]] inline TransportExecutor transport_executor(const boost::asio::any_io_executor& ambient) {
    auto& context = boost::asio::query(ambient, boost::asio::execution::context);
    return static_cast<boost::asio::io_context&>(context).get_executor();
}

/// One parsed endpoint URL: authority split into host and port, path as the
/// HTTP target.
struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

/// Parse an absolute endpoint URL. Only `https://` is accepted: client
/// transports are TLS-only (ADR 0054), so a plain-HTTP endpoint is refused at
/// the transport boundary rather than served in the clear.
[[nodiscard]] inline cch::support::Expected<ParsedUrl> parse_mcp_url(const std::string& url) {
    constexpr std::string_view kScheme{"https://"};
    if (!url.starts_with(kScheme)) {
        return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Validation,
                "unsupported URL scheme",
                "the MCP Streamable HTTP transport only supports https URLs"));
    }
    const auto rest = std::string_view{url}.substr(kScheme.size());
    const auto slash = rest.find('/');
    const auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);

    ParsedUrl parsed;
    parsed.target = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};
    if (authority.empty()) {
        return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Validation,
                "missing HTTPS host",
                "the Upstream MCP Server URL is missing host"));
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Validation,
                "invalid HTTPS authority",
                "the Upstream MCP Server URL has invalid host or port"));
    }
    return parsed;
}

/// Load the platform verify paths, plus an optional extra test trust anchor,
/// into one TLS client context. The extra anchor exists so a test can run a
/// local `https://` fixture under a committed test CA; production leaves it
/// empty and verifies against the platform store alone.
[[nodiscard]] inline cch::support::ExpectedVoid load_tls_client_ca(
        boost::asio::ssl::context& context, const std::optional<std::string>& trusted_ca_certificate_pem) {
    boost::system::error_code error;
    context.set_default_verify_paths(error);
    if (error) {
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Network, "CA loading failure", error.message()));
    }
    if (trusted_ca_certificate_pem) {
        if (context.add_certificate_authority(boost::asio::buffer(*trusted_ca_certificate_pem), error); error) {
            return std::unexpected(cch::support::make_error(
                    cch::support::ErrorCode::Network, "trust anchor loading failure", error.message()));
        }
    }
    return {};
}

/// SNI plus peer verification for one TLS client stream. Verification is never
/// relaxed: a fixture that is not trusted fails the handshake, which is the
/// certificate-failure case the transport must surface rather than bypass.
[[nodiscard]] inline cch::support::ExpectedVoid configure_tls_client_stream(
        TransportTlsStream& stream, const std::string& host) {
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Network, "TLS SNI setup failed", "OpenSSL rejected the host name"));
    }
    stream.set_verify_mode(boost::asio::ssl::verify_peer);
    stream.set_verify_callback(boost::asio::ssl::host_name_verification(host));
    return {};
}

/// Bridges one `McpRequest` stop token to one Asio `cancellation_signal`: when
/// the token stops, an `emit(all)` is posted to the owning executor. The stop
/// callback captures only shared state and posts, never touching
/// coroutine-frame locals, so a cross-thread `request_stop` cannot race frame
/// destruction.
class CancellationBridge {
public:
    CancellationBridge(std::stop_token stop_token, boost::asio::any_io_executor executor)
        : signal_(std::make_shared<boost::asio::cancellation_signal>()),
          callback_(std::move(stop_token), SignalEmitter{std::move(executor), signal_}) {}

    CancellationBridge(const CancellationBridge&) = delete;
    CancellationBridge& operator=(const CancellationBridge&) = delete;

    /// Binds the signal slot to one completion token.
    template <typename CompletionToken> [[nodiscard]] auto bind(CompletionToken&& token) const {
        return boost::asio::bind_cancellation_slot(signal_->slot(), std::forward<CompletionToken>(token));
    }

    /// Shared ownership of the signal, for the timeout timer that must cancel
    /// the same in-flight operations.
    [[nodiscard]] std::shared_ptr<boost::asio::cancellation_signal> signal_ptr() const { return signal_; }

private:
    struct SignalEmitter {
        boost::asio::any_io_executor executor;
        std::shared_ptr<boost::asio::cancellation_signal> signal;

        void operator()() const {
            boost::asio::post(executor, [signal = signal] { signal->emit(boost::asio::cancellation_type::all); });
        }
    };

    std::shared_ptr<boost::asio::cancellation_signal> signal_;
    std::stop_callback<SignalEmitter> callback_;
};

} // namespace cch::mcp::transport
