#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::mcp {

class UpstreamCredentialStore;

/// One parsed `WWW-Authenticate` challenge, reduced to what the authorization
/// flow needs (issue #849, spec #833 story 34).
///
/// Only the `Bearer` scheme is reduced; a challenge naming any other scheme
/// carries an empty `scheme` and no members, because the MCP Host answers
/// only OAuth challenges and must never mistake a `Basic` or `Negotiate`
/// challenge for one it can act on.
struct AuthorizationChallenge {
    /// The challenge's auth-scheme as the header spelled it, `Bearer` and
    /// `bearer` both accepted. Empty when the header held no `Bearer`
    /// challenge this build can act on.
    std::string scheme{};
    /// The RFC 9728 protected-resource-metadata URL the challenge pointed at.
    std::string resource_metadata_url{};
    /// The RFC 6750 `scope` the challenge asked for, space-delimited.
    std::string scope{};
    /// The RFC 6750 `error` codes the challenge carried, in the order they
    /// were written. They are the flow's evidence, not the user's: nothing
    /// here is published on the `needs_auth` status, because that path has no
    /// resolved credential to redact a server-written value with (issue #838,
    /// CODING_STANDARDS.md §10.7).
    std::vector<std::string> error_codes{};
};

/// What one Upstream MCP Server's authorization server is (issue #849). The
/// values are discovered, never configured: the endpoint triple comes from the
/// issuer's metadata document, and the issuer itself is the identity every
/// credential is keyed by.
struct UpstreamOAuthMetadata {
    /// The authorization server's issuer identifier. Every credential read or
    /// written by this flow carries exactly this value, and a credential
    /// issued by any other issuer is never used for it (RFC 9207).
    std::string issuer{};
    std::string authorization_endpoint{};
    std::string token_endpoint{};
    /// Absent when the authorization server publishes no dynamic client
    /// registration endpoint, in which case the flow cannot register a client
    /// and fails rather than guessing a client id.
    std::string registration_endpoint{};
    /// The scopes to request. Empty means the flow requests the scopes the
    /// protected-resource metadata asked for, and nothing more.
    std::vector<std::string> scopes{};
};

/// One Upstream MCP Server's OAuth credential, keyed by the issuer that minted
/// it (issue #849; the 2026-07-28 auth hardening).
///
/// The issuer is part of the credential rather than of the record it is
/// stored in alone, so a store can answer "is the credential under this key
/// the one *this* issuer issued?" without the caller trusting a key it did
/// not build. Every field here except `issuer` and `client_id` is secret:
/// the access and refresh tokens never reach a diagnostic, a status surface,
/// or a session record (issue #838, CODING_STANDARDS.md §10.7).
struct UpstreamOAuthCredential {
    /// The issuer this credential was issued by. It is the key a lookup
    /// matches, so issuers never cross-use one another's credentials.
    std::string issuer{};
    /// The dynamically registered client id, persisted with the credential so
    /// a later refresh presents the same client the authorization used.
    std::string client_id{};
    std::string access_token{};
    /// Absent when the authorization server issued no refresh token; such a
    /// credential is used until it expires and then the user authorizes again.
    std::string refresh_token{};
    /// Expiry in seconds since the Unix epoch; `0` when the authorization
    /// server declared none.
    std::int64_t expires_at{0};
    std::vector<std::string> scopes{};
};

/// How one browser authorization ended, as the status surface reports it
/// (issue #849).
enum class UpstreamOAuthOutcome {
    /// The user authorized the server and the credential was persisted.
    Authorized,
    /// The user dismissed the authorization. Nothing was persisted and the
    /// connection stays `needs_auth`.
    Cancelled,
    /// The flow failed. Nothing was persisted and the connection stays
    /// `needs_auth`.
    Failed,
};

/// The user-facing summary of one authorization. `message` is a bounded,
/// redacted line and never carries a token.
struct UpstreamOAuthReport {
    std::string server_id{};
    std::string issuer{};
    UpstreamOAuthOutcome outcome{UpstreamOAuthOutcome::Failed};
    std::string message{};
};

/// What one authorization presents to the user. The URL is the whole
/// presentation: there is no graphical OAuth progress surface (issue #849
/// non-goals), so a frontend shows the URL, may open a browser, and waits.
struct UpstreamOAuthPrompt {
    std::string server_id{};
    std::string issuer{};
    /// The authorization request URL. It carries the PKCE challenge, never a
    /// secret, and it is the only value a frontend needs to present.
    std::string authorization_url{};
};

