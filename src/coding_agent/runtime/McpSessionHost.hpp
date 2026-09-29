#pragma once

#include <cch/coding_agent/McpOAuth.hpp>
#include <cch/coding_agent/McpServerTrust.hpp>
#include <cch/coding_agent/McpUpstreamStatus.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamCatalogCache.hpp>
#include <cch/mcp/UpstreamOAuth.hpp>
#include <cch/mcp/UpstreamConnection.hpp>
#include <cch/mcp/UpstreamElicitation.hpp>
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

#include "coding_agent/runtime/McpToolBinding.hpp"

namespace cch::coding_agent::runtime {

/// The MCP Host's cross-session tool-catalog cache, as a host owns it (ADR
/// 0067, spec #833 story 17, issue #848).
///
/// The returned value is a `process`-scoped `cch_mcp::UpstreamCatalogCache`,
/// which is the only scope in which a catalog may be reused across the
/// sessions one pike process runs: a `session`-scoped cache shared between
/// sessions would serve one session's catalog to a session that never fetched
/// it, which is the contamination `cacheScope` exists to prevent. It is
/// deliberately shared live state (CODING_STANDARDS §7.4), carried by
/// `std::shared_ptr`, and every operation takes the cache's own lock.
///
/// Ownership is injection, not a process-global: the host that runs the
/// interactive sessions calls this once and hands the value to every session
/// it creates, exactly as it hands the Models Runtime (ADR 0029/0030). No
/// `static` is involved, and the frontends never see a `cch_mcp` type — this
/// factory is how they name one (ADR 0065's `frontend-no-direct-mcp-includes`).
///
/// The lifetime this accepts, and what invalidates an entry, are ADR 0067's
/// subject; `docs/runtime-capacities.md` records the bounds.
[[nodiscard]] std::shared_ptr<mcp::UpstreamCatalogCache> make_host_catalog_cache();

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
    /// How a browser authorization presents its URL and learns that the user
    /// is done (issue #849). Both sinks are absent together, and both absent
    /// is a non-interactive session: an authorization then fails closed rather
    /// than waiting for a browser nobody is watching. Two function values
    /// rather than an `cch_mcp` port, so a frontend reaches the flow only
    /// through this Owner (ADR 0065).
    std::optional<McpOAuthPromptSink> oauth_prompt{std::nullopt};
    std::optional<McpOAuthFinishSink> oauth_finish{std::nullopt};
    /// The frontend's first-enable trust prompt (issue #840). Empty is a
    /// non-interactive session, where an undecided server is declined for
    /// the session and nothing is persisted.
    McpServerTrustPrompter trust_prompter{};
    /// The per-Upstream tool-catalog cache every connection in this session
    /// reads and fills (spec #833 story 17; issue #848). Null means every
    /// connection walks `tools/list` and caches nothing, which is the
    /// behaviour the host had before the cache existed.
    ///
    /// **Injected by the host; the interactive host hands every session the
    /// same instance, so a repeated session does not re-walk `tools/list`
    /// (spec #833 story 17, issue #848, ADR 0067).** The value itself is
    /// `cch_mcp`'s and is built by `make_host_catalog_cache()`. A caller that
    /// runs one session and exits needs no cache at all: null is a session
    /// that walks its own catalog exactly as it did before the cache existed.
    std::shared_ptr<mcp::UpstreamCatalogCache> catalog_cache{nullptr};
    /// The session's Upstream-tool binding (issue #842, issue #847). A
    /// connected server configured with `activation: "eager"` has its catalog
    /// turned into callable `cch::agent::Tool` values under their Qualified
    /// Tool Names and staged here; a connected `lazy` server has its catalog
    /// recorded for the two meta-tools to search and activate, with no tool
    /// published. Either way the session drains the staged values onto its
    /// Agent at a turn boundary. Null is a session that publishes no tools,
    /// which is the ordinary session with no configured MCP server.
    std::shared_ptr<McpToolBinding> tool_binding{nullptr};
    /// How this session's Upstreams ask the user a Pending Elicitation
    /// question (issue #845; spec #833 stories 27-29). Every connection the
    /// host owns asks through it, so two Upstreams blocked at once are two
    /// questions the session can see. Null is a session that cannot ask, and
    /// an `input_required` result then fails exactly one tool call rather
    /// than suspending a call nobody can answer (ADR 0008).
    std::shared_ptr<mcp::UpstreamElicitationPort> elicitation{nullptr};
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
/// A host with `catalog_cache` set shares that one cache with every session
/// that is handed it, which is what lets a repeated session skip a
/// `tools/list` walk its predecessor already paid for. The cache is consulted
/// on `list_tools` only, and a warm entry never makes anything wait
/// (issue #848).
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

    /// Run one browser authorization for a configured Server Id (issue #849,
    /// spec #833 stories 34 and 35): discover the authorization server from the
    /// challenge the Upstream answered `401` with, register a client, present
    /// the URL through the configured sinks, validate the authorization
    /// response's issuer, exchange the code, and store the credential under
    /// `(server_id, issuer)`.
    ///
    /// A success adopts the issuer on the server's connection and attempts it
    /// again, so the reconnect authenticates with what was just stored. Every
    /// other outcome persists nothing and leaves the connection `needs_auth`.
    /// The host must outlive the returned operation.
    [[nodiscard]] support::AsyncResult<McpOAuthOutcome> authorize(
            std::string_view server_id, std::stop_token stop_token = {});

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
