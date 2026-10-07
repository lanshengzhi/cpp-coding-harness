// Spec #865 fourth slice (#876): MCP server management and persistence.
//
// The server list persists in pi's `mcp.json` (global `<agentDir>/mcp.json` and
// trusted-project `<cwd>/.pi/mcp.json`), not in `settings.json`; Session
// Assembly reloads it, so the list survives a restart. Every configured server
// reports an explicit lifecycle state (running / stopped / failed) on the
// creation result and as a per-state Session diagnostic, so a disabled or dead
// server stays visible instead of silently vanishing. A persisted server whose
// command now fails does not veto the session: it reports failed while the
// remaining servers connect. A server that dies mid-session surfaces the
// transport error on that call and reconnects on the next one.
//
// The acceptance cases pair each property with a separation case: "survives
// restart" vs. a list that would only work once; "stopped/failed keeps its
// entry visible" vs. a server that silently disappears; "the next call
// reconnects" vs. an error that swallowed the server for the rest of the
// session.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpServerStatus.hpp"
#include "coding_agent/mcp/McpStdioClient.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/JsonCompare.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string fixture_dir() { return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp"; }

[[nodiscard]] std::string read_fixture_text(std::string_view name) {
    std::ifstream input(fixture_dir() + "/" + std::string{name}, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Project one loaded entry onto a deterministic, path-free shape so the
/// fixture golden pins behavior rather than the machine's absolute path.
[[nodiscard]] support::JsonValue server_projection(const coding_agent::mcp::McpConfigEntry& entry) {
    support::JsonValue value{support::JsonValue::object_t{
            {"name", entry.name},
            {"enabled", entry.enabled},
    }};
    auto& object = value.get_object();
    if (const auto* stdio = std::get_if<coding_agent::mcp::McpStdioServerConfig>(&entry.config)) {
        object.emplace("kind", "stdio");
        object.emplace("command", stdio->command);
        support::JsonValue args{support::JsonValue::array_t{}};
        for (const auto& argument : stdio->args) {
            args.get_array().emplace_back(argument);
        }
        object.emplace("args", std::move(args));
    } else {
        const auto& http = std::get<coding_agent::mcp::McpHttpServerConfig>(entry.config);
        object.emplace("kind", "http");
        object.emplace("url", http.url);
        support::JsonValue headers{support::JsonValue::object_t{}};
        for (const auto& [name, header] : http.headers) {
            headers.get_object().emplace(name, header);
        }
        object.emplace("headers", std::move(headers));
    }
    return value;
}

[[nodiscard]] std::vector<std::string> server_names(const coding_agent::mcp::McpConfigLoad& load) {
    std::vector<std::string> names;
    names.reserve(load.servers.size());
    for (const auto& entry : load.servers) {
        names.push_back(entry.name);
    }
    return names;
}

/// Write a global `mcp.json` under the XDG root the test's EnvVarGuard points
/// at, mirroring pi's `<agentDir>/mcp.json`.
void write_global_mcp_json(const tests::TempWorkspace& workspace, std::string_view content) {
    const auto agent_dir = workspace.path() / "xdg" / "pike" / "agent";
    std::filesystem::create_directories(agent_dir);
    std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
    output << content;
}

[[nodiscard]] std::string echo_server_mcp_json(std::string_view server_name,
        bool enabled,
        std::optional<std::string_view> command_override = std::nullopt,
        std::optional<std::string_view> exposure = std::nullopt) {
    const std::string command = command_override ? std::string{*command_override} : std::string{"python3"};
    std::string json = "{\n  \"mcpServers\": {\n    \"" + std::string{server_name} + "\": {\n      \"command\": \"" +
                       command + "\"";
    if (!command_override) {
        json += ",\n      \"args\": [\"" + fixture_dir() + "/echo_server.py\"]";
    }
    if (exposure) {
        json += ",\n      \"exposure\": \"" + std::string{*exposure} + "\"";
    }
    if (!enabled) {
        json += ",\n      \"enabled\": false";
    }
    json += "\n    }\n  }\n}\n";
    return json;
}

/// A scripted provider that serves plain text turns; it never calls a tool, so
/// the cases that only assert the persisted server surface never depend on the
/// provider's shape.
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

/// Two tool calls in sequence, then a text turn: request 0 calls `crash` (which
/// kills the server), request 1 calls `echo` (which must reconnect), request 2
/// answers. This drives reconnect-after-death through the ordinary executor.
class CrashThenEchoProvider final : public tests::ScriptedProvider {
public:
    CrashThenEchoProvider() : ScriptedProvider("fake") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        return ai::detail::make_model_stream(
                [model = std::move(model), request](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("crashing")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_crash",
                                .name = "mcp__echo__crash",
                                .arguments = support::JsonValue{support::JsonValue::object_t{}},
                                .raw_arguments = "{}",
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else if (request == 1) {
                        round.content = {ai::text_content("echoing")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_echo",
                                .name = "mcp__echo__echo",
                                .arguments = support::JsonValue{support::JsonValue::object_t{{"text", "reconnected"}}},
                                .raw_arguments = R"({"text":"reconnected"})",
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
    int request_count_{0};
};

struct SessionAttempt {
    std::unique_ptr<coding_agent::AgentSession> session;
    std::vector<coding_agent::SessionDiagnostic> diagnostics;
    std::vector<coding_agent::mcp::McpServerStatus> mcp_servers;
};

/// Assemble one in-memory session that picks up the persisted `mcp.json` (the
/// production path; no explicit request-seam servers), keeping every other
/// contribution empty.
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

[[nodiscard]] bool session_exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::optional<coding_agent::mcp::McpServerStatus> status_for(
        const std::vector<coding_agent::mcp::McpServerStatus>& statuses, std::string_view name) {
    for (const auto& status : statuses) {
        if (status.name == name) {
            return status;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool has_diagnostic(const std::vector<coding_agent::SessionDiagnostic>& diagnostics,
        std::string_view code,
        std::string_view fragment) {
    return std::ranges::any_of(diagnostics, [&](const coding_agent::SessionDiagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.message.find(fragment) != std::string::npos;
    });
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

} // namespace

// ── Configuration persistence ────────────────────────────────────────────────

TEST_CASE("the committed mcp.json fixture loads into the pi mcpServers shape", "[coding_agent][mcp][issue876][spec]") {
    auto load = coding_agent::mcp::load_mcp_config(fixture_dir(), /* cwd */ {}, /* project_trusted */ false);
    REQUIRE(load.errors.empty());

    support::JsonValue actual{support::JsonValue::array_t{}};
    for (const auto& entry : load.servers) {
        actual.get_array().push_back(server_projection(entry));
    }
    auto golden = support::read_json(read_fixture_text("golden/mcp-config-servers.json"));
    REQUIRE(golden.has_value());
    const auto mismatch = tests::json_mismatch(*golden, actual);
    CHECK_FALSE(mismatch.has_value());
    if (mismatch) {
        INFO(*mismatch);
    }
}

TEST_CASE("a project mcp.json is read only while the project is trusted", "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json", R"({"mcpServers": {"global-server": {"command": "python3"}}})");
    workspace.write(".pi/mcp.json", R"({"mcpServers": {"project-server": {"command": "python3"}}})");

    const auto untrusted = coding_agent::mcp::load_mcp_config(
            workspace.path() / "home", workspace.path(), /* project_trusted */ false);
    CHECK(server_names(untrusted) == std::vector<std::string>{"global-server"});
    CHECK_FALSE(untrusted.project_config.has_value());

    const auto trusted =
            coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), /* project_trusted */ true);
    CHECK(server_names(trusted) == std::vector<std::string>{"global-server", "project-server"});
    REQUIRE(trusted.project_config.has_value());
    CHECK(*trusted.project_config == workspace.path() / ".pi" / "mcp.json");
}

TEST_CASE("a project entry overrides only the enabled flag of a global server", "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json", R"({"mcpServers": {"srv": {"command": "my-server", "args": ["--flag"]}}})");
    workspace.write(".pi/mcp.json", R"({"mcpServers": {"srv": {"enabled": false}}})");

    // Untrusted: the project override cannot reach the global entry, so the
    // server stays enabled — the separation case for the trust gate.
    const auto untrusted = coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), false);
    REQUIRE(untrusted.servers.size() == 1);
    CHECK(untrusted.servers.front().enabled);

    const auto trusted = coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), true);
    REQUIRE(trusted.servers.size() == 1);
    CHECK_FALSE(trusted.servers.front().enabled);
    const auto* stdio = std::get_if<coding_agent::mcp::McpStdioServerConfig>(&trusted.servers.front().config);
    REQUIRE(stdio != nullptr);
    CHECK(stdio->command == "my-server");
    CHECK(stdio->args == std::vector<std::string>{"--flag"});
}

TEST_CASE("a malformed mcp.json entry is an explicit error and the valid entries still load",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("home/mcp.json", R"({"mcpServers": {"bad": {"args": []}, "good": {"command": "python3"}}})");

    const auto load = coding_agent::mcp::load_mcp_config(workspace.path() / "home", workspace.path(), false);
    REQUIRE(load.errors.size() == 1);
    CHECK(load.errors.front().find("\"bad\"") != std::string::npos);
    // The bad entry is rejected and named; the good one is not dropped with it.
    CHECK(server_names(load) == std::vector<std::string>{"good"});
}

// ── Session lifecycle ────────────────────────────────────────────────────────

TEST_CASE("a persisted MCP server survives restart and reports the running state",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    // `direct` exposure declares the tool to the model; the default `codemode`
    // exposure registers it undeclared (the activation slice's separation
    // cases live in McpSessionIntegrationTest).
    write_global_mcp_json(
            workspace, echo_server_mcp_json("echo", /* enabled */ true, std::nullopt, /* exposure */ "direct"));
    tests::RuntimeFixture runtime;

    // First session sees the persisted server.
    {
        auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());
        CHECK(session_exposes_tool(*attempt.session, "mcp__echo__echo"));
        const auto status = status_for(attempt.mcp_servers, "echo");
        REQUIRE(status.has_value());
        CHECK(status->state == coding_agent::mcp::McpServerState::Running);
        attempt.session->close();
    }

    // A fresh session (the restart) sees it again, so the list persisted rather
    // than surviving only for the first assembly.
    {
        auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());
        CHECK(session_exposes_tool(*attempt.session, "mcp__echo__echo"));
        CHECK(status_for(attempt.mcp_servers, "echo")->state == coding_agent::mcp::McpServerState::Running);
        attempt.session->close();
    }
}

