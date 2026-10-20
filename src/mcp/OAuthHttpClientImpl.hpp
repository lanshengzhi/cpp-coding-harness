#pragma once

// Private Boost.Beast implementation of the OAuth HTTP seam.

#include <cch/mcp/OAuthHttpClient.hpp>

#include <memory>

namespace cch::mcp {

class BoostBeastOAuthHttpClient final : public OAuthHttpClient {
public:
    [[nodiscard]] support::AsyncResult<OAuthHttpResponse> post(std::string url,
            std::map<std::string, std::string, std::less<>> headers,
            std::string body,
            std::stop_token stop_token) override;

    [[nodiscard]] support::AsyncResult<OAuthHttpResponse> get(std::string url,
            std::map<std::string, std::string, std::less<>> headers,
            std::stop_token stop_token) override;
};

} // namespace cch::mcp
