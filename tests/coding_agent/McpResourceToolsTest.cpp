// Spec #882 resource-tools slice (#884): the Codex-compatible
// `list_mcp_resources`, `list_mcp_resource_templates`, and `read_mcp_resource`
// tools, a port of pi v1.0.4 `extensions/mcp/resources.ts`.
//
// The scripted servers are deterministic in-process fakes implementing the
// `McpResourceServer` seam, so multi-server semantics (sorted union, failures
// into `errors`, cursor rules, the unknown-server message) are pinned exactly.
// One case drives the real stdio fixture (`fixtures/pi-mcp/echo_server.py`),
// which lists a `_meta`/icons resource, a `ui://` MCP App resource, and a
// resource without a `name`, so the wire methods and the normalization
// (`resources/list`, `resources/templates/list`, `resources/read`) are
// exercised end to end.
//
// Separation cases: a server that fails while another succeeds separates
// "failures go to `errors`" from "listing threw"; a listed `ui://` resource
// separates "MCP App resources are filtered" from "everything is listed"; a
// tool whose server declares no `outputSchema` separates "the converted tool
// declares the MCP result schema" from "the field is always absent".

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpResourceTools.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/JsonCompare.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::extensions::ExtensionTool;
using coding_agent::extensions::ExtensionToolResult;
using coding_agent::mcp::McpResourceServer;
using coding_agent::mcp::McpResourceSnapshot;

[[nodiscard]] support::JsonValue object(support::JsonValue::object_t fields) {
    return support::JsonValue{std::move(fields)};
}

[[nodiscard]] support::JsonValue array(support::JsonValue::array_t items) {
    return support::JsonValue{std::move(items)};
}

/// A deterministic resource server whose pages, reads, and failures are
/// scripted. `page_size` controls pagination so the no-server path has to
/// follow `nextCursor`.
class ScriptedResourceServer final : public McpResourceServer {
public:
    ScriptedResourceServer(std::string name,
            std::vector<support::JsonValue> resources,
            std::vector<support::JsonValue> templates = {})
        : name_(std::move(name)), resources_(std::move(resources)), templates_(std::move(templates)) {}

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string> cursor, std::stop_token) override {
        calls.push_back(cursor ? "resources/list#" + *cursor : "resources/list");
        if (fail_listing) {
            return error_result(failure_message);
        }
        return ready(page_for(resources_, "resources", cursor, "uri"));
    }

    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string> cursor, std::stop_token) override {
        calls.push_back(cursor ? "resources/templates/list#" + *cursor : "resources/templates/list");
        if (fail_listing) {
            return error_result(failure_message);
        }
        return ready(page_for(templates_, "resourceTemplates", cursor, "uriTemplate"));
    }

    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(std::string uri, std::stop_token) override {
        calls.push_back("resources/read " + uri);
        if (!read_error.empty()) {
            return error_result(read_error);
        }
        const auto found = reads.find(uri);
        if (found == reads.end()) {
            return error_result("Unknown resource: " + uri);
        }
        return ready(object({{"contents", array(found->second)}}));
    }

    std::size_t page_size{1};
    bool fail_listing{false};
    std::string failure_message{"listing failed"};
    std::string read_error;
    std::map<std::string, std::vector<support::JsonValue>> reads;
    std::vector<std::string> calls;

private:
    [[nodiscard]] support::JsonValue page_for(const std::vector<support::JsonValue>& items,
            const std::string& key,
            const std::optional<std::string>& cursor,
            std::string_view name_source) {
        std::size_t start = 0;
        if (cursor.has_value()) {
            start = static_cast<std::size_t>(std::stoul(*cursor));
        }
        support::JsonValue::array_t page;
        for (std::size_t index = start; index < items.size() && page.size() < page_size; ++index) {
            // The real connection defaults a missing `name` from `uri`/`uriTemplate`,
            // so the scripted server stands in for that same behavior.
            support::JsonValue item = items[index];
            if (auto* object = item.get_if<support::JsonValue::object_t>()) {
                const auto name = object->find("name");
                if (name == object->end()) {
                    const auto source = object->find(std::string{name_source});
                    if (source != object->end()) {
                        object->emplace("name", source->second);
                    }
                }
            }
            page.push_back(std::move(item));
        }
        support::JsonValue::object_t result{{key, array(std::move(page))}};
        const std::size_t next = start + page_size;
        if (next < items.size()) {
            result.emplace("nextCursor", std::to_string(next));
        }
        return object(std::move(result));
    }

    [[nodiscard]] support::AsyncResult<support::JsonValue> ready(support::JsonValue value) {
        return support::AsyncResult<support::JsonValue>{support::Expected<support::JsonValue>{std::move(value)}};
    }

    [[nodiscard]] support::AsyncResult<support::JsonValue> error_result(std::string message) {
        return support::AsyncResult<support::JsonValue>{support::Expected<support::JsonValue>{
                std::unexpected(support::make_error(support::ErrorCode::Process, std::move(message)))}};
    }

    std::string name_;
    std::vector<support::JsonValue> resources_;
    std::vector<support::JsonValue> templates_;
};

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

