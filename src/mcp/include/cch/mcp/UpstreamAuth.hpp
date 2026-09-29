#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamOAuth.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::mcp {

/// The credential-store key one Upstream MCP Server's credentials occupy: the
/// `mcp.<server-id>` namespace of pi's shared credential store
/// (`CONTEXT.md`, spec #833 story 33). A Server Id is drawn from
/// `[A-Za-z0-9_-]` and so can never contain a `.`, which makes the namespace
/// prefix unambiguous over the store's flat key map. OAuth (#849) keys its
/// issuer-keyed credentials under the same namespace.
[[nodiscard]] std::string credential_key(std::string_view server_id);

/// Short-lived request authentication for one Upstream MCP Server (issue
/// #838, spec #833 story 33).
///
/// The bearer token is the secret: it lives in this value and in the
/// transport's request headers and nowhere else. It never enters
/// `UpstreamClientOptions`, a diagnostic, a log, or `settings.json`, and the
/// environment reference that produced it carries only the variable's name.
/// The carrier is owned by this package and declared here rather than reusing a
/// provider's authentication value, keeping the MCP credential lifecycle
/// independent of `cch_ai` (ADR 0065).
struct UpstreamAuth {
    /// The bearer token. Absent when the server declares no credential, in
    /// which case the request carries no `Authorization` header.
    std::optional<std::string> bearer{};
};

/// The credential-store handoff the MCP Host resolves its bearer through
/// (issue #838, spec #833 story 33).
///
/// The `cch_mcp` package owns upstream credential *handling*; the
/// `<agentDir>/auth.json` path derivation, the whole-file lock, the
/// owner-only permissions, and the lossless serializer stay with
/// `cch::coding_agent::AuthStorage`, which implements this interface. The
/// dependency direction is the legal one — `cch_coding_agent` reaches
/// `cch_mcp`, never the reverse — and no credential type crosses the boundary
/// in either direction (ADR 0065, ADR 0030).
class UpstreamCredentialStore {
public:
    virtual ~UpstreamCredentialStore() = default;

    /// The stored bearer for one Server Id, `std::nullopt` when the
    /// `mcp.<server-id>` key holds no credential. A store that cannot be read
    /// is an error, never a silent "no credential" — a failure to read must not
    /// be answered with an unauthenticated request.
    [[nodiscard]] virtual cch::support::AsyncResult<std::optional<std::string>> read_bearer(std::string server_id) = 0;

    /// The only write path: persist one Server Id's bearer under the
    /// `mcp.<server-id>` key, replacing any credential already there. The token
    /// reaches the store and nowhere else.
    [[nodiscard]] virtual cch::support::AsyncResult<void> write_bearer(std::string server_id, std::string bearer) = 0;

    /// The OAuth credential one issuer issued for one Server Id, or
    /// `std::nullopt` when the key holds no OAuth credential **or holds one
    /// issued by a different issuer**. The issuer is the key a lookup matches,
    /// so a credential is never presented to an authorization server that did
    /// not issue it, and one server's credential can never be spent against
    /// another server's issuer (issue #849).
    ///
    /// The two OAuth members are deliberately a separate pair of operations
    /// rather than a wider `read`: a bearer and an OAuth credential occupy the
    /// same Server Id and may not overwrite one another.
    [[nodiscard]] virtual cch::support::AsyncResult<std::optional<UpstreamOAuthCredential>> read_oauth(
            std::string server_id, std::string issuer) = 0;

    /// The only write path for an OAuth credential: persist it under
    /// `server_id`, keyed by the issuer it carries. A record of another type
    /// already under the key is left alone rather than replaced.
    [[nodiscard]] virtual cch::support::AsyncResult<void> write_oauth(
            std::string server_id, const UpstreamOAuthCredential& credential) = 0;
};

/// Resolve the live bearer for one Upstream MCP Server, per request
/// (issue #838, spec #833 story 33, ADR 0032's resolve-before-each-request
/// rule).
///
/// `bearer_env_var` is the *name* of the environment variable a
/// `bearer-env:<VAR>` reference in `settings.json` declared; the variable's
/// value is the secret and is never echoed into a diagnostic. A declared
/// variable that is set is authoritative: it is persisted under the
/// `mcp.<server-id>` credential key when the store does not already hold it,
/// and the request is authenticated from it. An unset or empty variable falls
/// back to the stored credential, which is how a credential this package did
/// not resolve from the environment authenticates a request. With neither a
/// resolvable environment value nor a stored credential the request fails as a
/// connection failure and nothing is sent: a guessed or empty token is never
/// used.
///
/// `store` is required exactly when a reference is declared; a declared
/// credential with no store is a connection failure, never an unauthenticated
/// request. A server that declares no reference resolves no credential, is
/// sent no `Authorization` header, and never touches the store.
[[nodiscard]] cch::support::AsyncResult<UpstreamAuth> resolve_upstream_auth(std::string server_id,
        std::optional<std::string> bearer_env_var,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::stop_token stop_token = {});

/// Resolve the live bearer an OAuth authorization stored, per request
/// (issue #849).
///
/// `issuer` is the issuer this connection authenticates against, learned from
/// the challenge the Upstream answered `401` with. A credential issued by any
/// other issuer is not a credential for it, so the lookup is keyed by issuer
/// and a mismatch reads as no credential at all rather than as a usable token
/// the connection would present to the wrong authorization server.
[[nodiscard]] cch::support::AsyncResult<UpstreamAuth> resolve_oauth_bearer(std::string server_id,
        std::string issuer,
        std::shared_ptr<UpstreamCredentialStore> store,
        std::stop_token stop_token = {});

/// Write one resolved `auth` onto a transport request: the bearer becomes
/// `Authorization: Bearer <token>`, and only when the request does not already
/// carry an `Authorization` header of its own. HTTP field names are
/// case-insensitive, so the occupied slot is looked up case-insensitively
/// rather than by one spelling.
void apply_upstream_auth(McpRequest& request, const UpstreamAuth& auth);

} // namespace cch::mcp
