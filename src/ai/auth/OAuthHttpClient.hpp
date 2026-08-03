#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <map>
#include <memory>
#include <stop_token>
#include <string>

namespace cch::ai::auth {

struct OAuthHttpResponse {
    int status_code{0};
    std::string body{};
};

/// Injectable HTTPS POST client used by the OAuth content layer. Login
/// requests observe the passed `std::stop_token`; implementations return a
/// `Cancelled` error when the token stops so the flow can normalize it to
/// "Login cancelled".
class OAuthHttpClient {
public:
    virtual ~OAuthHttpClient() = default;

    [[nodiscard]] virtual boost::asio::awaitable<util::Expected<OAuthHttpResponse>> post(
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) = 0;
};

/// Default Boost.Beast/OpenSSL implementation for `https://` endpoints.
class BoostBeastOAuthHttpClient final : public OAuthHttpClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<OAuthHttpResponse>> post(
        std::string url,
        std::map<std::string, std::string, std::less<>> headers,
        std::string body,
        std::stop_token stop_token) override;
};

} // namespace cch::ai::auth
