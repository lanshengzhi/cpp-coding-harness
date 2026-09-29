#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// What one Upstream MCP Server's browser authorization is (issue #849, spec
/// #833 stories 34 and 35).
///
/// The value is a projection: the authorization itself, the loopback callback,
/// and the token exchange all live in `cch_mcp`, and a frontend reads only
/// this (ADR 0065). Nothing here carries a token — not the URL, not the
/// message, not the outcome — because every one of them can reach a chat
/// block, a diagnostic, and a session record.
struct McpOAuthRequest {
    /// The Server Id being authorized: the sole stable identity of the Upstream
    /// MCP Server, and the credential key's suffix.
    std::string server_id{};
    /// The authorization server's issuer, once the flow has discovered it. The
    /// credential is keyed by it, so it is the identity the stored credential
    /// belongs to.
    std::string issuer{};
    /// The authorization request URL to show the user. It carries a PKCE
    /// challenge and a state, never a secret, and it is the only value a
    /// frontend needs to present.
    std::string authorization_url{};
};

/// How one authorization ended, as the session's own vocabulary says it.
enum class McpOAuthStatus {
    /// The user authorized the server and the credential was persisted; the
    /// connection now authenticates from it.
    Authorized,
    /// The user dismissed the authorization. Nothing was persisted and the
    /// connection stays `needs_auth`.
    Cancelled,
    /// The flow failed. Nothing was persisted and the connection stays
    /// `needs_auth`.
    Failed,
};

/// One authorization's outcome for the session to report.
struct McpOAuthOutcome {
    std::string server_id{};
    std::string issuer{};
    McpOAuthStatus status{McpOAuthStatus::Failed};
    /// A bounded, redacted, one-line explanation. Never a token.
    std::string message{};
};

/// Presents one authorization URL and resolves when the user is done with it.
///
/// A `Cancelled` answer means the user dismissed the dialog, which stops the
/// flow and persists nothing. An implementation that cannot present anything —
/// a headless run, a host that is not live — must fail rather than resolve, so
/// a non-interactive session fails closed instead of waiting for a browser
/// nobody is watching.
using McpOAuthPromptSink =
        std::move_only_function<cch::support::AsyncResult<void>(McpOAuthRequest request, std::stop_token stop_token)>;

/// The terminal outcome of the authorization the prompt was showing, so the
/// frontend can close what it opened. A bounded, redacted line and the
/// identity of what was authorized; never a token.
using McpOAuthFinishSink = std::move_only_function<void(McpOAuthOutcome outcome)>;

} // namespace cch::coding_agent
