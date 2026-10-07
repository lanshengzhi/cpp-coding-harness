// MCP resource tools (spec #882, ticket #884): the Codex-compatible
// `list_mcp_resources`, `list_mcp_resource_templates`, and `read_mcp_resource`
// tools. A port of pi v1.0.4 `extensions/mcp/resources.ts` over the
// transport-independent `McpResourceServer` seam, so the request wire
// (`resources/list`, `resources/templates/list`, `resources/read`) and the JSON
// result shapes are identical regardless of transport.

#include "coding_agent/mcp/McpResourceTools.hpp"

#include <cch/ai/Content.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

using extensions::ExtensionTool;
using extensions::ExtensionToolResult;

[[nodiscard]] support::Error error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Validation, std::move(message), std::move(detail));
}

/// pi `stringArgument`: a trimmed string argument, `std::nullopt` for an absent
/// or empty value, and an error when a present value is not a string.
[[nodiscard]] support::Expected<std::optional<std::string>> string_argument(
        const support::JsonValue& params, std::string_view key) {
    const auto* object = params.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::optional<std::string>{};
    }
    const auto entry = object->find(std::string{key});
    if (entry == object->end() || entry->second.holds<support::JsonValue::null_t>()) {
        return std::optional<std::string>{};
    }
    if (!entry->second.holds<std::string>()) {
        return std::unexpected(error(std::string{key} + " must be a string"));
    }
    std::string value = entry->second.get_string();
    const auto not_space = [](unsigned char character) { return std::isspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    if (value.empty()) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{std::move(value)};
}

/// pi `isMcpAppResource`'s `profile=mcp-app` test: the case-insensitive
/// `;\s*profile\s*=\s*"?mcp-app"?` pattern, without a regex.
[[nodiscard]] bool has_mcp_app_profile(std::string_view mime_type) {
    std::string lowered;
    lowered.reserve(mime_type.size());
    for (const char character : mime_type) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    const auto skip_space = [&lowered](std::size_t position) {
        while (position < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[position])) != 0) {
            ++position;
        }
        return position;
    };
    for (std::size_t semicolon = lowered.find(';'); semicolon != std::string::npos;
            semicolon = lowered.find(';', semicolon + 1)) {
        std::size_t position = skip_space(semicolon + 1);
        if (lowered.compare(position, 7, "profile") != 0) {
            continue;
        }
        position = skip_space(position + 7);
        if (position >= lowered.size() || lowered[position] != '=') {
            continue;
        }
        position = skip_space(position + 1);
        if (position < lowered.size() && lowered[position] == '"') {
            ++position;
        }
        if (lowered.compare(position, 7, "mcp-app") == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const support::JsonValue::object_t* as_object(const support::JsonValue& value) {
    return value.get_if<support::JsonValue::object_t>();
}

[[nodiscard]] const std::string* string_field(const support::JsonValue::object_t& object, std::string_view key) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end() || !entry->second.holds<std::string>()) {
        return nullptr;
    }
    return &entry->second.get_string();
}

/// A listed resource or template without `_meta` and icons, tagged with its
/// server (pi `listed`).
[[nodiscard]] support::JsonValue listed(const std::string& server, const support::JsonValue& item) {
    support::JsonValue result{support::JsonValue::object_t{}};
    auto& object = result.get_object();
    if (const auto* source = as_object(item)) {
        for (const auto& [key, value] : *source) {
            if (key == "_meta" || key == "icons") {
                continue;
            }
            object.emplace(key, value);
        }
    }
    object.emplace("server", server);
    return result;
}

[[nodiscard]] std::string invalid_list_result(std::string_view key) {
    return "MCP server sent an invalid " + std::string{key} + " result";
}

/// The `key` array of a list page, or an error when the page is malformed.
[[nodiscard]] support::Expected<const support::JsonValue::array_t*> list_items(
        const support::JsonValue& page, std::string_view key) {
    const auto* object = as_object(page);
    if (object == nullptr) {
        return std::unexpected(error(invalid_list_result(key)));
    }
    const auto entry = object->find(std::string{key});
    if (entry == object->end() || entry->second.get_if<support::JsonValue::array_t>() == nullptr) {
        return std::unexpected(error(invalid_list_result(key)));
    }
    return entry->second.get_if<support::JsonValue::array_t>();
}

