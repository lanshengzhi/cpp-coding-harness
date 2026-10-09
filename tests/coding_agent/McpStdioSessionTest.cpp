// Spec #865 second slice (#869): MCP server over stdio end to end. The fixture
// is a real MCP stdio child (`fixtures/pi-mcp/echo_server.py`), launched by the
// product transport on the Session's Runtime loop, handshaken over the wire,
// and converted into Agent tools through the #867 Extension Tool Source seam.
// The scripted fake provider serves the model, so no live keys or network are
// used.
//
// The acceptance case pairs "the server is registered and its tools are on the
// session's surface" with "a session assembled without the server does not
// expose them": a tool's existence is not what makes it visible, registration
// at assembly is.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/JsonCompare.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ToolRendererFixture.hpp"

#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

[[nodiscard]] std::string read_fixture_text(std::string_view name) {
    std::ifstream input(fixture_path(name), std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] coding_agent::mcp::McpStdioServerConfig echo_server_config() {
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_path("echo_server.py")};
    return config;
}

/// Scripted provider that calls one named tool (with fixed arguments) on its
/// first request and answers a plain text turn afterwards, so one prompt
/// exercises the MCP discover -> call -> result round through the ordinary
/// executor path.
class McpToolRoundProvider final : public tests::ScriptedProvider {
public:
    McpToolRoundProvider(std::string tool_name, support::JsonValue arguments)
        : ScriptedProvider("fake"), tool_name_(std::move(tool_name)), arguments_(std::move(arguments)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        const std::string tool_name = tool_name_;
        const support::JsonValue arguments = arguments_;
        auto serialized = support::write_json(arguments);
        const std::string raw_arguments = serialized ? std::move(*serialized) : std::string{"{}"};
        return ai::detail::make_model_stream(
                [model = std::move(model), request, tool_name, arguments, raw_arguments](
                        ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling the MCP tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_mcp",
                                .name = tool_name,
                                .arguments = arguments,
                                .raw_arguments = raw_arguments,
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else {
                        round.content = {ai::text_content("done")};
                        round.stop_reason = ai::AssistantStopReason::Stop;
                    }
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return round;
                });
    }

private:
    std::string tool_name_;
    support::JsonValue arguments_;
    int request_count_{0};
};

[[nodiscard]] bool session_exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::optional<std::string> tool_result_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return ai::text_from_content(result->content);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool tool_result_is_error(const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return result->is_error;
        }
    }
    return false;
}

/// The text of the last assistant message, so a case can assert the turn's
/// final answer without assuming the transcript's tail type.
[[nodiscard]] std::optional<std::string> last_assistant_text(const coding_agent::AgentSession& session) {
    const auto& messages = session.snapshot().agent_state.messages;
    for (auto iterator = messages.rbegin(); iterator != messages.rend(); ++iterator) {
        if (const auto* message = std::get_if<ai::AssistantMessage>(&*iterator)) {
            return ai::text_from_assistant_content(message->content);
        }
    }
    return std::nullopt;
}

/// Assemble one in-memory session with the given MCP servers configured and no
/// other tool contributions. Every caller holds a clean HOME so settings
/// isolation matches the rest of the coding-agent shard.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_mcp_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<coding_agent::mcp::McpStdioServerConfig> servers) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_servers = std::move(servers);
    auto models = tests::models_from_provider(std::move(provider));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

[[nodiscard]] support::JsonValue descriptor_json(const coding_agent::mcp::McpToolDescriptor& descriptor) {
    return support::JsonValue{support::JsonValue::object_t{
            {"server_tool_name", descriptor.server_tool_name},
            {"name", descriptor.full_name},
            {"description", descriptor.description},
            {"parameters", descriptor.parameters},
    }};
}

} // namespace

TEST_CASE("connecting an MCP stdio server lists its tools with the pi naming and schema",
        "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto source = tests::run_awaitable(
            runtime, coding_agent::mcp::McpExtensionToolSource::connect_stdio(echo_server_config()));
    REQUIRE(source.has_value());
    REQUIRE(source.value() != nullptr);

    support::JsonValue actual{support::JsonValue::object_t{
            {"server", source.value()->server_name()},
            {"tools", support::JsonValue::array_t{}},
    }};
    auto& tools = actual.get_object().at("tools").get_array();
    for (const auto& descriptor : source.value()->tools()) {
        tools.push_back(descriptor_json(descriptor));
    }

    auto golden = support::read_json(read_fixture_text("golden/discovered-tools.json"));
    REQUIRE(golden.has_value());
    const auto mismatch = tests::json_mismatch(*golden, actual);
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
}

TEST_CASE(
        "an MCP server tool is discoverable and callable in the Agent Session", "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{{"text", "hello"}}};
    auto session = make_mcp_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments),
            {echo_server_config()});

    // Discoverable: the server's tool is on the Agent's active tool surface
    // under its pi `mcp__<server>__<tool>` name.
    CHECK(session_exposes_tool(*session, "mcp__echo__echo"));

    // Callable through the ordinary executor path: the model's call runs
    // `tools/call` on the live server and the echoed text lands as the tool
    // result.
    REQUIRE(tests::run_awaitable(runtime, session->prompt("use the MCP echo tool")).has_value());
    CHECK(tool_result_text(*session, "mcp__echo__echo") == std::optional<std::string>{"hello"});
    CHECK_FALSE(tool_result_is_error(*session, "mcp__echo__echo"));

    session->close();
}

