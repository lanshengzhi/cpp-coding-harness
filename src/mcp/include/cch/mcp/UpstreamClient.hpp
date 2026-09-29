#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamToolCall.hpp>
#include <cch/support/AsyncResult.hpp>

#include <chrono>
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
    [[nodiscard]] cch::support::AsyncResult<UpstreamToolCallResult> call_tool(
            UpstreamToolCall call, std::stop_token stop_token = {});

private:
    std::string server_id_;
    /// Shared live state between this client and the operations it has in
    /// flight; owns the transport, the endpoint, the negotiated era, and the
    /// per-connection request-id counter.
    std::shared_ptr<Connection> connection_;
};

} // namespace cch::mcp
