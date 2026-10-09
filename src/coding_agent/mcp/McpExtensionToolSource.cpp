// MCP Extension Tool Source (spec #865, tickets #869 stdio / #873
// streamable-http; spec #882 activation routes the conversion through the
// live manager): connect one MCP server at Session Assembly, list its
// tools, and convert each into an extension Tool whose execution is
// `tools/call` on the transport-independent `McpServerConnection`. The tool
// conversion is pi `extensions/mcp/tools.ts` `createMcpToolDefinition`
// narrowed to this file's scope: name, description, input/output schema, and
// a text/image result mapping whose structured details are pi
// `convertMcpResult`'s scriptResult (the whole CallToolResult minus `_meta`).
// Output truncation beyond that mapping and pi's resource-link rendering are
// recorded as a residual in ADR 0066 (spec #882 close-out).

#include "coding_agent/mcp/McpExtensionToolSource.hpp"

#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"

#include <cch/ai/Content.hpp>

#include <cch/ai/Pkce.hpp>

#include <cch/ai/BoostBeastStreamTransport.hpp>
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <stop_token>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

[[nodiscard]] support::Error source_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Process, std::move(message), std::move(detail));
}

constexpr std::size_t kMcpOutputMaxBytes = 20 * 1024;

[[nodiscard]] std::string truncate_middle_mcp(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return std::string{text};
    const std::size_t keep = max_bytes / 2;
    std::string result;
    result.reserve(max_bytes + 64);
    result.append(text.substr(0, keep));
    result.append("\n... [truncated] ...\n");
    result.append(text.substr(text.size() - keep));
    return result;
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

} // namespace

/// Convert one `tools/call` result (see the header declaration).
support::Expected<extensions::ExtensionToolResult> convert_mcp_tools_call_result(
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

    // pi `convertMcpResult`: the script-visible structured content is the
    // whole CallToolResult object minus `_meta` (content blocks, the tool's
    // own `structuredContent`, `isError`) — codemode resolves a script call
    // to exactly this object, error results included.
    support::JsonValue::object_t script_result;
    for (const auto& [key, value] : *object) {
        if (key == "_meta") {
            continue;
        }
        script_result.emplace(key, value);
    }
    // pi limitMcpContent: text outputs > 20 KiB are middle-truncated, spilled to a temp file,
    // and fullOutputPath is recorded in details.
    std::string combined_text;
    for (const auto& item : outcome.content) {
        if (const auto* text = std::get_if<ai::TextContent>(&item)) {
            combined_text += text->text;
        }
    }

    if (combined_text.size() > kMcpOutputMaxBytes) {
        std::error_code ec;
        auto temp_dir = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            auto temp_file = temp_dir / std::format("mcp-{}-{}.txt", server, ::getpid());
            std::ofstream out(temp_file, std::ios::binary);
            if (out.is_open()) {
                out << combined_text;
                out.close();
                script_result.emplace("fullOutputPath", temp_file.string());
                outcome.content.clear();
                outcome.content.push_back(ai::text_content(truncate_middle_mcp(combined_text, kMcpOutputMaxBytes) +
                                                           "\n\nFull output saved to: " + temp_file.string()));
            }
        }
    }

    outcome.details = support::JsonValue{std::move(script_result)};
    if (const auto is_error = object->find("isError");
            is_error != object->end() && is_error->second.get_if<bool>() != nullptr) {
        outcome.is_error = *is_error->second.get_if<bool>();
    }
    return outcome;
}

/// `tools/list`, following `nextCursor` to exhaustion (pi `listAll`).
/// pi `index.ts` assigns the Agent-visible names across the whole listing
/// order-independently (colliding sanitized names all take the hash suffix),
/// so the assignment happens once, after every page is collected.
std::string progress_update_text(const support::JsonValue& params) {
    const auto* object = params.get_if<support::JsonValue::object_t>();
    if (object != nullptr) {
        if (const auto message = object->find("message");
                message != object->end() && message->second.holds<std::string>()) {
            return message->second.get_string();
        }
        const auto progress = object->find("progress");
        if (progress != object->end() && progress->second.holds<double>()) {
            std::string text = "Progress " + std::to_string(static_cast<long long>(progress->second.get_number()));
            if (const auto total = object->find("total"); total != object->end() && total->second.holds<double>()) {
                text += "/" + std::to_string(static_cast<long long>(total->second.get_number()));
            }
            return text;
        }
    }
    return {};
}

void assign_listing_names(std::string_view server, std::vector<McpToolDescriptor>& tools) {
    std::vector<std::string> raw_names;
    raw_names.reserve(tools.size());
    for (const auto& tool : tools) {
        raw_names.push_back(tool.server_tool_name);
    }
    const std::vector<std::string> assigned = assign_mcp_tool_names(server, raw_names);
    for (std::size_t index = 0; index < tools.size(); ++index) {
        tools[index].full_name = assigned[index];
    }
}

