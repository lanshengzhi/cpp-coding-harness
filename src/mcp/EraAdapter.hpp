#pragma once

#include <cch/mcp/UpstreamServer.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace cch::mcp::era {

/// The Modern Era request shape, as free functions so the era probe — the one
/// exchange that runs before a connection has selected an adapter — frames
/// itself exactly like every request after it. Every request carries the
/// reserved `io.modelcontextprotocol/*` `_meta` pair and the `Mcp-Method`
/// header (spec #833, "Wire and transport").
void attach_modern_request_meta(cch::support::JsonValue& params);

/// `name` is the Upstream resource a method addresses, or empty for a method
/// that addresses none.
[[nodiscard]] std::map<std::string, std::string> modern_request_headers(std::string_view method, std::string_view name);

/// The era seam (ADR 0064). One adapter is selected per connection by probing
/// the Upstream MCP Server, and every later request on that connection is
/// framed by the selected adapter. v1 ships only the Modern Era adapter; the
/// deferred Legacy Era adapter is a second implementation of this same
/// interface, bounded to `initialize` + `tools/list` + `tools/call` + ignoring
/// notifications and declaring no client capabilities.
class EraAdapter {
public:
    virtual ~EraAdapter() = default;

    /// The era's product name, used in diagnostics and status reporting.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    [[nodiscard]] virtual std::string_view discover_method() const noexcept = 0;
    [[nodiscard]] virtual std::string_view list_tools_method() const noexcept = 0;
    [[nodiscard]] virtual std::string_view call_tool_method() const noexcept = 0;

    /// Attach this era's per-request protocol metadata to `params`. The Modern
    /// Era carries the reserved `io.modelcontextprotocol/*` `_meta` keys and no
    /// initialize handshake; a legacy adapter would attach nothing.
    virtual void attach_request_meta(cch::support::JsonValue& params) const = 0;

    /// The Streamable HTTP request headers this era requires beyond the common
    /// set. `name` is the Upstream resource a method addresses, or empty for
    /// a method that addresses none.
    [[nodiscard]] virtual std::map<std::string, std::string> request_headers(
            std::string_view method, std::string_view name) const = 0;

    /// Read a probe result into the connection's server information, failing
    /// on a revision, a required field, or a declared capability set this era
    /// does not implement.
    [[nodiscard]] virtual cch::support::Expected<cch::mcp::UpstreamServerInfo> read_probe_result(
            const cch::support::JsonValue& result) const = 0;
};

/// Select the era adapter for a probed Upstream MCP Server. The probe result
/// is validated far enough to name its revision; a revision this build does
/// not speak is rejected rather than connected to optimistically.
[[nodiscard]] cch::support::Expected<std::unique_ptr<EraAdapter>> select_era_adapter(
        const cch::support::JsonValue& result);

} // namespace cch::mcp::era
