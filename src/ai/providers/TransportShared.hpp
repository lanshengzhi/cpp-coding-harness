#pragma once

#include "ai/TransportExecutor.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <openssl/ssl.h>

#include <optional>
#include <string>
#include <string_view>

namespace cch::ai::providers {

/// One parsed absolute URL: authority split into host/port, path as target.
struct ParsedUrl {
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

/// Parses an absolute URL under one required scheme (`https://` or `wss://`).
/// `noun`/`adjective` carry the caller's frozen error wording ("HTTPS"/"https"
/// for HTTP transports, "WebSocket"/"WebSocket" for the WebSocket transport);
/// `bad_scheme_detail` is the caller's verbatim unsupported-scheme detail.
[[nodiscard]] inline support::Expected<ParsedUrl> parse_transport_url(const std::string& url,
        std::string_view scheme,
        std::string_view noun,
        std::string_view adjective,
        std::string_view bad_scheme_detail) {
    if (!url.starts_with(scheme)) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "unsupported URL scheme", std::string{bad_scheme_detail}));
    }
    const auto rest = std::string_view{url}.substr(scheme.size());
    const auto slash = rest.find('/');
    const auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);

    ParsedUrl parsed;
    parsed.target = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};
    if (authority.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "missing " + std::string{noun} + " host",
                std::string{adjective} + " URL is missing host"));
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "invalid " + std::string{noun} + " authority",
                std::string{adjective} + " URL has invalid host or port"));
    }
    return parsed;
}

/// Loads the default verify paths (plus an optional extra test CA, used only
/// by the WebSocket transport's test seam) into one TLS client context.
[[nodiscard]] inline support::ExpectedVoid load_tls_client_ca(boost::asio::ssl::context& context,
        const std::optional<std::string>& trusted_ca_certificate_pem = std::nullopt) {
    boost::system::error_code error;
    context.set_default_verify_paths(error);
    if (error) {
        return std::unexpected(support::make_error(support::ErrorCode::Network, "CA loading failure", error.message()));
    }
    if (trusted_ca_certificate_pem) {
        if (context.add_certificate_authority(boost::asio::buffer(*trusted_ca_certificate_pem), error); error) {
            return std::unexpected(
                    support::make_error(support::ErrorCode::Network, "test CA loading failure", error.message()));
        }
    }
    return {};
}

/// SNI plus peer verification for one TLS client stream.
[[nodiscard]] inline support::ExpectedVoid configure_tls_client_stream(
        TransportTlsStream& stream, const std::string& host) {
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Network, "TLS SNI setup failed", "OpenSSL rejected the host name"));
    }
    stream.set_verify_mode(boost::asio::ssl::verify_peer);
    stream.set_verify_callback(boost::asio::ssl::host_name_verification(host));
    return {};
}

} // namespace cch::ai::providers