boost::asio::awaitable<support::Expected<std::vector<McpToolDescriptor>>> list_mcp_server_tools(
        McpServerConnection& connection) {
    const std::string server = connection.server_name();
    std::vector<McpToolDescriptor> tools;
    std::set<std::string> cursors;
    std::optional<std::string> cursor;
    // pi `listAll`: follow nextCursor to at most MAX_LIST_PAGES, fail a
    // duplicate cursor explicitly, and let `null`/`""` end pagination — a
    // server that echoes either forever must not loop the client.
    for (int page_number = 0;; ++page_number) {
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
            descriptor.parameters = schema->second;
            if (const auto description = entry_object->find("description");
                    description != entry_object->end() && description->second.holds<std::string>()) {
                descriptor.description = description->second.get_string();
            }
            if (const auto output = entry_object->find("outputSchema");
                    output != entry_object->end() && output->second.get_if<support::JsonValue::object_t>() != nullptr) {
                descriptor.output_schema = output->second;
            }
            if (descriptor.description.empty()) {
                descriptor.description = "MCP tool " + descriptor.server_tool_name + " from server " + server;
            }
            tools.push_back(std::move(descriptor));
        }
        // pi `validateListPage`: `null` and `""` end pagination; any other
        // non-string cursor is an explicit error.
        const auto next = object->find("nextCursor");
        if (next == object->end() || next->second.holds<support::JsonValue::null_t>()) {
            break;
        }
        if (!next->second.holds<std::string>()) {
            co_return std::unexpected(source_error("Invalid MCP tools/list cursor"));
        }
        std::string next_cursor = next->second.get_string();
        if (next_cursor.empty()) {
            break;
        }
        if (!cursors.insert(next_cursor).second) {
            co_return std::unexpected(source_error("MCP tools/list returned duplicate cursor: " + next_cursor));
        }
        if (page_number + 1 >= kMcpMaxListPages) {
            co_return std::unexpected(
                    source_error("MCP tools/list exceeded " + std::to_string(kMcpMaxListPages) + " pages"));
        }
        cursor = std::move(next_cursor);
    }
    assign_listing_names(server, tools);
    co_return tools;
}

[[nodiscard]] std::string plain_mcp_tool_name(std::string_view server, std::string_view tool) {
    std::string name = "mcp__";
    name += sanitize_identifier(server);
    name += "__";
    name += sanitize_identifier(tool);
    return name;
}

std::string mcp_tool_name(std::string_view server, std::string_view tool) {
    return mcp_tool_name(server, tool, nullptr);
}