/// pi's client treats a null or empty `nextCursor` as absent.
[[nodiscard]] std::optional<std::string> next_cursor(const support::JsonValue& page) {
    const auto* object = as_object(page);
    if (object == nullptr) {
        return std::nullopt;
    }
    const auto* cursor = string_field(*object, "nextCursor");
    if (cursor == nullptr || cursor->empty()) {
        return std::nullopt;
    }
    return *cursor;
}

/// pi `withoutTemplates`: a server that does not implement
/// `resources/templates/list` answers JSON-RPC `-32601` (method not found),
/// which is an empty template list rather than a failed listing. There is no
/// dedicated error discriminator on the shared error channel, so the detail
/// `json_rpc_error` writes is the signal.
[[nodiscard]] bool is_method_not_found(const support::Error& failure) {
    return failure.detail.starts_with("code -32601");
}

/// `resources/read` contents without `_meta`, so the tool's JSON carries only
/// the resource facts.
[[nodiscard]] support::Expected<std::vector<support::JsonValue>> stripped_contents(const support::JsonValue& result) {
    const auto* object = as_object(result);
    if (object == nullptr) {
        return std::unexpected(error("MCP server sent an invalid resources/read result"));
    }
    const auto entry = object->find("contents");
    if (entry == object->end() || entry->second.get_if<support::JsonValue::array_t>() == nullptr) {
        return std::unexpected(error("MCP server sent a resources/read result without contents"));
    }
    std::vector<support::JsonValue> contents;
    for (const auto& content : entry->second.get_array()) {
        support::JsonValue stripped{support::JsonValue::object_t{}};
        if (const auto* source = as_object(content)) {
            for (const auto& [key, value] : *source) {
                if (key == "_meta") {
                    continue;
                }
                stripped.get_object().emplace(key, value);
            }
        }
        contents.push_back(std::move(stripped));
    }
    return contents;
}

[[nodiscard]] ExtensionToolResult json_result(support::JsonValue payload) {
    ExtensionToolResult result;
    auto serialized = support::write_json(payload);
    result.content.push_back(ai::text_content(serialized ? std::move(*serialized) : std::string{"{}"}));
    result.details = std::move(payload);
    return result;
}

/// pi `findServer`: the named resource-bearing server, or pi's verbatim error
/// naming every server the tools can reach.
[[nodiscard]] support::Expected<McpResourceServer*> find_server(
        const std::vector<std::shared_ptr<McpResourceServer>>& servers, const std::string& name) {
    for (const auto& server : servers) {
        if (server->name() == name) {
            return server.get();
        }
    }
    std::string available;
    for (const auto& server : servers) {
        if (!available.empty()) {
            available += ", ";
        }
        available += server->name();
    }
    std::string message = "MCP server \"" + name + "\" has no resources";
    if (!available.empty()) {
        message += ". Servers with resources: " + available;
    }
    return std::unexpected(error(std::move(message)));
}

[[nodiscard]] support::AsyncResult<support::JsonValue> page_of(
        McpResourceServer& server, bool templates, std::optional<std::string> cursor, std::stop_token stop_token) {
    if (templates) {
        return server.resource_templates_page(std::move(cursor), stop_token);
    }
    return server.resources_page(std::move(cursor), stop_token);
}

/// Every page of one server's resources or templates (pi `allResources` /
/// `allResourceTemplates`), so the no-server listing covers all pages.
[[nodiscard]] boost::asio::awaitable<support::Expected<std::vector<support::JsonValue>>> collect_pages(
        McpResourceServer& server, bool templates, std::stop_token stop_token) {
    const std::string_view key = templates ? "resourceTemplates" : "resources";
    std::vector<support::JsonValue> items;
    std::optional<std::string> cursor;
    for (;;) {
        auto page = co_await support::detail::await_async_result(page_of(server, templates, cursor, stop_token));
        if (!page) {
            co_return std::unexpected(std::move(page.error()));
        }
        auto list = list_items(*page, key);
        if (!list) {
            co_return std::unexpected(std::move(list.error()));
        }
        for (const auto& item : **list) {
            items.push_back(item);
        }
        auto next = next_cursor(*page);
        if (!next) {
            break;
        }
        cursor = std::move(next);
    }
    co_return items;
}