TEST_CASE("a server disabled in mcp.json is stopped, its tools are absent, and a notice names it",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_server_mcp_json("echo", /* enabled */ false));
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    // The tool is absent from the model's surface...
    CHECK_FALSE(session_exposes_tool(*attempt.session, "mcp__echo__echo"));
    // ...but the server did not disappear: it is stopped with an explicit
    // notice naming the file. A silent skip would satisfy the tool-absence
    // check while violating the property.
    const auto status = status_for(attempt.mcp_servers, "echo");
    REQUIRE(status.has_value());
    CHECK(status->state == coding_agent::mcp::McpServerState::Stopped);
    CHECK(has_diagnostic(attempt.diagnostics, "mcp:stopped", "echo"));
    CHECK(has_diagnostic(attempt.diagnostics, "mcp:stopped", "mcp.json"));

    attempt.session->close();
}

TEST_CASE("a persisted server whose command now fails surfaces failed state and the session still starts",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    // Two entries: one whose executable is gone, one good. The dead entry must
    // surface as failed and must not veto the session or disappear silently.
    std::string config = "{\n  \"mcpServers\": {\n";
    config += "    \"broken\": {\"command\": \"/nonexistent/pike-mcp-server-xyz\"},\n";
    config += "    \"echo\": {\"command\": \"python3\", \"args\": [\"" + fixture_dir() +
              "/echo_server.py\"], \"exposure\": \"direct\"}\n";
    config += "  }\n}\n";
    write_global_mcp_json(workspace, config);
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<PlainTextProvider>());

    const auto failed = status_for(attempt.mcp_servers, "broken");
    REQUIRE(failed.has_value());
    CHECK(failed->state == coding_agent::mcp::McpServerState::Failed);
    CHECK(has_diagnostic(attempt.diagnostics, "mcp:failed", "broken"));

    // The good server still connected and its tools are usable.
    const auto running = status_for(attempt.mcp_servers, "echo");
    REQUIRE(running.has_value());
    CHECK(running->state == coding_agent::mcp::McpServerState::Running);
    CHECK(session_exposes_tool(*attempt.session, "mcp__echo__echo"));

    attempt.session->close();
}

