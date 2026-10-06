// MCP Extension Tool Source (spec #865, tickets #869 stdio / #873
// streamable-http): connect one MCP server at Session Assembly, list its
// tools, and convert each into an extension Tool whose execution is
// `tools/call` on the transport-independent `McpServerConnection`. The tool
// conversion is pi `extensions/mcp/tools.ts` `createMcpToolDefinition`
// narrowed to this slice's scope: name, description, input schema, and a
// text/image result mapping. Output truncation, resource tools, exposure
// policy, and OAuth are later slices.

#include "coding_agent/mcp/McpExtensionToolSource.hpp"

#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"

#include <cch/ai/Content.hpp>

#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <optional>
#include <stop_token>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

[[nodiscard]] support::Error source_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Process, std::move(message), std::move(detail));
}

[[nodiscard]] std::string sanitize_identifier(std::string_view value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (const char character : value) {
        const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '_';
        sanitized.push_back(allowed ? character : '_');
    }
    return sanitized;
}

/// Convert one `tools/call` result (`{content, structuredContent?, isError?}`)
/// into the extension-side outcome. Text blocks map to model text, image
/// blocks to model images, and any other block to its compact JSON text so no
/// server content is dropped silently.
[[nodiscard]] support::Expected<extensions::ExtensionToolResult> convert_tools_call_result(
        const std::string& server, const support::JsonValue& result) {
    const auto* object = result.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(source_error("MCP server '" + server + "' sent an invalid tools/call result"));
    }
    const auto content = object->find("content");
    if (content == object->end() || content->second.get_if<support::JsonValue::array_t>() == nullptr) {
        return std::unexpected(source_error("MCP server '" + server + "' sent a tools/call result without content"));
    }

    extensions::ExtensionToolResult outcome;
    for (const auto& block : content->second.get_array()) {
        const auto* block_object = block.get_if<support::JsonValue::object_t>();
        if (block_object == nullptr) {
            continue;
        }
        const auto type = block_object->find("type");
        const std::string block_type =
                (type != block_object->end() && type->second.holds<std::string>()) ? type->second.get_string() : "";
        if (block_type == "text") {
            const auto text = block_object->find("text");
            outcome.content.push_back(ai::text_content(
                    text != block_object->end() && text->second.holds<std::string>() ? text->second.get_string() : ""));
        } else if (block_type == "image") {
            const auto data = block_object->find("data");
            const auto mime = block_object->find("mimeType");
            outcome.content.push_back(ai::image_content(
                    data != block_object->end() && data->second.holds<std::string>() ? data->second.get_string() : "",
                    mime != block_object->end() && mime->second.holds<std::string>() ? mime->second.get_string() : ""));
        } else {
            auto serialized = support::write_json(block);
            outcome.content.push_back(ai::text_content(serialized ? std::move(*serialized) : std::string{}));
        }
    }

    if (const auto structured = object->find("structuredContent"); structured != object->end()) {
        outcome.details = structured->second;
    }
    if (const auto is_error = object->find("isError");
            is_error != object->end() && is_error->second.get_if<bool>() != nullptr) {
        outcome.is_error = *is_error->second.get_if<bool>();
    }
    return outcome;
}

/// `tools/list`, following `nextCursor` to exhaustion (pi `listAll`).
[[nodiscard]] boost::asio::awaitable<support::Expected<std::vector<McpToolDescriptor>>> list_server_tools(
        McpServerConnection& connection) {
    const std::string server = connection.server_name();
    std::vector<McpToolDescriptor> tools;
    std::optional<std::string> cursor;
    for (;;) {
        std::optional<support::JsonValue> params;
        if (cursor.has_value()) {
            params = support::JsonValue{support::JsonValue::object_t{{"cursor", *cursor}}};
        }
        auto page = co_await support::detail::await_async_result(connection.request("tools/list", std::move(params)));
        if (!page) {
            co_return std::unexpected(std::move(page.error()));
        }
        const auto* object = page->get_if<support::JsonValue::object_t>();
        if (object == nullptr) {
            co_return std::unexpected(source_error("MCP server '" + server + "' sent an invalid tools/list result"));
        }
        const auto list = object->find("tools");
        if (list == object->end() || list->second.get_if<support::JsonValue::array_t>() == nullptr) {
            co_return std::unexpected(
                    source_error("MCP server '" + server + "' sent a tools/list result without tools"));
        }
        for (const auto& entry : list->second.get_array()) {
            const auto* entry_object = entry.get_if<support::JsonValue::object_t>();
            if (entry_object == nullptr) {
                continue;
            }
            const auto name = entry_object->find("name");
            const auto schema = entry_object->find("inputSchema");
            // pi `isTool`: a tool is `{name: string, inputSchema: object}`.
            if (name == entry_object->end() || !name->second.holds<std::string>() || schema == entry_object->end() ||
                    schema->second.get_if<support::JsonValue::object_t>() == nullptr) {
                continue;
            }
            McpToolDescriptor descriptor;
            descriptor.server_tool_name = name->second.get_string();
            descriptor.full_name = mcp_tool_name(server, descriptor.server_tool_name);
            descriptor.parameters = schema->second;
            if (const auto description = entry_object->find("description");
                    description != entry_object->end() && description->second.holds<std::string>()) {
                descriptor.description = description->second.get_string();
            }
            if (descriptor.description.empty()) {
                descriptor.description = "MCP tool " + descriptor.server_tool_name + " from server " + server;
            }
            tools.push_back(std::move(descriptor));
        }
        const auto next = object->find("nextCursor");
        if (next == object->end() || !next->second.holds<std::string>()) {
            break;
        }
        cursor = next->second.get_string();
    }
    co_return tools;
}

} // namespace