[[nodiscard]] coding_agent::mcp::McpStdioServerConfig echo_server_config() {
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_path("echo_server.py")};
    return config;
}

/// Run one tool execute with `arguments` and return its terminal outcome.
[[nodiscard]] support::Expected<ExtensionToolResult> run_tool(
        tests::RuntimeFixture& runtime, ExtensionTool& tool, support::JsonValue arguments) {
    return tests::run_awaitable(
            runtime, support::detail::await_async_result(tool.execute(std::move(arguments), std::stop_token{})));
}

[[nodiscard]] support::JsonValue payload_of(const ExtensionToolResult& result) {
    REQUIRE(result.details.has_value());
    return *result.details;
}

[[nodiscard]] std::string tool_text(const ExtensionToolResult& result) {
    return ai::text_from_content(result.content);
}

} // namespace

TEST_CASE("the three resource tools carry pi's verbatim descriptions, parameters, and output schemas",
        "[coding_agent][mcp][issue884][spec]") {
    std::vector<std::shared_ptr<McpResourceServer>> servers;
    const auto tools = coding_agent::mcp::create_mcp_resource_tools(servers);
    REQUIRE(tools.size() == 3);

    CHECK(tools[0].definition.name == "list_mcp_resources");
    CHECK(tools[0].definition.description ==
          "Lists resources provided by MCP servers. Resources allow servers to share data that provides context to "
          "language models, such as files, database schemas, or application-specific information. Prefer resources "
          "over web search when possible.");
    CHECK(tools[1].definition.name == "list_mcp_resource_templates");
    CHECK(tools[1].definition.description ==
          "Lists resource templates provided by MCP servers. Parameterized resource templates allow servers to share "
          "data that takes parameters and provides context to language models, such as files, database schemas, or "
          "application-specific information. Prefer resource templates over web search when possible.");
    CHECK(tools[2].definition.name == "read_mcp_resource");
    CHECK(tools[2].definition.description ==
          "Read a specific resource from an MCP server given the server name and resource URI.");

    // The list tools share pi's `LIST_PARAMETERS`; read declares `required`
    // `[server, uri]`.
    const support::JsonValue expected_list_parameters = object({
            {"type", "object"},
            {"properties",
                    object({
                            {"server",
                                    object({{"type", "string"},
                                            {"description",
                                                    "MCP server name. Omit to list every server with resources."}})},
                            {"cursor",
                                    object({{"type", "string"},
                                            {"description", "Opaque cursor from a previous call with the same server; "
                                                            "omit for the first page."}})},
                    })},
            {"additionalProperties", false},
    });
    CHECK_FALSE(tests::json_mismatch(expected_list_parameters, tools[0].definition.parameters).has_value());
    CHECK_FALSE(tests::json_mismatch(expected_list_parameters, tools[1].definition.parameters).has_value());

    REQUIRE(tools[2].definition.output_schema.has_value());
    const auto read_required = tools[2].definition.output_schema->get_object().find("required");
    REQUIRE(read_required != tools[2].definition.output_schema->get_object().end());
    CHECK_FALSE(tests::json_mismatch(array({"server", "uri", "contents"}), read_required->second).has_value());

    REQUIRE(tools[0].definition.output_schema.has_value());
    const auto list_required = tools[0].definition.output_schema->get_object().find("required");
    REQUIRE(list_required != tools[0].definition.output_schema->get_object().end());
    CHECK_FALSE(tests::json_mismatch(array({"resources"}), list_required->second).has_value());
}

