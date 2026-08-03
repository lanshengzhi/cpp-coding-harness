#pragma once

#include "OAuthHttpClient.hpp"

#include <cch/ai/Auth.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace cch::ai::auth {

struct KimiCodingOAuthOptions {
    /// Override for the OAuth host. When nullopt, KIMI_CODE_OAUTH_HOST then
    /// KIMI_OAUTH_HOST are resolved at request time, falling back to
    /// `https://auth.kimi.com` with trailing slashes stripped, matching pi's
    /// `getOauthHost` (`auth/oauth/kimi-coding.ts`).
    std::optional<std::string> oauth_host{std::nullopt};
    /// Per-request timeout composed with the login cancellation token via pi's
    /// `requestSignal` = `AbortSignal.any([AbortSignal.timeout(30s), signal])`.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// Refresh backoff base. The frozen exponentiation 1s/2s/4s is preserved;
    /// the base is injectable so tests can shrink the real-time wait.
    std::chrono::milliseconds refresh_backoff_base{std::chrono::seconds{1}};
    /// Refresh retry ceiling: attempts run 0..`refresh_max_retries` inclusive.
    /// The frozen pi value is 3 (four attempts total with 1s/2s/4s backoff).
    int refresh_max_retries{3};
};

/// Kimi Code (subscription) OAuth content, ported from pi baseline 83114817
/// `packages/ai/src/auth/oauth/kimi-coding.ts`. Owns the device-flow request
/// shapes, poll/refresh/error messages, and `toAuth`; never renders.
class KimiCodingOAuth final : public std::enable_shared_from_this<KimiCodingOAuth> {
public:
    explicit KimiCodingOAuth(
        std::shared_ptr<OAuthHttpClient> http_client,
        KimiCodingOAuthOptions options = {});
    ~KimiCodingOAuth();
    KimiCodingOAuth(const KimiCodingOAuth&) = delete;
    KimiCodingOAuth& operator=(const KimiCodingOAuth&) = delete;

    /// RFC 8628 device authorization grant. Notifies a device_code event with
    /// `verification_uri_complete`, then polls for the token with the shared
    /// device-poll helper (wait-before-first-poll, interval/expires defaults
    /// 5s/15min, 30s per-request timeout composed with the interaction token).
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::OAuthCredential>> login(
        ai::AuthInteraction interaction);

    /// Keeps pi's signal parameter for parity (`kimiCodingOAuth.refresh`
    /// accepts an AbortSignal), but the only in-scope call path (the
    /// `OAuthRefreshHook`) never passes one: the frozen Kimi refresh-signal
    /// defect is reproduced as no-divergence, so request-path refresh is
    /// uncancellable.
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::OAuthCredential>> refresh(
        ai::OAuthCredential credential,
        std::stop_token stop_token = {});

    /// Derives request authentication as `Authorization: Bearer <access>`.
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::ModelAuth>> to_auth(
        const ai::OAuthCredential& credential) const;

private:
    std::shared_ptr<OAuthHttpClient> http_client_;
    KimiCodingOAuthOptions options_;
};

/// Build the `OAuthAuth` hooks for the kimi-coding provider. The default HTTP
/// client is the Boost.Beast implementation; tests inject scripted fakes.
[[nodiscard]] ai::OAuthAuth make_kimi_coding_oauth_auth(
    std::shared_ptr<OAuthHttpClient> http_client = nullptr,
    KimiCodingOAuthOptions options = {});

} // namespace cch::ai::auth
