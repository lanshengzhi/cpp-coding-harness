#pragma once

#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <map>
#include <string>
#include <string_view>

namespace cch::mcp::headers {

/// Standard base64 with padding. The MCP Host only ever encodes: a value it
/// encodes is the Upstream's to decode.
[[nodiscard]] std::string encode_base64(std::string_view bytes);

/// The wire spelling of one MCP request-header value derived from a tool
/// argument (SEP-2243).
///
/// A value whose bytes are all printable US-ASCII and which does not begin
/// with the `base64:` sentinel is sent verbatim. Anything else — a control
/// byte, a non-ASCII byte, or a value that would otherwise be mistaken for
/// the sentinel — is sent as `base64:<payload>`, so the sentinel stays
/// unambiguous for the Upstream.
[[nodiscard]] std::string to_header_value(std::string_view value);

/// The `Mcp-Param-*` request headers a call's validated annotation requires.
///
/// The 2026-07-28 revision makes the mirroring mandatory for an annotated
/// parameter, so a required parameter with no argument is an error rather than
/// an omitted header. An argument explicitly set to JSON `null` is not
/// mirrored: absence is expressed by omitting the header, which is why the
/// contract is one-way and never decodes an arbitrary value back. Each
/// mirrored value reaches the wire through `to_header_value`.
[[nodiscard]] cch::support::Expected<std::map<std::string, std::string>> mirror_parameter_headers(
        const cch::mcp::UpstreamToolDescriptor& tool, const cch::support::JsonValue& arguments);

} // namespace cch::mcp::headers
