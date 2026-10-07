// The ACTIVATION + INTEGRATION slice end to end (spec #882, ticket #884):
// the live MCP session manager constructed in SessionFactory, bound to the
// Agent's live tool surface in `bind_assembly`, connecting the persisted
// `mcp.json` servers through the production factory (scripted fake servers
// only — the python echo fixture), activating `codemode` from the configured
// exposure (pi `ensureDiscoveryActive`), registering the resource tools at
// the widest non-hidden exposure, and feeding the `mcp_servers` system
// prompt section from the live manager state.
//
// Every case is paired with the case the property separates: the default
// session declares the fixed four-tool loadout while a codemode-exposure
// server's tool stays undeclared yet reachable from a codemode script; an
// `autoEnableCodemode:false` config records pi's verbatim warning while the
// tools still register; a hidden exposure renders no section; a direct
// exposure declares both the server tools and the resource tools, and a
// declared `list_mcp_resources` call reads the scripted server's resources.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

struct SessionAttempt {
    std::unique_ptr<coding_agent::AgentSession> session;
    std::vector<coding_agent::SessionDiagnostic> diagnostics;
    std::vector<coding_agent::mcp::McpServerStatus> mcp_servers;
};

[[nodiscard]] std::string fixture_dir() { return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp"; }

/// Write a global `mcp.json` under the XDG root the test's EnvVarGuard points
/// at, mirroring pi's `<agentDir>/mcp.json`.
void write_global_mcp_json(const tests::TempWorkspace& workspace, std::string_view content) {
    const auto agent_dir = workspace.path() / "xdg" / "pike" / "agent";
    std::filesystem::create_directories(agent_dir);
    std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
    output << content;
}

/// The echo fixture as a persisted stdio server; `exposure` and
/// `auto_enable_codemode` default to pi's (codemode / true).
[[nodiscard]] std::string echo_mcp_json(std::optional<std::string_view> exposure = std::nullopt,
        bool auto_enable_codemode = true) {
    std::string json = "{\n  \"mcpServers\": {\n    \"echo\": {\n      \"command\": \"python3\",\n      \"args\": [\"" +
                       fixture_dir() + "/echo_server.py\"]";
    if (exposure) {
        json += ",\n      \"exposure\": \"" + std::string{*exposure} + "\"";
    }
    json += "\n    }\n  }";
    if (!auto_enable_codemode) {
        json += ",\n  \"autoEnableCodemode\": false";
    }
    json += "\n}\n";
    return json;
}

/// Assemble one in-memory session that picks up the persisted `mcp.json`.
[[nodiscard]] SessionAttempt make_persisted_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        const std::shared_ptr<tests::ScriptedProvider>& provider) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    auto models = tests::models_from_provider(provider);
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return SessionAttempt{
            .session = std::move(created->session),
            .diagnostics = std::move(created->diagnostics),
            .mcp_servers = std::move(created->mcp_servers),
    };
}

[[nodiscard]] bool exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::optional<std::string> section_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* system = std::get_if<ai::SystemMessage>(&message);
        if (system == nullptr) {
            continue;
        }
        for (const auto& section : system->sections) {
            if (section.name == name) {
                return section.text;
            }
        }
    }
    return std::nullopt;
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

/// A scripted provider that serves plain text turns; it never calls a tool.
class PlainTextProvider final : public tests::ScriptedProvider {
public:
    PlainTextProvider() : ScriptedProvider("fake") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        return ai::detail::make_model_stream(
                [model = std::move(model)](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    round.content = {ai::text_content("done")};
                    round.stop_reason = ai::AssistantStopReason::Stop;
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return round;
                });
    }
};

/// One scripted tool call to `name` with `arguments`, then a plain answer.
class SingleCallProvider final : public tests::ScriptedProvider {
public:
    explicit SingleCallProvider(std::string name, support::JsonValue arguments, std::string raw)
        : ScriptedProvider("fake"), name_(std::move(name)), arguments_(std::move(arguments)), raw_(std::move(raw)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        return ai::detail::make_model_stream(
                [this, model = std::move(model), request](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_1",
                                .name = name_,
                                .arguments = arguments_,
                                .raw_arguments = raw_,
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
    std::string name_;
    support::JsonValue arguments_;
    std::string raw_;
    int request_count_{0};
};

} // namespace

TEST_CASE("a codemode-exposure server registers its tools undeclared, activates codemode, and renders the mcp_servers section",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json());
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    // pi `defaultActive:false` reconciliation: codemode is registered
    // (activation declares it below) and the server's tool stays undeclared
    // at the default codemode exposure.
    CHECK(exposes_tool(*attempt.session, "codemode"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));

    // The mcp_servers section lists the server and how its tools are reached.
    const auto section = section_text(*attempt.session, "mcp_servers");
    REQUIRE(section.has_value());
    CHECK(section->find("MCP servers whose tools are not declared to you.") != std::string::npos);
    CHECK(section->find("Call the tools of `codemode` servers from codemode scripts.") != std::string::npos);
    CHECK(section->find("- mcp__echo (codemode)") != std::string::npos);

    // The connect phase resolved before creation: the server is running.
    const auto running = std::ranges::find_if(attempt.mcp_servers, [](const auto& status) {
        return status.name == "echo";
    });
    REQUIRE(running != attempt.mcp_servers.end());
    CHECK(running->state == coding_agent::mcp::McpServerState::Running);

    attempt.session->close();
}

TEST_CASE("a codemode-exposure MCP tool is reachable from a codemode script even though it is undeclared",
        "[coding_agent][mcp][issue884][spec]") {
    // The separation case for the declared-set check above: the model cannot
    // see `mcp__echo__echo`, yet a codemode script reaches it through the
    // registered-tool surface (pi: either discovery tool reaches every such
    // tool).
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json());
    tests::RuntimeFixture runtime;