/// How the MCP Host asks its owner to run one browser authorization
/// (issue #849).
///
/// The interface exists because the two halves of the flow live in different
/// packages: the loopback callback and the token exchange are `cch_mcp`'s,
/// and showing a URL and waiting for a dismissal is the caller's. It is a
/// second port beside the tool-approval and trust ports, not a new seam: the
/// wire exchanges themselves still ride the one `McpTransport`.
class UpstreamOAuthPrompter {
public:
    virtual ~UpstreamOAuthPrompter() = default;

    /// Present `prompt` and resolve when the user is done with it. A
    /// `Cancelled` answer means the user dismissed the authorization, which
    /// stops the flow and persists nothing; any other error fails it the same
    /// way. An implementation that cannot present anything (a non-interactive
    /// session) must fail rather than resolve, so the flow fails closed
    /// instead of waiting for a browser nobody is watching.
    [[nodiscard]] virtual cch::support::AsyncResult<void> present(
            UpstreamOAuthPrompt prompt, std::stop_token stop_token) = 0;

    /// Report the terminal outcome so the frontend can close what `present`
    /// opened. Called exactly once per `present` that did not fail, on the
    /// flow's own thread; it may only present the value.
    virtual void finish(UpstreamOAuthReport report) = 0;
};

/// What one `/mcp auth` authorization needs (issue #849, spec #833 stories
/// 34 and 35).
struct UpstreamOAuthRequest {
    /// The Server Id whose credential is being authorized; the credential is
    /// stored under it and the report names it.
    std::string server_id{};
    /// The Upstream MCP Server's endpoint, used as the RFC 8707 `resource`
    /// parameter and as the discovery base when `metadata` is empty.
    std::string resource_url{};
    /// The challenge the Upstream answered `401` with, when one was recorded.
    /// Its `resource_metadata` URL is the discovery entry point; an empty
    /// challenge falls back to `<origin>/.well-known/oauth-protected-resource`.
    std::optional<AuthorizationChallenge> challenge{std::nullopt};
    /// Metadata the caller already resolved, so a repeated authorization
    /// skips discovery. Empty means discover it.
    std::optional<UpstreamOAuthMetadata> metadata{std::nullopt};
    /// The client name dynamic client registration declares.
    std::string client_name{"pike MCP Host"};
};

/// The one successful authorization's result. It names the issuer the
/// credential is keyed by and whether that credential can be refreshed, so a
/// connection can authenticate from it without re-running the flow.
struct UpstreamOAuthGrant {
    std::string server_id{};
    std::string issuer{};
    /// The registered client id, which the connection's later refreshes
    /// present.
    std::string client_id{};
    /// Absent when the authorization server issued no refresh token.
    std::optional<std::string> refresh_token{std::nullopt};
    /// The access token's expiry, in seconds since the Unix epoch.
    std::int64_t expires_at{0};
    UpstreamOAuthOutcome outcome{UpstreamOAuthOutcome::Authorized};
};

/// Run one browser authorization for an Upstream MCP Server, end to end
/// (issue #849, spec #833 stories 34 and 35; ADR 0032's division of labour,
/// reimplemented rather than shared).
///
/// The flow is: discover the authorization server from the challenge (or the
/// endpoint) when `request.metadata` is empty; register a client dynamically
/// with `application_type` declared when the issuer already holds no client id
/// for this Server Id; start a loopback callback on an ephemeral port; build
/// the authorization request with PKCE; present the URL through `prompter`;
/// wait for the callback; **verify the `iss` parameter against the issuer the
/// request was sent to**; exchange the code; and persist the credential under
/// `(server_id, issuer)`.
///
/// Every failure persists nothing, and an `iss` that does not match is a
/// failure, never a warning: the authorization code is then refused before it
/// is exchanged, which is what makes this an authorization-code-injection
/// defence rather than a diagnostic (RFC 9207 §2.4).
///
/// `stop_token` cancels the flow wherever it has reached — the prompt, the
/// callback wait, the discovery exchanges, and the token exchange — and the
/// operation completes as `Cancelled`. A `prompter` that is absent is a
/// failure: a session with no way to show a URL cannot authorize anything and
/// must not wait for one (issue #849, fail closed).
[[nodiscard]] cch::support::AsyncResult<UpstreamOAuthGrant> authorize_upstream(UpstreamOAuthRequest request,
        std::shared_ptr<McpTransport> transport,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::shared_ptr<UpstreamOAuthPrompter> prompter,
        std::stop_token stop_token = {});

} // namespace cch::mcp