TEST_CASE("list_mcp_resources without a server lists every page of every server and collects failures",
        "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;

    auto alpha = std::make_shared<ScriptedResourceServer>("alpha",
            std::vector<support::JsonValue>{
                    object({{"uri", "file:///a.txt"},
                            {"name", "a.txt"},
                            {"title", "A"},
                            {"mimeType", "text/plain"},
                            {"size", 3},
                            {"_meta", object({{"hidden", true}})},
                            {"icons", array({object({{"src", "data:,"}})})}}),
                    object({{"uri", "ui://widget/a.html"}, {"name", "widget"}, {"mimeType", "text/html;profile=mcp-app"}}),
                    object({{"uri", "notes://scratch"}}),
            });
    auto zeta = std::make_shared<ScriptedResourceServer>("zeta", std::vector<support::JsonValue>{});
    zeta->fail_listing = true;
    zeta->failure_message = "connection closed";

    auto tools = coding_agent::mcp::create_mcp_resource_tools({zeta, alpha});
    auto outcome = run_tool(runtime, tools[0], object({}));
    REQUIRE(outcome.has_value());

    // Sorted by name (`alpha` before `zeta`); `alpha`'s MCP App resource is
    // filtered and its `_meta`/icons removed; `notes://scratch` defaults its
    // `name` from the `uri`; `zeta`'s failure lands in `errors`, not as a
    // thrown list.
    const support::JsonValue expected = object({
            {"resources",
                    array({
                            object({{"server", "alpha"},
                                    {"uri", "file:///a.txt"},
                                    {"name", "a.txt"},
                                    {"title", "A"},
                                    {"mimeType", "text/plain"},
                                    {"size", 3}}),
                            object({{"server", "alpha"}, {"uri", "notes://scratch"}, {"name", "notes://scratch"}}),
                    })},
            {"errors", array({object({{"server", "zeta"}, {"error", "connection closed"}})})},
    });
    const auto mismatch = tests::json_mismatch(expected, payload_of(*outcome));
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }

    // The tool's model-facing content is the compact JSON payload.
    const auto text = tool_text(*outcome);
    CHECK(text.starts_with("{\"errors\":["));
}

TEST_CASE("list_mcp_resources with a server lists one page and returns its cursor",
        "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;

    auto alpha = std::make_shared<ScriptedResourceServer>("alpha",
            std::vector<support::JsonValue>{
                    object({{"uri", "file:///a.txt"}, {"name", "a.txt"}}),
                    object({{"uri", "file:///b.txt"}, {"name", "b.txt"}}),
            });
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto first = run_tool(runtime, tools[0], object({{"server", "alpha"}}));
    REQUIRE(first.has_value());
    const support::JsonValue expected_first = object({
            {"server", "alpha"},
            {"resources", array({object({{"server", "alpha"}, {"uri", "file:///a.txt"}, {"name", "a.txt"}})})},
            {"nextCursor", "1"},
    });
    CHECK_FALSE(tests::json_mismatch(expected_first, payload_of(*first)).has_value());

    auto second = run_tool(runtime, tools[0], object({{"server", "alpha"}, {"cursor", "1"}}));
    REQUIRE(second.has_value());
    const support::JsonValue expected_second = object({
            {"server", "alpha"},
            {"resources", array({object({{"server", "alpha"}, {"uri", "file:///b.txt"}, {"name", "b.txt"}})})},
    });
    CHECK_FALSE(tests::json_mismatch(expected_second, payload_of(*second)).has_value());
    // The cursor reached the wire as a `resources/list` parameter.
    CHECK(std::ranges::find(alpha->calls, "resources/list#1") != alpha->calls.end());
}

TEST_CASE("a cursor without a server is pi's explicit error", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>("alpha", std::vector<support::JsonValue>{});
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto outcome = run_tool(runtime, tools[0], object({{"cursor", "1"}}));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message == "cursor can only be used when a server is specified");
}

TEST_CASE("an unknown server names every server the resource tools reach", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>("alpha", std::vector<support::JsonValue>{});
    auto beta = std::make_shared<ScriptedResourceServer>("beta", std::vector<support::JsonValue>{});
    auto tools = coding_agent::mcp::create_mcp_resource_tools({beta, alpha});

    auto outcome = run_tool(runtime, tools[0], object({{"server", "nope"}}));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message ==
          "MCP server \"nope\" has no resources. Servers with resources: beta, alpha");
}

