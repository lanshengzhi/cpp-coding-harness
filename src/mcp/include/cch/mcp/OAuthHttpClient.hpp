#pragma once

// Injectable HTTP client used by the OAuth content layer: POST for token and
// registration requests, GET for the RFC 9728 / RFC 8414 metadata documents.
// Requests observe the passed `std::stop_token`; implementations return a
// `Cancelled` error when the token stops so the flow can normalize it to
// "Login cancelled".

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <map>
#include <memory>
#include <stop_token>
#include <string>

namespace cch::mcp {

struct OAuthHttpResponse {
    int status_code{0};
    std::string body{};
};

class OAuthHttpClient {
public:
    virtual ~OAuthHttpClient() = default;

    [[nodiscard]] virtual support::AsyncResult<OAuthHttpResponse> post(std::string url,
            std::map<std::string, std::string, std::less<>> headers,
            std::string body,
            std::stop_token stop_token) = 0;

    /// A GET with no body, used by OAuth discovery. The response body is read
    /// whatever the status, because a discovery miss (4xx/502) is the caller's
    /// signal to try the next candidate.
    [[nodiscard]] virtual support::AsyncResult<OAuthHttpResponse> get(
            std::string url, std::map<std::string, std::string, std::less<>> headers, std::stop_token stop_token) = 0;
};

/// The production Boost.Beast/OpenSSL implementation for `https://` endpoints.
[[nodiscard]] std::shared_ptr<OAuthHttpClient> default_oauth_http_client();

} // namespace cch::mcp