[[nodiscard]] boost::asio::awaitable<support::Expected<ExtensionToolResult>> run_list(
        std::vector<std::shared_ptr<McpResourceServer>> servers,
        support::JsonValue arguments,
        bool templates,
        std::stop_token stop_token) {
    const std::string key = templates ? "resourceTemplates" : "resources";
    auto server_name = string_argument(arguments, "server");
    if (!server_name) {
        co_return std::unexpected(std::move(server_name.error()));
    }
    auto cursor = string_argument(arguments, "cursor");
    if (!cursor) {
        co_return std::unexpected(std::move(cursor.error()));
    }

    support::JsonValue payload{support::JsonValue::object_t{}};
    auto& payload_object = payload.get_object();

    if (server_name->has_value()) {
        auto server = find_server(servers, **server_name);
        if (!server) {
            co_return std::unexpected(std::move(server.error()));
        }
        auto page = co_await support::detail::await_async_result(
                page_of(**server, templates, std::move(*cursor), stop_token));
        if (!page) {
            co_return std::unexpected(std::move(page.error()));
        }
        auto list = list_items(*page, key);
        if (!list) {
            co_return std::unexpected(std::move(list.error()));
        }
        support::JsonValue::array_t items;
        for (const auto& item : **list) {
            if (is_mcp_app_resource(item)) {
                continue;
            }
            items.push_back(listed((*server)->name(), item));
        }
        payload_object.emplace("server", (*server)->name());
        payload_object.emplace(key, std::move(items));
        if (auto next = next_cursor(*page)) {
            payload_object.emplace("nextCursor", std::move(*next));
        }
        co_return json_result(std::move(payload));
    }

    if (cursor->has_value()) {
        co_return std::unexpected(error("cursor can only be used when a server is specified"));
    }

    std::sort(servers.begin(), servers.end(),
            [](const std::shared_ptr<McpResourceServer>& left, const std::shared_ptr<McpResourceServer>& right) {
                return left->name() < right->name();
            });
    support::JsonValue::array_t items;
    support::JsonValue::array_t errors;
    for (const auto& server : servers) {
        auto collected = co_await collect_pages(*server, templates, stop_token);
        if (!collected) {
            errors.emplace_back(support::JsonValue{support::JsonValue::object_t{
                    {"server", server->name()},
                    {"error", collected.error().message},
            }});
            continue;
        }
        for (const auto& item : *collected) {
            if (is_mcp_app_resource(item)) {
                continue;
            }
            items.push_back(listed(server->name(), item));
        }
    }
    payload_object.emplace(key, std::move(items));
    if (!errors.empty()) {
        payload_object.emplace("errors", std::move(errors));
    }
    co_return json_result(std::move(payload));
}

