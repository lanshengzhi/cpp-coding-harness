#include "mcp/EraAdapter.hpp"

#include "mcp/Protocol.hpp"
#include "mcp/WireDto.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::era {
namespace {

using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

/// The Modern Era (2026-07-28 and later): per-request `_meta` protocol
/// fields, `server/discover` instead of an `initialize` handshake, and Multi
/// Round-Trip `input_required` tool results.
class ModernEraAdapter final : public EraAdapter {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "modern"; }

    [[nodiscard]] std::string_view discover_method() const noexcept override { return protocol::kMethodDiscover; }

    [[nodiscard]] std::string_view list_tools_method() const noexcept override { return protocol::kMethodListTools; }

    [[nodiscard]] std::string_view call_tool_method() const noexcept override { return protocol::kMethodCallTool; }

    void attach_request_meta(JsonValue& params) const override { attach_modern_request_meta(params); }

    [[nodiscard]] std::map<std::string, std::string> request_headers(
            std::string_view method, std::string_view name) const override {
        return modern_request_headers(method, name);
    }

    [[nodiscard]] Expected<UpstreamServerInfo> read_probe_result(const JsonValue& result) const override {
        return dto::read_discover_result(result);
    }
};

} // namespace

void attach_modern_request_meta(JsonValue& params) {
    if (params.get_if<JsonValue::object_t>() == nullptr) {
        params = JsonValue::object_t{};
    }
    auto& object = params.get_object();
    auto& meta = object["_meta"];
    if (meta.get_if<JsonValue::object_t>() == nullptr) {
        meta = JsonValue::object_t{};
    }
    auto& meta_object = meta.get_object();
    // Both keys are reserved: they are rewritten, never merged, so nothing a
    // caller put in `params._meta` can ride along on the wire.
    meta_object[std::string(protocol::kMetaProtocolVersionKey)] = JsonValue(std::string(protocol::kProtocolVersion));
    meta_object[std::string(protocol::kMetaClientCapabilitiesKey)] = protocol::client_capabilities();
}

std::map<std::string, std::string> modern_request_headers(std::string_view method, std::string_view name) {
    std::map<std::string, std::string> headers{
            {std::string(protocol::kHeaderProtocolVersion), std::string(protocol::kProtocolVersion)},
            {std::string(protocol::kHeaderMethod), std::string(method)},
    };
    if (!name.empty()) {
        headers.emplace(std::string(protocol::kHeaderName), std::string(name));
    }
    return headers;
}

Expected<std::unique_ptr<EraAdapter>> select_era_adapter(const JsonValue& result) {
    const auto* declared = result.get_if<JsonValue::object_t>();
    if (declared == nullptr) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "the Upstream MCP Server probe returned no result object",
                "the era of an Upstream that cannot name its protocol revision is undetermined"));
    }
    const auto revision = declared->find(std::string(protocol::kResultProtocolVersion));
    if (revision == declared->end()) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "the Upstream MCP Server probe returned no protocol revision",
                "the result carries no \"protocolVersion\" member"));
    }
    const auto* text = revision->second.get_if<std::string>();
    if (text == nullptr) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "the Upstream MCP Server probe returned no protocol revision",
                "the result's \"protocolVersion\" member is not a string"));
    }
    if (*text != protocol::kProtocolVersion) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "the Upstream MCP Server speaks an unsupported protocol era",
                "the probed revision is \"" + *text + "\" and this build speaks \"" +
                        std::string(protocol::kProtocolVersion) + "\""));
    }
    return std::make_unique<ModernEraAdapter>();
}

} // namespace cch::mcp::era
