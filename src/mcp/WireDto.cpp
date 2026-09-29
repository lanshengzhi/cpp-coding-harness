#include "mcp/WireDto.hpp"

#include "mcp/Protocol.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::mcp::dto {
namespace {

using support::Error;
using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

[[nodiscard]] const JsonValue* member(const JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string(key));
    return found == object->end() ? nullptr : &found->second;
}

[[nodiscard]] const JsonValue::object_t* as_object(const JsonValue& value) {
    return value.get_if<JsonValue::object_t>();
}

[[nodiscard]] const JsonValue::array_t* as_array(const JsonValue& value) { return value.get_if<JsonValue::array_t>(); }

[[nodiscard]] std::optional<std::string> as_string(const JsonValue& value) {
    const auto* text = value.get_if<std::string>();
    if (text == nullptr) {
        return std::nullopt;
    }
    return *text;
}

[[nodiscard]] std::optional<bool> as_boolean(const JsonValue& value) {
    const auto* flag = value.get_if<bool>();
    if (flag == nullptr) {
        return std::nullopt;
    }
    return *flag;
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

[[nodiscard]] bool is_json_null(const JsonValue& value) { return value.holds<JsonValue::null_t>(); }

[[nodiscard]] Error violation(std::string detail) {
    return make_error(ErrorCode::Validation, "the Upstream MCP Server returned an unusable result", std::move(detail));
}

/// The freshness hint an Upstream put on a `tools/list` result, as the
/// `ttlMs` member. A member that is not a finite non-negative number is a
/// violation like any other typed field: the host does not decode a hint it
/// cannot read, it ignores it. A hint longer than the host admits is clamped
/// rather than refused, and it is clamped *before* the integral conversion,
/// so a hint large enough to overflow the type is bounded and not undefined.
[[nodiscard]] Expected<std::chrono::milliseconds> read_freshness(const JsonValue& value) {
    const auto* number = value.get_if<double>();
    if (number == nullptr || !std::isfinite(*number) || *number < 0.0) {
        return std::unexpected(violation("a tools/list result has a non-numeric, negative, or non-finite \"" +
                                         std::string(protocol::kResultCatalogFreshness) + "\""));
    }
    const double ceiling = static_cast<double>(protocol::kMaxCatalogFreshness.count());
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::min(*number, ceiling))};
}

[[nodiscard]] Expected<std::string> require_string(
        const JsonValue& object, std::string_view key, std::string_view what) {
    const auto* raw = member(object, key);
    if (raw == nullptr) {
        return std::unexpected(violation(std::string(what) + " has no \"" + std::string(key) + "\" member"));
    }
    const auto text = as_string(*raw);
    if (!text.has_value() || text->empty()) {
        return std::unexpected(
                violation(std::string(what) + " has a non-string or empty \"" + std::string(key) + "\""));
    }
    return *text;
}

/// RFC 9110 field-name token: the only bytes a request header name may carry.
[[nodiscard]] bool is_field_name_token(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool tchar = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                           std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos;
        if (!tchar) {
            return false;
        }
    }
    return true;
}

/// A header an `x-mcp-header` annotation may not claim: the transport's own
/// reserved set, and the `Mcp-Param-` namespace the mirroring writes into.
[[nodiscard]] bool is_reserved_header_name(std::string_view name) {
    const std::string lowered(lower_ascii(name));
    return lowered == lower_ascii(protocol::kHeaderProtocolVersion) ||
           lowered == lower_ascii(protocol::kHeaderMethod) || lowered == lower_ascii(protocol::kHeaderName) ||
           lowered.starts_with(lower_ascii(protocol::kHeaderParamPrefix));
}

/// The tool-parameter names declared by a tool's `inputSchema`.
[[nodiscard]] std::vector<std::string> declared_parameter_names(const JsonValue& parameters) {
    std::vector<std::string> names;
    const auto* properties = member(parameters, "properties");
    const auto* object = properties == nullptr ? nullptr : as_object(*properties);
    if (object != nullptr) {
        names.reserve(object->size());
        for (const auto& [name, schema] : *object) {
            names.push_back(name);
        }
    }
    return names;
}