TEST_CASE("an MCP tool renders through the existing fallback renderer, not a new seam",
        "[coding_agent][mcp][issue869][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // MCP tools register no named renderer, exactly like pi's MCP tools: the
    // default registry falls back for the `mcp__*` name.
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "mcp__echo__echo", "call_mcp", R"({"text":"hello"})", "/workspace");

    const auto screen = tests::render_tool_screen(component, 80);

    auto golden = support::read_json(read_fixture_text("golden/tool-call-render.json"));
    REQUIRE(golden.has_value());
    REQUIRE(golden->get_if<support::JsonValue::array_t>() != nullptr);
    std::vector<std::string> expected;
    for (const auto& line : golden->get_array()) {
        REQUIRE(line.holds<std::string>());
        expected.push_back(line.get_string());
    }
    CHECK(screen.visible == expected);
}

TEST_CASE(
        "a failing MCP tool call is isolated and does not end the Agent Turn", "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    // `crash` exits the server without responding: the in-flight request fails
    // with the connection closing, which is the strongest per-call failure.
    support::JsonValue arguments{support::JsonValue::object_t{}};
    auto session = make_mcp_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__crash", arguments),
            {echo_server_config()});
    REQUIRE(session_exposes_tool(*session, "mcp__echo__crash"));

    REQUIRE(tests::run_awaitable(runtime, session->prompt("crash the MCP server")).has_value());

    // The failing call produced an error tool result and the turn continued to
    // the model's next answer. The message names the closed connection, so the
    // error came from the transport (the server exited), not from a rejected
    // call that never reached it.
    CHECK(tool_result_is_error(*session, "mcp__echo__crash"));
    const auto crash_text = tool_result_text(*session, "mcp__echo__crash").value_or("<none>");
    CHECK(crash_text.find("connection closed") != std::string::npos);
    CHECK(last_assistant_text(*session) == std::optional<std::string>{"done"});
    CHECK(session->is_open());

    session->close();
}

TEST_CASE("a tool-level MCP error result is reported as an error but the Turn continues",
        "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    // `fail` returns a well-formed `tools/call` result with `isError: true`:
    // the execute succeeds and the error flag flows through to the model.
    support::JsonValue arguments{support::JsonValue::object_t{}};
    auto session = make_mcp_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__fail", arguments),
            {echo_server_config()});

    REQUIRE(tests::run_awaitable(runtime, session->prompt("fail the MCP tool")).has_value());
    CHECK(tool_result_is_error(*session, "mcp__echo__fail"));
    CHECK(tool_result_text(*session, "mcp__echo__fail") == std::optional<std::string>{"fail: requested failure"});
    CHECK(session->is_open());

    session->close();
}

TEST_CASE("an MCP process failure includes a bounded stderr tail", "[coding_agent][mcp][issue911]") {
    tests::TempWorkspace workspace;
    const auto script = workspace.path() / "stderr_server.py";
    {
        std::ofstream output(script);
        output << "import os, sys\n"
                  "sys.stderr.write('x' * 70000 + 'TAIL_MARKER')\n"
                  "sys.stderr.flush()\n"
                  "os._exit(3)\n";
    }
    auto config = echo_server_config();
    config.args = {script.string()};
    tests::RuntimeFixture runtime;

    auto connected = tests::run_awaitable(runtime, coding_agent::mcp::McpStdioClient::connect(std::move(config)));

    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error().message.find("TAIL_MARKER") != std::string::npos);
    CHECK(connected.error().message.find(std::string(2001, 'x')) == std::string::npos);
}

TEST_CASE("a malformed server frame is a recoverable transport error", "[coding_agent][mcp][issue869][protocol]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto client = tests::run_awaitable(runtime, coding_agent::mcp::McpStdioClient::connect(echo_server_config()));
    REQUIRE(client.has_value());

    // The server writes a non-JSON line before the valid response. pi keeps
    // serving and the pending request still resolves; a run that no-op'd the
    // transport could not produce this value.
    auto response = tests::run_awaitable(
            runtime, support::detail::await_async_result((*client)->request("debug/emit_garbage")));
    REQUIRE(response.has_value());
    const auto* object = response->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto after = object->find("after_garbage");
    REQUIRE(after != object->end());
    REQUIRE(after->second.get_if<bool>() != nullptr);
    CHECK(*after->second.get_if<bool>());
}

TEST_CASE("an unreachable MCP server fails Session Assembly explicitly, never silently",
        "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    coding_agent::mcp::McpStdioServerConfig broken;
    broken.name = "broken";
    broken.command = "/nonexistent/pike-mcp-server-xyz";

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_servers = {broken};
    auto models = tests::models_from_provider(std::make_shared<McpToolRoundProvider>(
            "mcp__echo__echo", support::JsonValue{support::JsonValue::object_t{}}));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().message.find("broken") != std::string::npos);
}

TEST_CASE("an MCP tool is visible to the session only when its server is registered at assembly",
        "[coding_agent][mcp][issue869][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{{"text", "hello"}}};

    // A session with no MCP server configured does not expose the tool, even
    // though the fixture server exists and offers it.
    auto without = make_mcp_session(
            runtime, workspace, std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments), {});
    CHECK_FALSE(session_exposes_tool(*without, "mcp__echo__echo"));
    without->close();

    // A session assembled with the server exposes it. Together the two cases
    // separate registration-at-assembly (the property) from the server's
    // existence.
    auto with = make_mcp_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments),
            {echo_server_config()});
    CHECK(session_exposes_tool(*with, "mcp__echo__echo"));
    with->close();
}