    // pi's codemode routing hands the guest the tool's structured details, or
    // its text: the echo fixture returns no structuredContent, so the script
    // sees the echoed text itself.
    const std::string script =
            "const r = await tools.mcp__echo__echo({ text: \"from-script\" });"
            " text(\"echoed:\" + r);"
            " return \"done\";";
    support::JsonValue arguments{support::JsonValue::object_t{{"code", script}}};
    auto raw = support::write_json(arguments);
    REQUIRE(raw.has_value());
    auto attempt = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>(
                    "codemode", std::move(arguments), std::move(*raw)));

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("run the script")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    // The scripted echo server answered through the live manager connection:
    // the undeclared tool ran, proving reachability the declared set cannot
    // show.
    CHECK(result->find("echoed:from-script") != std::string::npos);

    attempt.session->close();
}

TEST_CASE("autoEnableCodemode false keeps codemode inactive and records pi's verbatim warning on the manager",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json(std::nullopt, /* auto_enable_codemode */ false));
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    CHECK_FALSE(exposes_tool(*attempt.session, "codemode"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));
    auto* manager =
            coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    REQUIRE(manager != nullptr);
    REQUIRE(manager->warnings().size() == 1);
    CHECK(manager->warnings().front() ==
            "MCP tools are only reachable from the codemode or tool_search tool, but neither is active "
            "(autoEnableCodemode is false); they cannot be called.");

    attempt.session->close();
}

TEST_CASE("a hidden-exposure server registers its tools undeclared and renders no mcp_servers section",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("hidden"));
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    CHECK_FALSE(exposes_tool(*attempt.session, "codemode"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK_FALSE(section_text(*attempt.session, "mcp_servers").has_value());

    attempt.session->close();
}

TEST_CASE("a direct-exposure server declares its tools and the resource tools read the scripted server's resources",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("direct"));
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    // Direct tools are declared like built-ins (pi `_isActivatedOnRegistration`).
    CHECK(exposes_tool(*attempt.session, "mcp__echo__echo"));
    // The resource tools take the widest non-hidden exposure of the
    // resource-bearing servers: direct, so they are declared too.
    CHECK(exposes_tool(*attempt.session, "list_mcp_resources"));
    CHECK(exposes_tool(*attempt.session, "list_mcp_resource_templates"));
    CHECK(exposes_tool(*attempt.session, "read_mcp_resource"));
    // pi `renderServersSection` lists only the servers whose tools are not
    // declared: a direct-only server renders no section.
    CHECK_FALSE(section_text(*attempt.session, "mcp_servers").has_value());

    // The declared resource tool reads the scripted server's resources
    // through the live manager connection: the `ui://` MCP App resource is
    // filtered out and the nameless one is defaulted from its URI.
    auto attempt_call = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>(
                    "list_mcp_resources",
                    support::JsonValue{support::JsonValue::object_t{}},
                    "{}"));
    REQUIRE(tests::run_awaitable(runtime, attempt_call.session->prompt("list resources")).has_value());
    const auto listed = tool_result_text(*attempt_call.session, "list_mcp_resources");
    REQUIRE(listed.has_value());
    CHECK(listed->find("file:///docs/readme.md") != std::string::npos);
    CHECK(listed->find("notes://scratch") != std::string::npos);
    CHECK(listed->find("ui://widget/app.html") == std::string::npos);

    attempt.session->close();
    attempt_call.session->close();
}

TEST_CASE("a codemode-exposure resource-bearing server keeps the resource tools undeclared but reachable from scripts",
        "[coding_agent][mcp][issue884][spec]") {
    // The widest non-hidden exposure is codemode here, so the resource tools
    // register undeclared — and the separation case proves they still run:
    // a codemode script lists the scripted server's resources through them.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json());
    tests::RuntimeFixture runtime;

    // The resource tool answers with the Codex-compatible JSON text; the
    // script surfaces it as-is.
    const std::string script =
            "const r = await tools.list_mcp_resources({ server: \"echo\" });"
            " text(r);"
            " return \"done\";";
    support::JsonValue arguments{support::JsonValue::object_t{{"code", script}}};
    auto raw = support::write_json(arguments);
    REQUIRE(raw.has_value());
    auto attempt = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>(
                    "codemode", std::move(arguments), std::move(*raw)));

    CHECK_FALSE(exposes_tool(*attempt.session, "list_mcp_resources"));
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("list resources through a script")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    CHECK(result->find("file:///docs/readme.md") != std::string::npos);

    attempt.session->close();
}