/// Validate the `x-mcp-header` annotation of one tool and read it.
///
/// The annotation is a JSON object whose keys are the tool's own parameter
/// names and whose values are objects carrying the mirrored header's `name`
/// and optional `description`, `required`, and `schema`. A violation of any
/// rule rejects the whole tool, because a partially mirrored call would
/// silently drop a header the Upstream requires.
[[nodiscard]] Expected<std::vector<UpstreamHeaderParameter>> read_header_annotation(
        const std::string& tool_name, const JsonValue& annotations, const JsonValue& parameters) {
    const auto* raw = member(annotations, protocol::kAnnotationHeaderParams);
    if (raw == nullptr) {
        return std::vector<UpstreamHeaderParameter>{};
    }
    const auto* declared = as_object(*raw);
    if (declared == nullptr) {
        return std::unexpected(violation("tool \"" + tool_name + "\" has a non-object \"" +
                                         std::string(protocol::kAnnotationHeaderParams) + "\" annotation"));
    }

    const auto parameter_names = declared_parameter_names(parameters);
    std::vector<UpstreamHeaderParameter> parameters_out;
    parameters_out.reserve(declared->size());
    for (const auto& [argument_name, descriptor] : *declared) {
        const auto* fields = as_object(descriptor);
        if (fields == nullptr) {
            return std::unexpected(
                    violation("tool \"" + tool_name + "\" parameter \"" + argument_name + "\" has a non-object \"" +
                              std::string(protocol::kAnnotationHeaderParams) + "\" descriptor"));
        }
        if (std::find(parameter_names.begin(), parameter_names.end(), argument_name) == parameter_names.end()) {
            return std::unexpected(
                    violation("tool \"" + tool_name + "\" mirrors undeclared parameter \"" + argument_name + "\""));
        }
        auto header_name = require_string(
                descriptor, "name", "tool \"" + tool_name + "\" parameter \"" + argument_name + "\" annotation");
        if (!header_name) {
            return std::unexpected(header_name.error());
        }
        if (!is_field_name_token(*header_name) || is_reserved_header_name(*header_name)) {
            return std::unexpected(violation("tool \"" + tool_name + "\" parameter \"" + argument_name +
                                             "\" claims the reserved or malformed header name"));
        }
        const std::string lowered(lower_ascii(*header_name));
        const auto duplicate = std::any_of(parameters_out.begin(),
                parameters_out.end(),
                [&lowered](const auto& existing) { return lower_ascii(existing.header_name) == lowered; });
        if (duplicate) {
            return std::unexpected(violation("tool \"" + tool_name + "\" mirrors one header name twice"));
        }

        UpstreamHeaderParameter parameter;
        parameter.argument_name = argument_name;
        parameter.header_name = std::move(*header_name);
        if (const auto* raw_description = member(descriptor, "description"); raw_description != nullptr) {
            const auto description = as_string(*raw_description);
            if (!description.has_value()) {
                return std::unexpected(violation("tool \"" + tool_name + "\" parameter \"" + argument_name +
                                                 "\" has a non-string \"description\""));
            }
            parameter.description = std::move(*description);
        }
        if (const auto* raw_required = member(descriptor, "required"); raw_required != nullptr) {
            const auto required = as_boolean(*raw_required);
            if (!required.has_value()) {
                return std::unexpected(violation("tool \"" + tool_name + "\" parameter \"" + argument_name +
                                                 "\" has a non-boolean \"required\""));
            }
            parameter.required = *required;
        }
        if (const auto* raw_schema = member(descriptor, "schema"); raw_schema != nullptr) {
            if (as_object(*raw_schema) == nullptr) {
                return std::unexpected(violation(
                        "tool \"" + tool_name + "\" parameter \"" + argument_name + "\" has a non-object \"schema\""));
            }
            parameter.schema = *raw_schema;
        }
        parameters_out.push_back(std::move(parameter));
    }
    return parameters_out;
}

