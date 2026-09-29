#pragma once

#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamTool.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cch::mcp::dto {

/// One decoded `tools/list` entry: either the tool the catalog admits, or the
/// reason the catalog rejects it. A rejected tool is dropped on its own, so a
/// single bad annotation cannot cost a server its whole catalog; a tool that
/// cannot even be named fails the page instead, because there is nothing to
/// skip cleanly.
struct ToolListEntry {
    std::optional<cch::mcp::UpstreamToolDescriptor> tool{};
    std::string rejection{};
};

/// One decoded `tools/list` page.
struct ToolListPage {
    std::vector<cch::mcp::UpstreamToolDescriptor> tools{};
    std::optional<std::string> next_cursor{};
};

/// One decoded `tools/call` result carrying the server's own outcome.
struct ToolCallOutcome {
    bool is_error{false};
    cch::support::JsonValue content{};
};

// The wire DTOs of the released 2026-07-28 revision that this build decodes.
// The decoders are the contract the tests snapshot, so implementation and
// golden cannot drift wrong together. Each is fail-closed: a required field
// that is missing or of the wrong type fails the decode rather than decoding
// to a default, and no diagnostic ever echoes an Upstream-supplied value that
// could carry a credential.

/// Decode a `server/discover` result. The declared capability set is checked
/// against the capabilities this build implements, so an unrecognized
/// capability fails the probe.
[[nodiscard]] cch::support::Expected<cch::mcp::UpstreamServerInfo> read_discover_result(
        const cch::support::JsonValue& result);

/// Decode one `tools/list` page. A tool that cannot even be named fails the
/// page; a tool whose `x-mcp-header` annotation is invalid is rejected on its
/// own and never reaches the catalog, so it can never be registered or called.
[[nodiscard]] cch::support::Expected<ToolListPage> read_tool_list_page(const cch::support::JsonValue& result);

/// Decode a `tools/call` result. An absent or unrecognized `resultType` and
/// an `input_required` Multi Round-Trip result both fail the decode, which the
/// client stack turns into exactly one failed tool call.
[[nodiscard]] cch::support::Expected<ToolCallOutcome> read_tool_call_result(const cch::support::JsonValue& result);

} // namespace cch::mcp::dto
