#pragma once

#include <cch/util/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace cch::ai::auth {

struct OAuthCallbackServerOptions {
    /// Bind host; the frozen default is 127.0.0.1 with PI_OAUTH_CALLBACK_HOST
    /// resolved by the caller.
    std::string host{"127.0.0.1"};
    /// Bind port; the frozen default is 1455.
    std::uint16_t port{1455};
    /// Expected OAuth state; only a matching `/auth/callback` settles the wait.
    std::string state{};
};

/// Local loopback callback server for the Codex browser login flow. Serves
/// the frozen pi HTML pages for `/auth/callback` validation in pi's exact
/// order (404 route / 400 state / 400 code / 200 success). A listen failure
/// degrades the server so `wait_for_code` resolves `std::nullopt` and login
/// continues with manual code entry only.
class OAuthCallbackServer {
private:
    struct Impl;

public:
    /// Binds the acceptor. On any listen failure the returned server still
    /// reports `wait_for_code` as `std::nullopt` (manual-input-only degrade).
    [[nodiscard]] static boost::asio::awaitable<
        util::Expected<std::shared_ptr<OAuthCallbackServer>>>
    start(OAuthCallbackServerOptions options);

    ~OAuthCallbackServer();
    OAuthCallbackServer(const OAuthCallbackServer&) = delete;
    OAuthCallbackServer& operator=(const OAuthCallbackServer&) = delete;

    /// Constructed only through `start`.
    explicit OAuthCallbackServer(std::shared_ptr<Impl> impl);

    /// Actual bound port; matches options.port unless the OS assigned one.
    [[nodiscard]] std::uint16_t bound_port() const;

    /// Resolves with the authorization code when a valid callback arrives, or
    /// `std::nullopt` when the wait is cancelled (manual prompt won, listen
    /// failure, server closed). Safe to await once.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>>
    wait_for_code();

    /// Settle a pending `wait_for_code` with `std::nullopt` (idempotent).
    void cancel_wait();

    /// Stop accepting and close outstanding connections (idempotent).
    void close();

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::ai::auth