[[nodiscard]] boost::asio::awaitable<support::Expected<ExtensionToolResult>> run_read(
        std::vector<std::shared_ptr<McpResourceServer>> servers,
        support::JsonValue arguments,
        std::stop_token stop_token) {
    auto server_name = string_argument(arguments, "server");
    if (!server_name) {
        co_return std::unexpected(std::move(server_name.error()));
    }
    if (!server_name->has_value()) {
        co_return std::unexpected(error("server must be provided"));
    }
    auto uri = string_argument(arguments, "uri");
    if (!uri) {
        co_return std::unexpected(std::move(uri.error()));
    }
    if (!uri->has_value()) {
        co_return std::unexpected(error("uri must be provided"));
    }
    auto server = find_server(servers, **server_name);
    if (!server) {
        co_return std::unexpected(std::move(server.error()));
    }
    auto result = co_await support::detail::await_async_result((*server)->read_resource(**uri, stop_token));
    if (!result) {
        co_return std::unexpected(std::move(result.error()));
    }
    auto contents = stripped_contents(*result);
    if (!contents) {
        co_return std::unexpected(std::move(contents.error()));
    }

    support::JsonValue payload{support::JsonValue::object_t{
            {"server", (*server)->name()},
            {"uri", **uri},
            {"contents", std::move(*contents)},
    }};

    ExtensionToolResult outcome;
    const auto& rendered = payload.get_object().at("contents").get_array();
    if (rendered.empty()) {
        outcome.content.push_back(ai::text_content("Resource " + **uri + " is empty."));
    }
    for (const auto& content : rendered) {
        const auto* object = as_object(content);
        if (object == nullptr) {
            continue;
        }
        const std::string* content_uri = string_field(*object, "uri");
        const std::string* mime_type = string_field(*object, "mimeType");
        const std::string* text = string_field(*object, "text");
        const std::string* blob = string_field(*object, "blob");
        if (rendered.size() > 1) {
            outcome.content.push_back(ai::text_content((content_uri != nullptr ? *content_uri : std::string{}) + ":"));
        }
        if (text != nullptr) {
            outcome.content.push_back(ai::text_content(*text));
        } else if (blob != nullptr && mime_type != nullptr && mime_type->starts_with("image/")) {
            outcome.content.push_back(ai::image_content(*blob, *mime_type));
        } else if (blob != nullptr) {
            const std::string kind = (mime_type != nullptr && !mime_type->empty()) ? *mime_type : "binary";
            outcome.content.push_back(ai::text_content("[Binary resource " +
                                                       (content_uri != nullptr ? *content_uri : std::string{}) + " (" + kind +
                                                       ") not shown inline; its base64 is in the result details]"));
        }
    }
    outcome.details = std::move(payload);
    co_return outcome;
}

[[nodiscard]] support::JsonValue string_property(std::string description) {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "string"},
            {"description", std::move(description)},
    }};
}

[[nodiscard]] support::JsonValue optional_string_property() {
    return support::JsonValue{support::JsonValue::object_t{{"type", "string"}}};
}

/// pi `LIST_PARAMETERS`.
[[nodiscard]] support::JsonValue list_parameters() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"server",
                                    string_property(
                                            "MCP server name. Omit to list every server with resources.")},
                            {"cursor",
                                    string_property("Opaque cursor from a previous call with the same server; omit for "
                                                    "the first page.")},
                    }},
            {"additionalProperties", false},
    }};
}

/// pi `READ_PARAMETERS`.
[[nodiscard]] support::JsonValue read_parameters() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"server",
                                    string_property("MCP server name exactly as configured. Must match the 'server' "
                                                    "field returned by list_mcp_resources.")},
                            {"uri",
                                    string_property(
                                            "Resource URI to read. Must be one of the URIs returned by "
                                            "list_mcp_resources.")},
                    }},
            {"required", support::JsonValue::array_t{"server", "uri"}},
            {"additionalProperties", false},
    }};
}

[[nodiscard]] support::JsonValue error_items_schema() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "array"},
            {"description", "Servers that could not be listed"},
            {"items",
                    support::JsonValue::object_t{
                            {"type", "object"},
                            {"properties",
                                    support::JsonValue::object_t{
                                            {"server", support::JsonValue::object_t{{"type", "string"}}},
                                            {"error", support::JsonValue::object_t{{"type", "string"}}},
                                    }},
                            {"required", support::JsonValue::array_t{"server", "error"}},
                    }},
    }};
}

/// pi `LIST_OUTPUT_SCHEMA`.
[[nodiscard]] support::JsonValue list_output_schema() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"server", optional_string_property()},
                            {"resources",
                                    support::JsonValue::object_t{
                                            {"type", "array"},
                                            {"items",
                                                    support::JsonValue::object_t{
                                                            {"type", "object"},
                                                            {"properties",
                                                                    support::JsonValue::object_t{
                                                                            {"server",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"}}},
                                                                            {"uri",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"}}},
                                                                            {"name",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"}}},
                                                                            {"title", optional_string_property()},
                                                                            {"description",
                                                                                    optional_string_property()},
                                                                            {"mimeType", optional_string_property()},
                                                                            {"size",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "number"}}},
                                                                    }},
                                                            {"required",
                                                                    support::JsonValue::array_t{
                                                                            "server", "uri", "name"}},
                                                    }},
                                    }},
                            {"nextCursor", optional_string_property()},
                            {"errors", error_items_schema()},
                    }},
            {"required", support::JsonValue::array_t{"resources"}},
    }};
}

