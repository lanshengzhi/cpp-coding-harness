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
/// (which drains it) and the call-approval policy (which reads the reverse
/// mapping), so every access is under one mutex.
class McpToolBinding {
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

    /// Take everything staged since the last drain, in discovery order.
    /// Empty once the binding is closed.
    [[nodiscard]] std::vector<McpToolPublication> take_pending();

    /// Every tool published so far, name-ordered by Qualified Tool Name.
    [[nodiscard]] std::vector<McpPublishedTool> published() const;

    /// The Server Id a Qualified Tool Name was published under, or `std::nullopt`
    /// for a name this binding never published — which is how a built-in tool
    /// name is told apart from an MCP one.
    [[nodiscard]] std::optional<std::string> server_id_for(std::string_view qualified_name) const;

    /// The call-approval policy configured for one Server Id, `allow` when the
    /// Server Id is not configured.
    [[nodiscard]] McpServerApproval approval_for(std::string_view server_id) const;

    /// Stop accepting publications. Idempotent, and the whole of "a catalog
    /// that completes after the session closed registers nothing".
    void close() noexcept;

    [[nodiscard]] bool closed() const;

private:
    /// The caller of `publish`, with the Qualified Tool Name already resolved.
    [[nodiscard]] support::ExpectedVoid stage(std::shared_ptr<mcp::UpstreamConnection> connection,
            mcp::UpstreamToolDescriptor descriptor,
            std::string qualified_name);

    mutable std::mutex mutex_;
    /// Qualified Tool Name -> the row, for the reverse mapping every consumer
    /// but the executor reads.
    std::map<std::string, McpPublishedTool, std::less<>> published_;
    /// Qualified Tool Name -> the row most recently staged for it, so a
    /// republish is a replacement rather than a second pending entry.
    std::map<std::string, McpPublishedTool, std::less<>> staged_;
    std::map<std::string, McpServerApproval, std::less<>> approvals_;
    std::vector<McpToolPublication> pending_;
    bool closed_{false};
};

} // namespace cch::coding_agent::runtime
