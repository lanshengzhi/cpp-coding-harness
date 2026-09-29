#pragma once

#include <cch/agent/AgentTool.hpp>
#include <cch/coding_agent/McpToolBinding.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/mcp/UpstreamConnection.hpp>
#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::runtime {

/// One discovered Upstream tool, staged for the Agent that will execute it: the
/// projection row beside the callable `agent::Tool` value the Qualified Tool
/// Name resolves to. Move-only, because the execute operation is.
struct McpToolPublication {
    McpPublishedTool tool{};
    agent::Tool binding{};
};

/// One Upstream MCP Server's own usage guidance, decoded by `cch_mcp` and
/// surfaced to the model (spec #833 story 18; issue #847).
struct McpServerInstructions {
    std::string server_id{};
    std::string text{};
};

/// What one `mcp_activate` call did. Activation is sticky for the session
/// (spec #833 story 16), so a second activation of the same tool is a success
/// that changed nothing, not an error.
enum class McpActivationOutcome { Activated, AlreadyActive };

/// The session's Upstream-tool binding (issue #842, ADR 0065): the one value
/// that turns a discovered upstream tool descriptor into a callable
/// `cch::agent::Tool` under its Qualified Tool Name, and the one place the
/// reverse mapping back to the Upstream's own Server Id and tool name is
/// kept.
///
/// Publication is asynchronous and split in two, deliberately. Discovery
/// completes on the MCP Host's own serialized domain and stages the tool here;
/// the Agent registers it on its own domain, at a turn boundary. A tool staged
/// after the session closed is dropped rather than drained, so a catalog that
/// arrives late can never re-register into a surface that is gone.
///
/// The value is shared between the MCP Host (which fills it), the session
/// (which drains it), the built-in meta-tools (which read the catalog and stage
/// activations), and the call-approval policy (which reads the reverse
/// mapping), so every access is under one mutex.
class McpToolBinding : public std::enable_shared_from_this<McpToolBinding> {
public:
    /// Adopt the configured servers' call-approval policies. A Server Id that
    /// is not configured — and every non-Qualified Tool Name — is treated as
    /// `allow`, so a built-in tool call is never blocked by this value.
    void set_servers(const std::vector<UserMcpServerSettings>& servers);

    /// Build the `agent::Tool` for one discovered descriptor and stage it.
    ///
    /// `connection` is the Upstream connection the tool calls through and is
    /// kept alive by the tool value, so a call can never reach a released
    /// connection. Re-publishing the same (Server Id, tool name) — a
    /// reconnect, or a catalog refresh — replaces the staged value rather than
    /// stacking a second one. A Qualified Tool Name already held by a
    /// *different* Upstream tool is rejected instead of overwritten, because
    /// `ToolRegistry::add` overwrites silently and a collision would silently
    /// retarget a live tool.
    [[nodiscard]] support::ExpectedVoid publish(
            const std::shared_ptr<mcp::UpstreamConnection>& connection, mcp::UpstreamToolDescriptor descriptor);

    /// The `lazy` half of discovery (issue #847): record one connected
    /// `lazy` Upstream's catalog so `mcp_search` can list it and `mcp_activate`
    /// can call it, **without** publishing any of its tools into the Agent's
    /// tool surface. A `lazy` tool is what keeps dozens of Upstreams out of the
    /// model context: its JSON Schema reaches a real request only after an
    /// activation, and never before.
    ///
    /// The two meta-tools are staged on the first call, which is what makes
    /// their registration condition "at least one connected `lazy` Upstream"
    /// rather than "at least one configured one". A catalog refresh replaces
    /// the stored descriptor and re-activates only what the transcript or the
    /// model already activated; a refresh never un-activates a live tool,
    /// because removing and re-adding a tool invalidates the provider's prompt
    /// cache (ADR 0064, spec #833 story 16).
    [[nodiscard]] support::ExpectedVoid record_lazy_catalog(
            const std::shared_ptr<mcp::UpstreamConnection>& connection, mcp::UpstreamCatalog catalog);

    /// The compact catalog rows `mcp_search` returns: Server Id, Qualified Tool
    /// Name, and the Upstream's one-line description, in name order. An empty
    /// `query` or `server_id` matches every catalogued tool. The row carries no
    /// schema — that is the whole context economy of the meta-tool.
    [[nodiscard]] std::vector<McpPublishedTool> search(std::string_view query, std::string_view server_id) const;

    /// The row of one catalogued `lazy` tool, or `std::nullopt` for a name no
    /// discovered catalog holds.
    [[nodiscard]] std::optional<McpPublishedTool> row_for(std::string_view qualified_name) const;

    /// Make one catalogued tool callable from the next turn boundary on, by
    /// staging its binding exactly as the eager publication path does. An
    /// already-active tool is `AlreadyActive`; a name no catalog holds is an
    /// error the meta-tool reports back to the model.
    ///
    /// Activation itself needs no approval: the call-approval policy is keyed
    /// by Qualified Tool Name, and this value publishes no row for the
    /// meta-tools, so `mcp_activate` is a built-in call like any other.
    /// Visibility is not trust; the gate is the call-time policy
    /// (spec #833 story 15, issue #843).
    [[nodiscard]] support::Expected<McpActivationOutcome> activate(std::string_view qualified_name);