TEST_CASE("list_mcp_resource_templates uses `resourceTemplates` and defaults a missing name",
        "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>("alpha",
            std::vector<support::JsonValue>{},
            std::vector<support::JsonValue>{
                    object({{"uriTemplate", "db://{table}/rows"}, {"name", "rows"}}),
                    object({{"uriTemplate", "ui://widget/{id}"}, {"name", "widget"}, {"mimeType", "text/html;profile=mcp-app"}}),
            });
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto outcome = run_tool(runtime, tools[1], object({}));
    REQUIRE(outcome.has_value());
    const support::JsonValue expected = object({
            {"resourceTemplates",
                    array({object({{"server", "alpha"}, {"uriTemplate", "db://{table}/rows"}, {"name", "rows"}})})},
    });
    const auto mismatch = tests::json_mismatch(expected, payload_of(*outcome));
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
}

TEST_CASE("read_mcp_resource returns the contents shape with `_meta` stripped", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>("alpha", std::vector<support::JsonValue>{});
    alpha->reads.emplace("file:///a.txt",
            std::vector<support::JsonValue>{object({{"uri", "file:///a.txt"},
                                                    {"mimeType", "text/plain"},
                                                    {"text", "hello"},
                                                    {"_meta", object({{"x", 1}})}})});
    alpha->reads.emplace("blob://image",
            std::vector<support::JsonValue>{
                    object({{"uri", "blob://image"}, {"mimeType", "image/png"}, {"blob", "aGVsbG8="}})});
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto text = run_tool(runtime, tools[2], object({{"server", "alpha"}, {"uri", "file:///a.txt"}}));
    REQUIRE(text.has_value());
    const support::JsonValue expected = object({
            {"server", "alpha"},
            {"uri", "file:///a.txt"},
            {"contents", array({object({{"uri", "file:///a.txt"}, {"mimeType", "text/plain"}, {"text", "hello"}})})},
    });
    const auto mismatch = tests::json_mismatch(expected, payload_of(*text));
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
    CHECK(tool_text(*text) == std::string{"hello"});

    // A base64 blob from an image mime becomes an image block, and the JSON
    // payload keeps the base64 blob.
    auto blob = run_tool(runtime, tools[2], object({{"server", "alpha"}, {"uri", "blob://image"}}));
    REQUIRE(blob.has_value());
    REQUIRE(blob->content.size() == 1);
    CHECK(std::holds_alternative<ai::ImageContent>(blob->content[0]));
    const auto payload = payload_of(*blob);
    const auto contents = payload.get_object().at("contents").get_array();
    REQUIRE(contents.size() == 1);
    CHECK(contents[0].get_object().at("blob").get_string() == "aGVsbG8=");
}

TEST_CASE("read_mcp_resource requires server and uri", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>("alpha", std::vector<support::JsonValue>{});
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto missing_server = run_tool(runtime, tools[2], object({{"uri", "file:///a.txt"}}));
    REQUIRE_FALSE(missing_server.has_value());
    CHECK(missing_server.error().message == "server must be provided");

    auto missing_uri = run_tool(runtime, tools[2], object({{"server", "alpha"}}));
    REQUIRE_FALSE(missing_uri.has_value());
    CHECK(missing_uri.error().message == "uri must be provided");
}

TEST_CASE("a listed resource with no name defaults it from the uri", "[coding_agent][mcp][issue884][spec]") {
    tests::RuntimeFixture runtime;
    auto alpha = std::make_shared<ScriptedResourceServer>(
            "alpha", std::vector<support::JsonValue>{object({{"uri", "notes://scratch"}})});
    auto tools = coding_agent::mcp::create_mcp_resource_tools({alpha});

    auto outcome = run_tool(runtime, tools[0], object({{"server", "alpha"}}));
    REQUIRE(outcome.has_value());
    const auto resources = payload_of(*outcome).get_object().at("resources").get_array();
    REQUIRE(resources.size() == 1);
    CHECK(resources[0].get_object().at("name").get_string() == "notes://scratch");
}