/// pi `LIST_TEMPLATES_OUTPUT_SCHEMA`.
[[nodiscard]] support::JsonValue list_templates_output_schema() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"server", optional_string_property()},
                            {"resourceTemplates",
                                    support::JsonValue::object_t{
                                            {"type", "array"},
                                            {"items",
                                                    support::JsonValue::object_t{
                                                            {"type", "object"},
                                                            {"properties",
                                                                    support::JsonValue::object_t{
                                                                            {"server",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"}}},
                                                                            {"uriTemplate",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"},
                                                                                            {"description",
                                                                                                    "RFC 6570 URI "
                                                                                                    "template"}}},
                                                                            {"name",
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "string"}}},
                                                                            {"title", optional_string_property()},
                                                                            {"description",
                                                                                    optional_string_property()},
                                                                            {"mimeType",
                                                                                    optional_string_property()},
                                                                    }},
                                                            {"required",
                                                                    support::JsonValue::array_t{
                                                                            "server", "uriTemplate", "name"}},
                                                    }},
                                    }},
                            {"nextCursor", optional_string_property()},
                            {"errors", error_items_schema()},
                    }},
            {"required", support::JsonValue::array_t{"resourceTemplates"}},
    }};
}

/// pi `READ_OUTPUT_SCHEMA`.
[[nodiscard]] support::JsonValue read_output_schema() {
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties",
                    support::JsonValue::object_t{
                            {"server", support::JsonValue::object_t{{"type", "string"}}},
                            {"uri", support::JsonValue::object_t{{"type", "string"}}},
                            {"contents",
                                    support::JsonValue::object_t{
                                            {"type", "array"},
                                            {"items",
                                                    support::JsonValue::object_t{
                                                            {"anyOf",
                                                                    support::JsonValue::array_t{
                                                                            support::JsonValue{
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "object"},
                                                                                            {"properties",
                                                                                                    support::JsonValue::object_t{
                                                                                                            {"uri",
                                                                                                                    support::JsonValue::object_t{
                                                                                                                            {"type",
                                                                                                                             "string"}}},
                                                                                                            {"mimeType",
                                                                                                                    optional_string_property()},
                                                                                                            {"text",
                                                                                                                    support::JsonValue::object_t{
                                                                                                                            {"type",
                                                                                                                             "string"}}},
                                                                                                    }},
                                                                                            {"required",
                                                                                                    support::JsonValue::array_t{
                                                                                                            "uri", "text"}},
                                                                                    }},
                                                                            support::JsonValue{
                                                                                    support::JsonValue::object_t{
                                                                                            {"type", "object"},
                                                                                            {"properties",
                                                                                                    support::JsonValue::object_t{
                                                                                                            {"uri",
                                                                                                                    support::JsonValue::object_t{
                                                                                                                            {"type",
                                                                                                                             "string"}}},
                                                                                                            {"mimeType",
                                                                                                                    optional_string_property()},
                                                                                                            {"blob",
                                                                                                                    support::JsonValue::object_t{
                                                                                                                            {"type",
                                                                                                                             "string"},
                                                                                                                            {"description",
                                                                                                                             "base64"}}},
                                                                                                    }},
                                                                                            {"required",
                                                                                                    support::JsonValue::array_t{
                                                                                                            "uri", "blob"}},
                                                                                    }},
                                                                    }},
                                                    }},
                                    }},
                    }},
            {"required", support::JsonValue::array_t{"server", "uri", "contents"}},
    }};
}

constexpr std::string_view kListResourcesDescription =
        "Lists resources provided by MCP servers. Resources allow servers to share data that provides context to "
        "language models, such as files, database schemas, or application-specific information. Prefer resources over "
        "web search when possible.";
constexpr std::string_view kListResourceTemplatesDescription =
        "Lists resource templates provided by MCP servers. Parameterized resource templates allow servers to share data "
        "that takes parameters and provides context to language models, such as files, database schemas, or "
        "application-specific information. Prefer resource templates over web search when possible.";
