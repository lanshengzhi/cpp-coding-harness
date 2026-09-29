#pragma once

#include <cch/coding_agent/McpServerTrust.hpp>
#include <cch/coding_agent/McpUpstreamStatus.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamConnection.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::runtime {

/// What the session wires the MCP Host with (issue #841). Every field is a
/// value the caller already owns: the configured `mcpServers` entries, the
/// trust store's file, the credential store over `auth.json`, and the
/// session's serialized Runtime executor. Nothing here names an Upstream
/// protocol decision — the connection machinery in `cch_mcp` owns those.
struct McpSessionHostOptions {
    /// The configured Upstream MCP Servers, in `mcpServers` order. The order
    /// is the order the `/mcp` overview renders and the order connections
    /// are started in.
    std::vector<UserMcpServerSettings> servers{};
    /// `<agentConfigDir>/mcp-trust.json`, the first-enable consent store
    /// (issue #840). An unreadable store fails closed there, so this host
    /// never has to decide what an unreadable decision means.
    std::filesystem::path trust_store_path{};
    /// The `mcp.<server-id>` credential store (issue #838). Null is a
    /// session with no credential store, which a server that declares a
    /// `bearer-env:` reference can never connect with — the connection
    /// fails closed rather than sending an unauthenticated request.
    std::shared_ptr<mcp::UpstreamCredentialStore> credentials{nullptr};
    /// The session's serialized execution domain. Every connection's
    /// operations and its reconnect timer run there, one at a time, as the
    /// transport and connection contracts require.
    boost::asio::any_io_executor executor{};
    /// Private test seam (issue #841): the transport every connection in
    /// this session uses. Empty means the production Streamable HTTP
    /// transport. A test injects `tests::ScriptedMcpTransport` here, which
    /// is the MCP Host's one transport seam — no second seam is added.
    std::shared_ptr<mcp::McpTransport> transport{nullptr};
    /// Private test seam: the connection's timer. Empty means a Runtime
    /// `steady_timer` on `executor`; a test injects
    /// `tests::ScriptedMcpDelay` so the bounded reconnect ladder and the
    /// cleanup bound are driven without a wall clock.
    mcp::UpstreamDelay delay{};
    /// The frontend's first-enable trust prompt (issue #840). Empty is a
    /// non-interactive session, where an undecided server is declined for
    /// the session and nothing is persisted.
    McpServerTrustPrompter trust_prompter{};
};

/// The session's MCP Host wiring (issue #841, ADR 0065): the trust-gated set
/// of Upstream MCP Server connections this Agent Session owns.
///
/// Starting the host starts one connection attempt per *enabled* server and
/// waits for none of them. A configured server the user has not enabled is
/// `disabled` here and is never contacted, which is what makes "no upstream
/// request without consent" a property of the construction rather than of a
/// check somebody can forget. The first-enable prompt is a separate,
/// caller-driven step: until the user answers it the server stays disabled,
/// and the answer is what enables it (and only then connects it).
///
/// The host is a passive owner of values plus the connections themselves. It
/// holds no event loop of its own: everything runs on the caller's serialized
/// executor, and closing the host is the deterministic two-phase close of
/// ADR 0011 across every connection it owns.
class McpSessionHost {
public:
    /// The state the connections and the in-flight operations share, so a
    /// connection can publish its status after the host handle that started
    /// it is gone. Defined in the implementation file.
    struct State;

    /// Wire the configured servers and start every enabled connection now.
    /// Never blocks: the returned handle exists before any Upstream has
    /// answered, and a server that never answers at all does not delay it.
    /// Returns `nullptr` when no server is configured, which is the ordinary
    /// session with no MCP surface at all.
    [[nodiscard]] static std::shared_ptr<McpSessionHost> start(McpSessionHostOptions options);

    /// One row per configured Server Id, in `mcpServers` order, with the
    /// state the connection machinery last published. The whole read model
    /// for the `/mcp` overview.
    [[nodiscard]] std::vector<McpUpstreamStatus> upstream_status() const;

    /// The configured servers still awaiting the user's first-enable
    /// consent, in configuration order.
    [[nodiscard]] std::vector<McpServerTrustPromptRequest> pending_trust_requests() const;

    /// Ask one server's first-enable prompt and, when the user accepts,
    /// enable and connect that server — the only path on which a first
    /// upstream request can happen. A server that already has a decision is
    /// returned as is, without asking. The host must outlive the returned
    /// operation, which records the answer into the gate this host owns.
    [[nodiscard]] support::AsyncResult<McpServerTrustResolution> ask_trust(
            std::string_view server_id, std::stop_token stop_token = {});

    /// The connection for one Server Id, or `nullptr` when the server is
    /// configured but not enabled (or was never configured). The seam the
    /// tool-publication path builds `cch::agent::Tool` values over.
    [[nodiscard]] mcp::UpstreamConnection* connection(std::string_view server_id) const;

    /// The deterministic two-phase close of ADR 0011 across every connection
    /// this host owns: stop admission, request cancellation, and complete
    /// within the connection cleanup bound. No Upstream socket or timer
    /// outlives it. Idempotent.
    [[nodiscard]] support::AsyncResult<void> close();

private:
    McpSessionHost() = default;
    std::shared_ptr<State> state_;
};

} // namespace cch::coding_agent::runtime
