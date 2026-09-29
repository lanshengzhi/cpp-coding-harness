#pragma once

#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <map>
#include <string>

namespace cch::mcp::headers {

/// The `Mcp-Param-*` request headers a call's validated annotation requires.
///
/// The 2026-07-28 revision makes the mirroring mandatory for an annotated
/// parameter, so a required parameter with no argument is an error rather than
/// an omitted header. An argument explicitly set to JSON `null` is not
/// mirrored: absence is expressed by omitting the header, which is why the
/// contract is one-way and never decodes an arbitrary value back.
///
/// A value whose bytes are all printable US-ASCII and which does not begin
/// with the `base64:` sentinel is sent verbatim. Anything else — a control
/// byte, a non-ASCII byte, or a value that would otherwise be mistaken for
/// the sentinel — is sent as `base64:<payload>` so the encoding stays
/// unambiguous for the server (SEP-2243).
[[nodiscard]] cch::support::Expected<std::map<std::string, std::string>> mirror_parameter_headers(
        const cch::mcp::UpstreamToolDescriptor& tool, const cch::support::JsonValue& arguments);

} // namespace cch::mcp::headers
