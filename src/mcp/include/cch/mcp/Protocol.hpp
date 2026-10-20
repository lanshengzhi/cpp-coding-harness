#pragma once

// Transport-independent MCP protocol facts and `initialize` validation
// (`packages/mcp/src/protocol/types.ts`, `client.ts`). Both wire transports
// speak the same JSON-RPC handshake, so the constants and the handshake checks
// live here once; framing stays in the implementation.

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace cch::mcp {

/// The MCP protocol version Pike requests on a connection: pi
/// `LATEST_PROTOCOL_VERSION`, the version the frozen bundle records under
/// `protocol.latest`.
inline constexpr std::string_view kProtocolVersion = "2025-11-25";

/// pi `SUPPORTED_PROTOCOL_VERSIONS`: the negotiated `initialize` result must
/// select one of these, exactly like pi `client.ts`.
inline constexpr std::string_view kSupportedProtocolVersions[] = {
        "2025-11-25",
        "2025-06-18",
        "2025-03-26",
        "2024-11-05",
};

/// pi `MAX_LIST_PAGES`: the most `tools/list` pages one listing walk follows
/// before the server is rejected as unbounded.
inline constexpr int kMaxListPages = 1000;

/// pi `MAX_TOOL_NAME_LENGTH`: the longest Agent-visible MCP tool name before
/// the hash suffix rule applies. The application owns the naming rule; the
/// bound is part of the generic protocol value set.
inline constexpr std::size_t kMaxToolNameLength = 64;

/// Client identity sent in the `initialize` handshake.
inline constexpr std::string_view kClientName = "pike";
inline constexpr std::string_view kClientVersion = "0.1.0";

namespace detail {

/// The `initialize` parameters (pi `client.ts` `connect`): the client's
/// protocol version, capabilities (with roots when cwd is non-empty), and
/// identity. Shared by the initial handshake and a reconnect, and by both
/// transports.
[[nodiscard]] inline support::JsonValue initialize_params(bool with_roots = true) {
    support::JsonValue::object_t capabilities;
    if (with_roots) {
        capabilities["roots"] = support::JsonValue::object_t{};
    }
    return support::JsonValue{support::JsonValue::object_t{
            {"protocolVersion", std::string{kProtocolVersion}},
            {"capabilities", support::JsonValue{std::move(capabilities)}},
            {"clientInfo",
                    support::JsonValue::object_t{
                            {"name", std::string{kClientName}},
                            {"version", std::string{kClientVersion}},
                    }},
    }};
}

/// Validate the `initialize` result like pi `validateInitializeResult`: the
/// protocol version, capabilities object, and serverInfo identity must be
/// present, so a server that cannot speak MCP fails explicitly at connect.
[[nodiscard]] inline support::ExpectedVoid validate_initialize_result(
        const std::string& server, const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' sent an invalid initialize result"));
    }
    const auto protocol_version = object->find("protocolVersion");
    if (protocol_version == object->end() || !protocol_version->second.holds<std::string>()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' initialize result is missing protocolVersion"));
    }
    const auto capabilities = object->find("capabilities");
    if (capabilities == object->end() || capabilities->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' initialize result is missing capabilities"));
    }
    const auto server_info = object->find("serverInfo");
    if (server_info == object->end() || server_info->second.get_if<support::JsonValue::object_t>() == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server '" + server + "' initialize result is missing serverInfo"));
    }
    const auto* server_info_object = server_info->second.get_if<support::JsonValue::object_t>();
    const auto server_name = server_info_object->find("name");
    if (server_name == server_info_object->end() || !server_name->second.holds<std::string>()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' initialize result has a serverInfo without a string name"));
    }
    const auto server_version = server_info_object->find("version");
    if (server_version == server_info_object->end() || !server_version->second.holds<std::string>()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' initialize result has a serverInfo without a string version"));
    }
    const auto instructions = object->find("instructions");
    if (instructions != object->end() && !instructions->second.holds<std::string>()) {
        return std::unexpected(support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' initialize result has a non-string instructions"));
    }
    // pi `client.ts` connect: the negotiated version must be one pi speaks,
    // rejected with pi's verbatim message.
    const auto selected = protocol_version->second.get_string();
    const bool supported =
            std::ranges::any_of(kSupportedProtocolVersions, [&selected](std::string_view version) {
                return version == selected;
            });
    if (!supported) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "MCP server selected unsupported protocol version " + selected));
    }
    return {};
}

/// pi `validateInitializeResult`'s optional `instructions`: the server usage
/// guidance the application's prompt section falls back to. Absent unless the
/// initialize result carries a non-empty string.
[[nodiscard]] inline std::optional<std::string> initialize_instructions(const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto instructions = object->find("instructions");
    if (instructions == object->end() || !instructions->second.holds<std::string>() ||
            instructions->second.get_string().empty()) {
        return std::nullopt;
    }
    return instructions->second.get_string();
}

} // namespace detail

} // namespace cch::mcp
