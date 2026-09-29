#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamElicitation.hpp>
#include <cch/mcp/UpstreamOAuth.hpp>
#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamToolCall.hpp>
#include <cch/support/AsyncResult.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::mcp {

struct UpstreamClientOptions {
    /// The Upstream MCP Server's Streamable HTTP endpoint.
    std::string url{};
    /// Bounds one exchange: connection setup, request dispatch, and response
    /// headers (spec #833 story 24, 30 s default).
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// The *name* of the environment variable a `bearer-env:<VAR>` reference
    /// in `settings.json` declared (issue #835, #838). The variable's value is
    /// the secret and is resolved per request from the environment, so it is
    /// never held here, in a diagnostic, or in configuration. `std::nullopt`
    /// means the server declares no bearer credential and every request
    /// authenticates nothing.
    std::optional<std::string> bearer_env_var{std::nullopt};
    /// The credential store the bearer is persisted under and, when the
    /// environment variable is unset, resolved from. Required whenever
    /// `bearer_env_var` is declared: a declared credential with no store is a
    /// connection failure, never an unauthenticated request.
    std::shared_ptr<UpstreamCredentialStore> credentials{nullptr};
    /// How a `tools/call` this client issues asks the user a Pending
    /// Elicitation question (issue #845). Null is a client with no user to
    /// ask, and an `input_required` result then fails exactly one tool call —
    /// the behaviour issue #836 shipped before the loop existed, kept as the
    /// default so a caller that has not wired the port degrades rather than
    /// hangs. Shared rather than owned so a reconnect re-asks through the same
    /// port; a port holds no per-call state, only the seam.
    std::shared_ptr<UpstreamElicitationPort> elicitation{nullptr};
    /// The issuer this connection authorizes against once `/mcp auth` has run
    /// (issue #849), absent until it has. When it is set and no `bearer_env_var`
    /// is declared, every request authenticates from the OAuth credential that
    /// issuer issued, resolved per request exactly as a bearer is.
    std::optional<std::string> oauth_issuer{std::nullopt};
    /// Where one parsed `WWW-Authenticate` challenge is reported, so the
    /// connection can remember what the Upstream asked for and the
    /// authorization flow can start from it. Called on the exchange's own
    /// domain with the reduced challenge; it carries no secret.
    std::optional<std::function<void(const AuthorizationChallenge&)>> auth_challenge_sink{std::nullopt};
};

/// The MCP Host's client stack for one Upstream MCP Server: the era probe, the
/// paginated tool catalog, and the tool call, all above the injected
/// transport (ADR 0065).
///
/// One client is one connection. The era adapter is selected once by probing
/// `server/discover` and every later request is framed by it; a reconnect
/// builds a new client, which re-probes. Operations on one client are driven
/// from a single serialized execution domain — the connection's executor —
/// and each operation owns everything it needs after the call returns, so
/// releasing the client while an operation is in flight is safe.
class UpstreamClient {
public:
    /// The per-connection live state this client shares with the operations it
    /// has in flight; defined in `UpstreamClient.cpp`.
    struct Connection;

    /// `transport` is the injected transport seam. The client shares it with
    /// the connection that owns this client, so a reconnect reuses one
    /// transport across connection lifetimes.
    UpstreamClient(std::string server_id, std::shared_ptr<McpTransport> transport, UpstreamClientOptions options = {});

    [[nodiscard]] const std::string& server_id() const noexcept { return server_id_; }

    /// The negotiated era's product name, empty until the probe succeeds.
    [[nodiscard]] std::string_view era_name() const noexcept;

    /// Probe `server/discover`, select this connection's era adapter, and
    /// report what the Upstream declared. This is the reconnect entry point:
    /// calling it again re-probes and re-selects.
    [[nodiscard]] cch::support::AsyncResult<UpstreamServerInfo> probe_era(std::stop_token stop_token = {});

    /// Walk `tools/list` to the end of the catalog under the per-Upstream tool
    /// cap and pagination-cursor cap, applying the defensive catalog rules
    /// (duplicate tool names, pagination-cursor loops, and invalid
    /// `x-mcp-header` annotations). Probes the era first when this connection
    /// has not probed yet.
    [[nodiscard]] cch::support::AsyncResult<UpstreamCatalog> list_tools(std::stop_token stop_token = {});

    /// Issue one `tools/call`, mirroring the tool's validated `x-mcp-header`
    /// parameters into request headers. Probes the era first when this
    /// connection has not probed yet. A protocol violation by the Upstream
    /// completes as one failed call rather than as an operation error.
    ///
    /// The call declares a progress token, so an Upstream that reports progress
    /// for it reaches `progress_sink` — and only for this call: a notification
    /// naming any other token, or no token this connection minted, is dropped
    /// rather than delivered (spec #833 story 31). The sink is called on this
    /// operation's own domain, never after the call has settled, and may only
    /// record or forward the value.
    ///
    /// Cancelling `stop_token` closes the response stream the transport is
    /// reading and makes the Upstream stop the work behind it: the call
    /// completes as `Cancelled` and one `notifications/cancelled` is written
    /// for this call's request id (ADR 0020, spec #833 story 30).
    ///
    /// A result the Upstream suspends for client input runs the Multi
    /// Round-Trip loop when an elicitation port is configured: the operation
    /// stays pending while the user answers, then the **original** `name` and
    /// `arguments` are re-sent under a **new** JSON-RPC id together with the
    /// opaque `requestState` echoed verbatim and the user's answer. Without a
    /// port the suspension fails one call, which is the defensive matrix.
    ///
    /// A stopped wait inside the Multi Round-Trip loop is the same stop: the
    /// pending question is withdrawn from the port, the call completes as
    /// `Cancelled`, and **nothing is re-sent** — the user never answered, so
    /// nothing goes out in their name.
    [[nodiscard]] cch::support::AsyncResult<UpstreamToolCallResult> call_tool(
            UpstreamToolCall call,
            std::stop_token stop_token = {},
            UpstreamProgressSink progress_sink = nullptr);

private:
    std::string server_id_;
    /// Shared live state between this client and the operations it has in
    /// flight; owns the transport, the endpoint, the negotiated era, and the
    /// per-connection request-id counter.
    std::shared_ptr<Connection> connection_;
};

} // namespace cch::mcp