std::string mcp_tool_name(std::string_view server, std::string_view tool) {
    std::string name = "mcp__";
    name += sanitize_identifier(server);
    name += "__";
    name += sanitize_identifier(tool);
    return name;
}

McpExtensionToolSource::McpExtensionToolSource(
        std::shared_ptr<McpServerConnection> connection, std::vector<McpToolDescriptor> tools)
    : connection_(std::move(connection)), server_name_(connection_->server_name()), tools_(std::move(tools)) {}

boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>>
McpExtensionToolSource::connect_stdio(McpStdioServerConfig config) {
    auto client = co_await McpStdioClient::connect(std::move(config));
    if (!client) {
        co_return std::unexpected(std::move(client.error()));
    }
    auto tools = co_await list_server_tools(**client);
    if (!tools) {
        co_return std::unexpected(std::move(tools.error()));
    }
    co_return std::unique_ptr<McpExtensionToolSource>(
            new McpExtensionToolSource(std::move(*client), std::move(*tools)));
}

boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>> McpExtensionToolSource::connect_http(
        McpHttpServerConfig config, std::shared_ptr<McpRequestAuthSource> request_auth) {
    auto client = co_await McpHttpClient::connect(
            std::move(config), std::make_shared<ai::providers::BoostBeastStreamTransport>(), std::move(request_auth));
    if (!client) {
        co_return std::unexpected(std::move(client.error()));
    }
    auto tools = co_await list_server_tools(**client);
    if (!tools) {
        co_return std::unexpected(std::move(tools.error()));
    }
    co_return std::unique_ptr<McpExtensionToolSource>(
            new McpExtensionToolSource(std::move(*client), std::move(*tools)));
}

support::Expected<std::vector<extensions::ExtensionTool>> McpExtensionToolSource::load_tools() {
    std::vector<extensions::ExtensionTool> result;
    result.reserve(tools_.size());
    for (const auto& descriptor : tools_) {
        extensions::ExtensionTool tool;
        tool.definition.name = descriptor.full_name;
        tool.definition.description = descriptor.description;
        tool.definition.parameters = descriptor.parameters;
        // pi MCP tools carry no `executionMode`, so they are parallel-capable;
        // the transport serializes frames internally, which keeps that safe.
        tool.concurrency = agent::ToolConcurrency::ParallelSafe;
        // pi MCP tools set `description` only (no `promptSnippet`), so they are
        // declared to the model without joining the prompt's Available tools
        // list.
        tool.prompt_snippet = std::nullopt;

        auto connection = connection_;
        const std::string server_tool_name = descriptor.server_tool_name;
        const std::string server = server_name_;
        tool.execute = [connection, server_tool_name, server](support::JsonValue arguments,
                               std::stop_token stop_token) -> support::AsyncResult<extensions::ExtensionToolResult> {
            return support::AsyncResult<extensions::ExtensionToolResult>{
                    support::AsyncProducer<extensions::ExtensionToolResult, support::Error>{
                            [connection, server_tool_name, server, arguments = std::move(arguments), stop_token](
                                    support::AsyncCompletion<extensions::ExtensionToolResult, support::Error>
                                            completion) mutable noexcept {
                                if (stop_token.stop_requested()) {
                                    completion(std::unexpected(
                                            support::make_error(support::ErrorCode::Cancelled, "Operation aborted")));
                                    return;
                                }
                                support::JsonValue params{support::JsonValue::object_t{
                                        {"name", server_tool_name},
                                        {"arguments", std::move(arguments)},
                                }};
                                connection->request("tools/call", std::move(params))
                                        .start([server, completion = std::move(completion)](
                                                       support::Expected<support::JsonValue> outcome) mutable noexcept {
                                            if (!outcome) {
                                                completion(std::unexpected(std::move(outcome.error())));
                                                return;
                                            }
                                            completion(convert_tools_call_result(server, *outcome));
                                        });
                            }}};
        };
        result.push_back(std::move(tool));
    }
    return result;
}

} // namespace cch::coding_agent::mcp