TEST_CASE("the real stdio fixture lists, templates, and reads over `resources/*`",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto client = tests::run_awaitable(runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config()));
    REQUIRE(client.has_value());
    auto server = std::make_shared<coding_agent::mcp::McpConnectionResourceServer>(*client);
    auto tools = coding_agent::mcp::create_mcp_resource_tools({server});

    // Connected-time snapshot: the `ui://` and `profile=mcp-app` entries are
    // filtered out of the resources and templates the server lists.
    auto snapshot = tests::run_awaitable(
            runtime, support::detail::await_async_result(
                             coding_agent::mcp::fetch_mcp_resource_snapshot(*server, std::stop_token{})));
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->has_resources);
    REQUIRE(snapshot->resources.size() == 2);
    CHECK(snapshot->resources[0].get_object().at("uri").get_string() == "file:///docs/readme.md");
    CHECK(snapshot->resources[1].get_object().at("name").get_string() == "notes://scratch");
    REQUIRE(snapshot->resource_templates.size() == 1);

    // Every page of the one resource-bearing server, `_meta`/icons stripped.
    auto listed = run_tool(runtime, tools[0], object({}));
    REQUIRE(listed.has_value());
    const auto resources = payload_of(*listed).get_object().at("resources").get_array();
    REQUIRE(resources.size() == 2);
    CHECK(resources[0].get_object().at("server").get_string() == "echo");
    CHECK_FALSE(resources[0].get_object().contains("_meta"));
    CHECK_FALSE(resources[0].get_object().contains("icons"));

    auto templates = run_tool(runtime, tools[1], object({}));
    REQUIRE(templates.has_value());
    const auto listed_templates = payload_of(*templates).get_object().at("resourceTemplates").get_array();
    REQUIRE(listed_templates.size() == 1);
    CHECK(listed_templates[0].get_object().at("uriTemplate").get_string() == "db://{table}/rows");

    auto read = run_tool(
            runtime, tools[2], object({{"server", "echo"}, {"uri", "file:///docs/readme.md"}}));
    REQUIRE(read.has_value());
    const support::JsonValue expected = object({
            {"server", "echo"},
            {"uri", "file:///docs/readme.md"},
            {"contents",
                    array({object(
                            {{"uri", "file:///docs/readme.md"}, {"mimeType", "text/markdown"}, {"text", "# readme\n"}})})},
    });
    const auto mismatch = tests::json_mismatch(expected, payload_of(*read));
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
    CHECK(tool_text(*read) == std::string{"# readme\n"});
}

TEST_CASE("converted MCP tools declare pi's CallToolResult output schema",
        "[coding_agent][mcp][issue884][spec]") {
    // A server tool with no `outputSchema` still declares the MCP result shape.
    const auto without_structured = coding_agent::mcp::create_mcp_result_schema(std::nullopt);
    const support::JsonValue expected_without = object({
            {"type", "object"},
            {"properties",
                    object({
                            {"content",
                                    object({{"type", "array"}, {"items", object({{"type", "object"}})}})},
                            {"isError", object({{"type", "boolean"}})},
                            {"_meta", object({{"type", "object"}})},
                    })},
            {"required", array({"content"})},
    });
    CHECK_FALSE(tests::json_mismatch(expected_without, without_structured).has_value());

    // A server tool's own `outputSchema` nests as `structuredContent`.
    const support::JsonValue structured = object({{"type", "object"}, {"properties", object({{"total", object({{"type", "number"}})}})}});
    const auto with_structured = coding_agent::mcp::create_mcp_result_schema(structured);
    const auto structured_property = with_structured.get_object()
                                             .at("properties")
                                             .get_object()
                                             .find("structuredContent");
    REQUIRE(structured_property != with_structured.get_object().at("properties").get_object().end());
    CHECK_FALSE(tests::json_mismatch(structured, structured_property->second).has_value());
}

TEST_CASE("a connected MCP tool's Agent-visible definition carries the output schema",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto source = tests::run_awaitable(
            runtime, coding_agent::mcp::McpExtensionToolSource::connect_stdio(echo_server_config()));
    REQUIRE(source.has_value());
    auto loaded = source.value()->load_tools();
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->empty());
    for (const auto& tool : *loaded) {
        REQUIRE(tool.definition.output_schema.has_value());
        const auto& schema = *tool.definition.output_schema;
        CHECK(schema.get_object().at("type").get_string() == "object");
        CHECK_FALSE(tests::json_mismatch(array({"content"}), schema.get_object().at("required")).has_value());
    }
}