[[nodiscard]] Expected<ToolListEntry> read_tool(const JsonValue& value) {
    if (as_object(value) == nullptr) {
        return std::unexpected(violation("a tools/list entry is not a JSON object"));
    }
    UpstreamToolDescriptor tool;
    auto name = require_string(value, "name", "a tools/list entry");
    if (!name) {
        return std::unexpected(name.error());
    }
    tool.name = std::move(*name);
    if (const auto* raw_description = member(value, "description"); raw_description != nullptr) {
        const auto description = as_string(*raw_description);
        if (!description.has_value()) {
            return std::unexpected(violation("tool \"" + tool.name + "\" has a non-string \"description\""));
        }
        tool.description = std::move(*description);
    }
    if (const auto* raw_parameters = member(value, "inputSchema"); raw_parameters != nullptr) {
        if (as_object(*raw_parameters) == nullptr) {
            return std::unexpected(violation("tool \"" + tool.name + "\" has a non-object \"inputSchema\""));
        }
        tool.parameters = *raw_parameters;
    }
    if (const auto* raw_annotations = member(value, "annotations"); raw_annotations != nullptr) {
        if (as_object(*raw_annotations) == nullptr) {
            return std::unexpected(violation("tool \"" + tool.name + "\" has non-object \"annotations\""));
        }
        auto headers = read_header_annotation(tool.name, *raw_annotations, tool.parameters);
        if (!headers) {
            return ToolListEntry{.tool = std::nullopt, .rejection = headers.error().detail};
        }
        tool.header_parameters = std::move(*headers);
    }
    return ToolListEntry{.tool = std::move(tool), .rejection = {}};
}