constexpr std::string_view kReadResourceDescription =
        "Read a specific resource from an MCP server given the server name and resource URI.";

[[nodiscard]] ExtensionTool make_list_tool(
        std::vector<std::shared_ptr<McpResourceServer>> servers, bool templates) {
    ExtensionTool tool;
    tool.definition.name = std::string{templates ? kListMcpResourceTemplatesTool : kListMcpResourcesTool};
    tool.definition.description = std::string{templates ? kListResourceTemplatesDescription : kListResourcesDescription};
    tool.definition.parameters = list_parameters();
    tool.definition.output_schema = templates ? list_templates_output_schema() : list_output_schema();
    // pi MCP tools carry no `executionMode`; the transports serialize frames
    // internally, which keeps parallel-safe listing safe.
    tool.concurrency = agent::ToolConcurrency::ParallelSafe;
    tool.execute = [servers = std::move(servers), templates](
                           support::JsonValue arguments, std::stop_token stop_token) {
        return support::detail::make_async_result([servers, templates, arguments = std::move(arguments), stop_token]()
                                                          mutable -> boost::asio::awaitable<
                                                                  support::Expected<ExtensionToolResult>> {
            co_return co_await run_list(std::move(servers), std::move(arguments), templates, stop_token);
        });
    };
    return tool;
}

} // namespace

bool is_mcp_app_resource(const support::JsonValue& item) {
    const auto* object = as_object(item);
    if (object == nullptr) {
        return false;
    }
    const std::string* uri = string_field(*object, "uri");
    if (uri == nullptr) {
        uri = string_field(*object, "uriTemplate");
    }
    if (uri != nullptr && uri->starts_with("ui://")) {
        return true;
    }
    const std::string* mime_type = string_field(*object, "mimeType");
    return mime_type != nullptr && has_mcp_app_profile(*mime_type);
}

McpConnectionResourceServer::McpConnectionResourceServer(std::shared_ptr<McpServerConnection> connection)
    : connection_(std::move(connection)) {}

const std::string& McpConnectionResourceServer::name() const noexcept { return connection_->server_name(); }

support::AsyncResult<support::JsonValue> McpConnectionResourceServer::resources_page(
        std::optional<std::string> cursor, std::stop_token stop_token) {
    auto connection = connection_;
    return support::detail::make_async_result(
            [connection, cursor = std::move(cursor), stop_token]() -> boost::asio::awaitable<
                    support::Expected<support::JsonValue>> {
                std::optional<support::JsonValue> params;
                if (cursor.has_value()) {
                    params = support::JsonValue{support::JsonValue::object_t{{"cursor", *cursor}}};
                }
                auto result = co_await support::detail::await_async_result(
                        connection->request("resources/list", std::move(params), stop_token));
                if (!result) {
                    co_return std::unexpected(std::move(result.error()));
                }
                auto list = list_items(*result, "resources");
                if (!list) {
                    co_return std::unexpected(std::move(list.error()));
                }
                // pi's client defaults a missing resource `name` from its `uri`.
                auto* items = result->get_object().at("resources").get_if<support::JsonValue::array_t>();
                for (auto& item : *items) {
                    if (auto* object = item.get_if<support::JsonValue::object_t>()) {
                        if (string_field(*object, "name") == nullptr) {
                            if (const std::string* uri = string_field(*object, "uri")) {
                                object->emplace("name", *uri);
                            }
                        }
                    }
                }
                co_return *result;
            });
}