    /// The Qualified Tool Names a resumed transcript records as active. A name
    /// whose catalog has not been discovered yet is remembered and staged the
    /// moment that catalog lands, which is how a resumed session restores its
    /// active loadout without ever waiting for discovery before the Agent is
    /// assembled (ADR 0066, spec #833 story 7).
    void set_restore_names(std::vector<std::string> qualified_names);

    /// The built-in meta-tools staged by the first `lazy` catalog, for the
    /// session to register on the Agent exactly like any other discovered
    /// tool. Empty once drained, and empty forever when no `lazy` Upstream has
    /// connected.
    [[nodiscard]] std::vector<agent::Tool> take_pending_meta_tools();

    /// One Upstream MCP Server's own usage guidance per Server Id that offered
    /// some, in Server Id order.
    [[nodiscard]] std::vector<McpServerInstructions> instructions() const;

    /// Take everything staged since the last drain, in discovery order.
    /// Empty once the binding is closed.
    [[nodiscard]] std::vector<McpToolPublication> take_pending();

    /// Every tool published so far, name-ordered by Qualified Tool Name.
    [[nodiscard]] std::vector<McpPublishedTool> published() const;

    /// The Server Id a Qualified Tool Name was published under, or `std::nullopt`
    /// for a name this binding never published — which is how a built-in tool
    /// name is told apart from an MCP one.
    [[nodiscard]] std::optional<std::string> server_id_for(std::string_view qualified_name) const;

    /// The reverse-mapping row for one published tool, or `std::nullopt` for a
    /// name this binding never published. The call-approval prompt reads it to
    /// show the Upstream's own tool name beside the Qualified Tool Name
    /// (issue #843).
    [[nodiscard]] std::optional<McpPublishedTool> publication_for(std::string_view qualified_name) const;

    /// The call-approval policy configured for one Server Id, `allow` when the
    /// Server Id is not configured.
    [[nodiscard]] McpServerApproval approval_for(std::string_view server_id) const;

    /// Stop accepting publications. Idempotent, and the whole of "a catalog
    /// that completes after the session closed registers nothing".
    void close() noexcept;

    [[nodiscard]] bool closed() const;

private:
    /// How far one catalogued `lazy` tool has travelled toward the model's
    /// tool payload.
    enum class Activation { Catalogued, Staged, Active };

    /// One `lazy` Upstream tool as the search catalog holds it: the row
    /// `mcp_search` renders, the descriptor and connection `mcp_activate` turns
    /// into a callable value, and how far that value has travelled.
    struct CatalogEntry {
        McpPublishedTool tool{};
        mcp::UpstreamToolDescriptor descriptor{};
        std::shared_ptr<mcp::UpstreamConnection> connection{nullptr};
        Activation activation{Activation::Catalogued};
    };

    /// The caller of `publish`, with the Qualified Tool Name already resolved.
    [[nodiscard]] support::ExpectedVoid stage(std::shared_ptr<mcp::UpstreamConnection> connection,
            mcp::UpstreamToolDescriptor descriptor,
            std::string qualified_name);

    /// Stage the callable binding of one catalogued tool. The caller holds
    /// `mutex_`.
    void stage_activation_locked(std::map<std::string, CatalogEntry, std::less<>>::iterator& entry);

    /// Reject a Qualified Tool Name another Upstream tool already holds, the way
    /// the eager path does. The caller holds `mutex_`.
    [[nodiscard]] support::ExpectedVoid refuse_name_collision_locked(
            const std::string& qualified_name, std::string_view tool_name) const;

    mutable std::mutex mutex_;
    /// Qualified Tool Name -> the row, for the reverse mapping every consumer
    /// but the executor reads.
    std::map<std::string, McpPublishedTool, std::less<>> published_;
    /// Qualified Tool Name -> the row most recently staged for it, so a
    /// republish is a replacement rather than a second pending entry.
    std::map<std::string, McpPublishedTool, std::less<>> staged_;
    /// Qualified Tool Name -> every `lazy` Upstream tool discovered so far,
    /// activated or not. The `mcp_search` catalog is this map.
    std::map<std::string, CatalogEntry, std::less<>> catalog_;
    /// Server Id -> that server's own usage guidance, for the System Prompt.
    std::map<std::string, std::string, std::less<>> instructions_;
    /// Qualified Tool Names a resumed transcript records as active, kept until
    /// the catalog that describes them is discovered (ADR 0066).
    std::vector<std::string> restore_names_;
    std::map<std::string, McpServerApproval, std::less<>> approvals_;
    std::vector<McpToolPublication> pending_;
    /// The meta-tools, staged once by the first `lazy` catalog and drained by
    /// the session at a turn boundary.
    std::vector<agent::Tool> pending_meta_tools_;
    bool meta_tools_staged_{false};
    bool closed_{false};
};

} // namespace cch::coding_agent::runtime