/// pi `createMcpToolName`'s hash suffix: the first 8 hex characters of the
/// sha256 of the RAW `${server}\0${tool}` pair (pi hashes before
/// sanitizing, so two tools that sanitize to one name hash differently).
[[nodiscard]] std::string tool_name_hash_suffix(std::string_view server, std::string_view tool) {
    std::string input{server};
    input.push_back('\0');
    input += tool;
    auto digest = ai::auth::sha256_digest(input);
    if (!digest || digest->size() < 4) {
        // pi cannot fail here; a digest failure degrades to the truncated
        // plain name rather than failing tool registration.
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string suffix;
    suffix.reserve(9);
    suffix.push_back('_');
    for (std::size_t index = 0; index < 4; ++index) {
        const auto byte = static_cast<unsigned char>((*digest)[index]);
        suffix.push_back(kHex[byte >> 4]);
        suffix.push_back(kHex[byte & 0x0f]);
    }
    return suffix;
}

std::string mcp_tool_name(
        std::string_view server, std::string_view tool, std::move_only_function<bool(const std::string&)> is_taken) {
    std::string name = plain_mcp_tool_name(server, tool);
    if (name.size() <= kMcpMaxToolNameLength && (is_taken == nullptr || !is_taken(name))) {
        return name;
    }
    // pi: `${name.slice(0, MAX_TOOL_NAME_LENGTH - hash.length - 1)}_${hash}`
    // with an 8-hex hash — the kept prefix is 55 characters.
    std::string suffixed = name.substr(0, kMcpMaxToolNameLength - 9);
    suffixed += tool_name_hash_suffix(server, tool);
    return suffixed;
}

std::vector<std::string> assign_mcp_tool_names(
        std::string_view server, const std::vector<std::string>& raw_tool_names) {
    // pi `index.ts`: `plain` counts the sanitized names; a candidate is taken
    // when another tool owns it or its plain name is duplicated, so every
    // colliding tool hashes and the result does not depend on list order.
    std::map<std::string, int> plain_counts;
    for (const auto& raw : raw_tool_names) {
        ++plain_counts[plain_mcp_tool_name(server, raw)];
    }
    std::set<std::string> owners;
    std::vector<std::string> assigned;
    assigned.reserve(raw_tool_names.size());
    for (const auto& raw : raw_tool_names) {
        auto taken = [&](const std::string& candidate) {
            auto it = plain_counts.find(candidate);
            return owners.contains(candidate) || (it != plain_counts.end() && it->second > 1);
        };
        std::string name = mcp_tool_name(server, raw, std::move(taken));
        assigned.push_back(name);
        owners.insert(std::move(name));
    }
    return assigned;
}

support::JsonValue create_mcp_result_schema(const std::optional<support::JsonValue>& structured_content_schema) {
    support::JsonValue::object_t properties;
    properties.emplace("content",
            support::JsonValue{support::JsonValue::object_t{
                    {"type", "array"},
                    {"items", support::JsonValue::object_t{{"type", "object"}}},
            }});
    if (structured_content_schema.has_value()) {
        properties.emplace("structuredContent", *structured_content_schema);
    }
    properties.emplace("isError", support::JsonValue{support::JsonValue::object_t{{"type", "boolean"}}});
    properties.emplace("_meta", support::JsonValue{support::JsonValue::object_t{{"type", "object"}}});
    return support::JsonValue{support::JsonValue::object_t{
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", support::JsonValue::array_t{"content"}},
    }};
}

McpExtensionToolSource::McpExtensionToolSource(
        ConstructionKey, std::shared_ptr<McpServerConnection> connection, std::vector<McpToolDescriptor> tools)
    : connection_(std::move(connection)), server_name_(connection_->server_name()), tools_(std::move(tools)) {}

boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>>
McpExtensionToolSource::connect_stdio(McpStdioServerConfig config) {
    auto client = co_await McpStdioClient::connect(std::move(config));
    if (!client) {
        co_return std::unexpected(std::move(client.error()));
    }
    auto tools = co_await list_mcp_server_tools(**client);
    if (!tools) {
        co_return std::unexpected(std::move(tools.error()));
    }
    co_return std::make_unique<McpExtensionToolSource>(ConstructionKey{}, std::move(*client), std::move(*tools));
}

boost::asio::awaitable<support::Expected<std::unique_ptr<McpExtensionToolSource>>> McpExtensionToolSource::connect_http(
        McpHttpServerConfig config, std::shared_ptr<McpRequestAuthSource> request_auth) {
    auto client = co_await McpHttpClient::connect(
            std::move(config), std::make_shared<ai::providers::BoostBeastStreamTransport>(), std::move(request_auth));
    if (!client) {
        co_return std::unexpected(std::move(client.error()));
    }
    auto tools = co_await list_mcp_server_tools(**client);
    if (!tools) {
        co_return std::unexpected(std::move(tools.error()));
    }
    co_return std::make_unique<McpExtensionToolSource>(ConstructionKey{}, std::move(*client), std::move(*tools));
}

support::Expected<std::vector<extensions::ExtensionTool>> McpExtensionToolSource::load_tools() {
    std::vector<extensions::ExtensionTool> result;
    result.reserve(tools_.size());
    for (const auto& descriptor : tools_) {
        extensions::ExtensionTool tool;
        tool.definition.name = descriptor.full_name;
        tool.definition.description = descriptor.description;
        tool.definition.parameters = descriptor.parameters;
        tool.definition.output_schema = create_mcp_result_schema(descriptor.output_schema);
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
        // pi `createMcpToolDefinition`'s execute observes the call's progress
        // (pi `McpRequestOptions.onProgress`), so the tool runs through the
        // context form and streams "Progress <n>[/<total>]" updates through
        // the run's update sink.
        tool.context_execute =
                [connection, server_tool_name, server](support::JsonValue arguments,
                        extensions::ExtensionToolContext context,
                        std::stop_token stop_token) -> support::AsyncResult<extensions::ExtensionToolResult> {
            agent::ToolUpdateSink update_sink = std::move(context.update_sink);
            return support::AsyncResult<extensions::ExtensionToolResult>{
                    support::AsyncProducer<extensions::ExtensionToolResult, support::Error>{
                            [connection,
                                    server_tool_name,
                                    server,
                                    arguments = std::move(arguments),
                                    update_sink = std::move(update_sink),
                                    stop_token](support::AsyncCompletion<extensions::ExtensionToolResult,
                                    support::Error> completion) mutable noexcept {
                                if (stop_token.stop_requested()) {
                                    completion(std::unexpected(
                                            support::make_error(support::ErrorCode::Cancelled, "Operation aborted")));
                                    return;
                                }
                                support::JsonValue params{support::JsonValue::object_t{
                                        {"name", server_tool_name},
                                        {"arguments", std::move(arguments)},
                                }};
                                McpServerConnection::ProgressCallback on_progress;
                                if (update_sink) {
                                    on_progress = [update_sink = std::move(update_sink)](
                                                          const support::JsonValue& progress) mutable {
                                        (void)update_sink(agent::AsyncToolExecutionResult{
                                                .content = std::vector<ai::Content>{ai::text_content(
                                                        progress_update_text(progress))},
                                        });
                                    };
                                }
                                connection
                                        ->request("tools/call",
                                                std::move(params),
                                                McpServerConnection::RequestOptions{.stop_token = stop_token,
                                                        .on_progress = std::move(on_progress)})
                                        .start([server, completion = std::move(completion)](
                                                       support::Expected<support::JsonValue> outcome) mutable noexcept {
                                            if (!outcome) {
                                                completion(std::unexpected(std::move(outcome.error())));
                                                return;
                                            }
                                            completion(convert_mcp_tools_call_result(server, *outcome));
                                        });
                            }}};
        };
        result.push_back(std::move(tool));
    }
    return result;
}

} // namespace cch::coding_agent::mcp