support::AsyncResult<support::JsonValue> McpConnectionResourceServer::resource_templates_page(
        std::optional<std::string> cursor, std::stop_token stop_token) {
    auto connection = connection_;
    return support::detail::make_async_result(
            [connection, cursor = std::move(cursor), stop_token]() -> boost::asio::awaitable<
                    support::Expected<support::JsonValue>> {
                std::optional<support::JsonValue> params;
                if (cursor.has_value()) {
                    params = support::JsonValue{support::JsonValue::object_t{{"cursor", *cursor}}};
                }
                auto result = co_await support::detail::await_async_result(
                        connection->request("resources/templates/list", std::move(params), stop_token));
                if (!result) {
                    if (is_method_not_found(result.error())) {
                        co_return support::JsonValue{support::JsonValue::object_t{
                                {"resourceTemplates", support::JsonValue::array_t{}}}};
                    }
                    co_return std::unexpected(std::move(result.error()));
                }
                auto list = list_items(*result, "resourceTemplates");
                if (!list) {
                    co_return std::unexpected(std::move(list.error()));
                }
                // pi defaults a missing template `name` from its `uriTemplate`.
                auto* items = result->get_object().at("resourceTemplates").get_if<support::JsonValue::array_t>();
                for (auto& item : *items) {
                    if (auto* object = item.get_if<support::JsonValue::object_t>()) {
                        if (string_field(*object, "name") == nullptr) {
                            if (const std::string* uri_template = string_field(*object, "uriTemplate")) {
                                object->emplace("name", *uri_template);
                            }
                        }
                    }
                }
                co_return *result;
            });
}

support::AsyncResult<support::JsonValue> McpConnectionResourceServer::read_resource(
        std::string uri, std::stop_token stop_token) {
    auto connection = connection_;
    return support::detail::make_async_result([connection, uri = std::move(uri), stop_token]() -> boost::asio::awaitable<
            support::Expected<support::JsonValue>> {
        co_return co_await support::detail::await_async_result(connection->request("resources/read",
                support::JsonValue{support::JsonValue::object_t{{"uri", uri}}}, stop_token));
    });
}

support::AsyncResult<McpResourceSnapshot> fetch_mcp_resource_snapshot(
        McpResourceServer& server, std::stop_token stop_token) {
    return support::detail::make_async_result(
            [&server, stop_token]() -> boost::asio::awaitable<support::Expected<McpResourceSnapshot>> {
                McpResourceSnapshot snapshot;
                std::optional<std::string> cursor;
                for (;;) {
                    auto page = co_await support::detail::await_async_result(
                            server.resources_page(cursor, stop_token));
                    if (!page) {
                        break;
                    }
                    auto list = list_items(*page, "resources");
                    if (!list) {
                        break;
                    }
                    snapshot.has_resources = true;
                    for (const auto& item : **list) {
                        if (!is_mcp_app_resource(item)) {
                            snapshot.resources.push_back(item);
                        }
                    }
                    auto next = next_cursor(*page);
                    if (!next) {
                        break;
                    }
                    cursor = std::move(next);
                }
                cursor.reset();
                for (;;) {
                    auto page = co_await support::detail::await_async_result(
                            server.resource_templates_page(cursor, stop_token));
                    if (!page) {
                        break;
                    }
                    auto list = list_items(*page, "resourceTemplates");
                    if (!list) {
                        break;
                    }
                    for (const auto& item : **list) {
                        if (!is_mcp_app_resource(item)) {
                            snapshot.resource_templates.push_back(item);
                        }
                    }
                    auto next = next_cursor(*page);
                    if (!next) {
                        break;
                    }
                    cursor = std::move(next);
                }
                co_return snapshot;
            });
}

std::vector<ExtensionTool> create_mcp_resource_tools(std::vector<std::shared_ptr<McpResourceServer>> servers) {
    std::vector<ExtensionTool> tools;
    tools.reserve(3);
    tools.push_back(make_list_tool(servers, false));
    tools.push_back(make_list_tool(servers, true));

    ExtensionTool read_tool;
    read_tool.definition.name = std::string{kReadMcpResourceTool};
    read_tool.definition.description = std::string{kReadResourceDescription};
    read_tool.definition.parameters = read_parameters();
    read_tool.definition.output_schema = read_output_schema();
    read_tool.concurrency = agent::ToolConcurrency::ParallelSafe;
    read_tool.execute = [servers = std::move(servers)](
                                support::JsonValue arguments, std::stop_token stop_token) {
        return support::detail::make_async_result([servers, arguments = std::move(arguments), stop_token]() mutable
                                                          -> boost::asio::awaitable<
                                                                  support::Expected<ExtensionToolResult>> {
            co_return co_await run_read(std::move(servers), std::move(arguments), stop_token);
        });
    };
    tools.push_back(std::move(read_tool));
    return tools;
}

} // namespace cch::coding_agent::mcp
