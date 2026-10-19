#pragma once

#include "support/TransportExecutor.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

#include <openssl/ssl.h>

#include <string>
#include <string_view>

namespace cch::support {

/// One parsed absolute URL: authority split into host/port, path as target.
struct ParsedUrl {
    bool is_tls{true};
    std::string host;
    std::string port{"443"};
    std::string target{"/"};
};

/// Parses an absolute HTTP or HTTPS URL. `bad_scheme_detail` is the caller's verbatim
/// unsupported-scheme detail.
[[nodiscard]] inline Expected<ParsedUrl> parse_transport_url(
        const std::string& url, std::string_view bad_scheme_detail, bool allow_http = true) {
    constexpr std::string_view kHttpsScheme{"https://"};
    constexpr std::string_view kHttpScheme{"http://"};

    bool is_tls = true;
    std::string_view rest;
    std::string default_port = "443";

    if (url.starts_with(kHttpsScheme)) {
        is_tls = true;
        rest = std::string_view{url}.substr(kHttpsScheme.size());
        default_port = "443";
    } else if (allow_http && url.starts_with(kHttpScheme)) {
        is_tls = false;
        rest = std::string_view{url}.substr(kHttpScheme.size());
        default_port = "80";
    } else {
        return std::unexpected(
                make_error(ErrorCode::Validation, "unsupported URL scheme", std::string{bad_scheme_detail}));
    }

    const auto slash = rest.find('/');
    const auto authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);

    ParsedUrl parsed;
    parsed.is_tls = is_tls;
    parsed.port = std::move(default_port);
    parsed.target = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};
    if (authority.empty()) {
        return std::unexpected(make_error(ErrorCode::Validation, "missing URL host", "URL is missing host"));
    }
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        return std::unexpected(
                make_error(ErrorCode::Validation, "invalid URL authority", "URL has invalid host or port"));
    }
    return parsed;
}

/// Loads the system default verify paths into one TLS client context.
[[nodiscard]] inline ExpectedVoid load_tls_client_ca(boost::asio::ssl::context& context) {
    boost::system::error_code error;
    context.set_default_verify_paths(error);
    if (error) {
        return std::unexpected(make_error(ErrorCode::Network, "CA loading failure", error.message()));
    }
    return {};
}

/// SNI plus peer verification for one TLS client stream.
[[nodiscard]] inline ExpectedVoid configure_tls_client_stream(TransportTlsStream& stream, const std::string& host) {
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        return std::unexpected(
                make_error(ErrorCode::Network, "TLS SNI setup failed", "OpenSSL rejected the host name"));
    }
    stream.set_verify_mode(boost::asio::ssl::verify_peer);
    stream.set_verify_callback(boost::asio::ssl::host_name_verification(host));
    return {};
}

} // namespace cch::support
