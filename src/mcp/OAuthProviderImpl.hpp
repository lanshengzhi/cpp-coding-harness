#pragma once

// Private implementation of the configured-endpoint OAuth provider.

#include <cch/mcp/Auth.hpp>
#include <cch/mcp/OAuthHttpClient.hpp>
#include <cch/mcp/OAuthServerConfig.hpp>

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <stop_token>
#include <string>

namespace cch::mcp {

class OAuthProviderImpl {
public:
    OAuthProviderImpl(OAuthServerConfig config, std::shared_ptr<OAuthHttpClient> http_client);

    OAuthProviderImpl(OAuthProviderImpl&&) noexcept;
    OAuthProviderImpl& operator=(OAuthProviderImpl&&) noexcept;
    ~OAuthProviderImpl();
    OAuthProviderImpl(const OAuthProviderImpl&) = delete;
    OAuthProviderImpl& operator=(const OAuthProviderImpl&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> login(AuthInteraction interaction);
    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> refresh(OAuthCredential credential);

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> request_token(
            std::string body, std::stop_token stop_token);

    OAuthServerConfig config_;
    std::shared_ptr<OAuthHttpClient> http_client_;
};

} // namespace cch::mcp