/// Read the declared server capabilities of a `server/discover` result,
/// failing on any capability this build does not implement.
[[nodiscard]] Expected<std::vector<std::string>> read_capabilities(const JsonValue& capabilities) {
    const auto* declared = as_object(capabilities);
    if (declared == nullptr) {
        return std::unexpected(violation("a server/discover result has a non-object \"capabilities\" member"));
    }
    std::vector<std::string> names;
    names.reserve(declared->size());
    for (const auto& [name, value] : *declared) {
        if (name != protocol::kServerCapabilityTools) {
            return std::unexpected(
                    violation("a server/discover result declares the unsupported capability \"" + name + "\""));
        }
        const auto* fields = as_object(value);
        if (fields == nullptr) {
            return std::unexpected(violation("a server/discover result has a non-object \"tools\" capability"));
        }
        if (const auto* raw_list_changed = member(value, "listChanged"); raw_list_changed != nullptr) {
            if (!as_boolean(*raw_list_changed).has_value()) {
                return std::unexpected(
                        violation("a server/discover result has a non-boolean \"tools.listChanged\" capability"));
            }
        }
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

Expected<UpstreamServerInfo> read_discover_result(const JsonValue& result) {
    if (as_object(result) == nullptr) {
        return std::unexpected(violation("a server/discover result is not a JSON object"));
    }
    UpstreamServerInfo info;
    auto version = require_string(result, protocol::kResultProtocolVersion, "a server/discover result");
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != protocol::kProtocolVersion) {
        return std::unexpected(
                violation("a server/discover result speaks the unsupported protocol revision \"" + *version + "\""));
    }
    const auto* server_info = member(result, "serverInfo");
    if (server_info == nullptr || as_object(*server_info) == nullptr) {
        return std::unexpected(violation("a server/discover result has no object \"serverInfo\" member"));
    }
    auto name = require_string(*server_info, "name", "a server/discover \"serverInfo\"");
    if (!name) {
        return std::unexpected(name.error());
    }
    info.name = std::move(*name);
    auto release = require_string(*server_info, "version", "a server/discover \"serverInfo\"");
    if (!release) {
        return std::unexpected(release.error());
    }
    info.version = std::move(*release);

    const auto* raw_capabilities = member(result, "capabilities");
    if (raw_capabilities == nullptr) {
        return std::unexpected(violation("a server/discover result has no \"capabilities\" member"));
    }
    auto capabilities = read_capabilities(*raw_capabilities);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    info.capabilities = std::move(*capabilities);

    if (const auto* raw_instructions = member(result, "instructions"); raw_instructions != nullptr) {
        const auto instructions = as_string(*raw_instructions);
        if (!instructions.has_value()) {
            return std::unexpected(violation("a server/discover result has non-string \"instructions\""));
        }
        info.instructions = std::move(*instructions);
    }
    return info;
}

Expected<ToolListPage> read_tool_list_page(const JsonValue& result) {
    if (as_object(result) == nullptr) {
        return std::unexpected(violation("a tools/list result is not a JSON object"));
    }
    const auto* raw_tools = member(result, "tools");
    const auto* tools = raw_tools == nullptr ? nullptr : as_array(*raw_tools);
    if (tools == nullptr) {
        return std::unexpected(violation("a tools/list result has no array \"tools\" member"));
    }
    ToolListPage page;
    page.tools.reserve(tools->size());
    for (const auto& entry : *tools) {
        auto read = read_tool(entry);
        if (!read) {
            return std::unexpected(read.error());
        }
        if (!read->tool.has_value()) {
            continue; // an invalid x-mcp-header annotation drops this tool alone
        }
        page.tools.push_back(std::move(*read->tool));
    }
    if (const auto* raw_cursor = member(result, "nextCursor"); raw_cursor != nullptr && !is_json_null(*raw_cursor)) {
        const auto cursor = as_string(*raw_cursor);
        if (!cursor.has_value() || cursor->empty()) {
            return std::unexpected(violation("a tools/list result has a non-string or empty \"nextCursor\""));
        }
        page.next_cursor = *cursor;
    }
    if (const auto* raw_freshness = member(result, protocol::kResultCatalogFreshness);
            raw_freshness != nullptr && !is_json_null(*raw_freshness)) {
        auto freshness = read_freshness(*raw_freshness);
        if (!freshness) {
            return std::unexpected(freshness.error());
        }
        page.freshness = *freshness;
    }
    if (const auto* raw_scope = member(result, protocol::kResultCatalogCacheScope);
            raw_scope != nullptr && !is_json_null(*raw_scope)) {
        const auto scope = as_string(*raw_scope);
        if (!scope.has_value() || scope->empty()) {
            return std::unexpected(violation("a tools/list result has a non-string or empty \"" +
                                             std::string(protocol::kResultCatalogCacheScope) + "\""));
        }
        page.cache_scope = std::move(*scope);
    }
    return page;
}

Expected<ToolCallOutcome> read_tool_call_result(const JsonValue& result) {
    if (as_object(result) == nullptr) {
        return std::unexpected(violation("a tools/call result is not a JSON object"));
    }
    const auto* raw_result_type = member(result, "resultType");
    if (raw_result_type == nullptr) {
        return std::unexpected(violation("a tools/call result has no \"resultType\" member"));
    }
    const auto result_type = as_string(*raw_result_type);
    if (!result_type.has_value()) {
        return std::unexpected(violation("a tools/call result has a non-string \"resultType\""));
    }
    if (*result_type == protocol::kResultTypeInputRequired) {
        return std::unexpected(make_error(ErrorCode::Validation,
                "an unexpected Multi Round-Trip \"input_required\" tool result",
                "the Upstream MCP Server asked for client input on a call this build drives without a Multi "
                "Round-Trip loop"));
    }
    if (*result_type != protocol::kResultTypeCallResult) {
        return std::unexpected(
                violation("a tools/call result has the unrecognized \"resultType\" \"" + *result_type + "\""));
    }
    ToolCallOutcome outcome;
    if (const auto* raw_content = member(result, "content"); raw_content != nullptr) {
        outcome.content = *raw_content;
    }
    if (const auto* raw_is_error = member(result, "isError"); raw_is_error != nullptr) {
        const auto is_error = as_boolean(*raw_is_error);
        if (!is_error.has_value()) {
            return std::unexpected(violation("a tools/call result has a non-boolean \"isError\""));
        }
        outcome.is_error = *is_error;
    }
    return outcome;
}

} // namespace cch::mcp::dto