// ── Reconnect after server death ─────────────────────────────────────────────

TEST_CASE("a dead stdio server reports the call error and reconnects on the next call",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_dir() + "/echo_server.py"};
    auto client = tests::run_awaitable(runtime, coding_agent::mcp::McpStdioClient::connect(config));
    REQUIRE(client.has_value());

    // `crash` exits the server without responding: the in-flight call fails
    // explicitly (never a hang).
    support::JsonValue crash_params{support::JsonValue::object_t{
            {"name", "crash"},
            {"arguments", support::JsonValue::object_t{}},
    }};
    auto crashed = tests::run_awaitable(
            runtime, support::detail::await_async_result((*client)->request("tools/call", std::move(crash_params))));
    REQUIRE_FALSE(crashed.has_value());
    CHECK((*client)->closed());

    // The next call reconnects the server (pi `connection.reconnect()`), so the
    // dead server did not end the connection for the rest of the session.
    support::JsonValue echo_params{support::JsonValue::object_t{
            {"name", "echo"},
            {"arguments", support::JsonValue::object_t{{"text", "after-reconnect"}}},
    }};
    auto echoed = tests::run_awaitable(
            runtime, support::detail::await_async_result((*client)->request("tools/call", std::move(echo_params))));
    REQUIRE(echoed.has_value());
    const auto* object = echoed->get_if<support::JsonValue::object_t>();
    REQUIRE(object != nullptr);
    const auto content = object->find("content");
    REQUIRE(content != object->end());
    REQUIRE(content->second.get_if<support::JsonValue::array_t>() != nullptr);
    REQUIRE_FALSE(content->second.get_array().empty());
    const auto text = content->second.get_array().front().get_if<support::JsonValue::object_t>();
    REQUIRE(text != nullptr);
    CHECK(text->at("text").get_string() == "after-reconnect");
    CHECK_FALSE((*client)->closed());
}

TEST_CASE("a server that dies mid-session does not end the session and reconnects",
        "[coding_agent][mcp][issue876][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(
            workspace, echo_server_mcp_json("echo", /* enabled */ true, std::nullopt, /* exposure */ "direct"));
    tests::RuntimeFixture runtime;

    auto attempt = make_persisted_session(runtime, workspace, std::make_shared<CrashThenEchoProvider>());
    CHECK(session_exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK(session_exposes_tool(*attempt.session, "mcp__echo__crash"));

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("crash then echo")).has_value());

    // The dead call is an explicit error, the following call reconnected and
    // succeeded, and the session is still open.
    CHECK(tool_result_is_error(*attempt.session, "mcp__echo__crash"));
    CHECK(tool_result_text(*attempt.session, "mcp__echo__echo") == std::optional<std::string>{"reconnected"});
    CHECK_FALSE(tool_result_is_error(*attempt.session, "mcp__echo__echo"));
    CHECK(attempt.session->is_open());

    attempt.session->close();
}
