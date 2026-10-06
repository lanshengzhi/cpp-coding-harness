// Spec #865 slice (#871): tool selection flags and patterns. `--tools` /
// `--exclude-tools` resolve over the discovered tool set (built-ins plus any
// extension/MCP tools) at Session assembly; an unselected tool is absent from
// the Agent's surface and is reported, never removed silently. The fixture is
// the real MCP stdio echo server from #869, and the scripted fake provider
// serves the model, so no live keys or network are used.
//
// The acceptance cases pair each property with a case a cheap check would let
// through: the "allow pattern matches" case is paired with the deny that wins
// over it, and the "notice exists" case is paired with the removed tool being
// genuinely uncallable through the ordinary executor path.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/runtime/ToolSelection.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "support/AsyncResultBridge.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

[[nodiscard]] coding_agent::mcp::McpStdioServerConfig echo_server_config() {
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_path("echo_server.py")};
    return config;
}

/// `--tools` intent only (pi allowlist).
[[nodiscard]] coding_agent::runtime::ToolSelection allowed_only(std::vector<std::string> entries) {
    coding_agent::runtime::ToolSelection selection;
    selection.allowed = std::move(entries);
    return selection;
}

/// `--exclude-tools` intent only (pi denylist).
[[nodiscard]] coding_agent::runtime::ToolSelection excluded_only(std::vector<std::string> entries) {
    coding_agent::runtime::ToolSelection selection;
    selection.excluded = std::move(entries);
    return selection;
}

/// Scripted provider that calls one named tool on its first request and then
/// answers a plain text turn, so one prompt exercises a call against a tool
/// that may or may not be on the session's surface.
class ToolCallProvider final : public tests::ScriptedProvider {
public:
    ToolCallProvider(std::string tool_name, support::JsonValue arguments)
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
                    round.provider = "selection-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling the tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_selected",
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

[[nodiscard]] const coding_agent::SessionDiagnostic* find_diagnostic(
        const std::vector<coding_agent::SessionDiagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) {
            return &diagnostic;
        }
    }
    return nullptr;
}

/// Assemble one in-memory session with the given tool selection and MCP
/// servers, returning the creation bundle so a case can assert both the
/// session's surface and its diagnostics. Every caller holds a clean HOME.
[[nodiscard]] coding_agent::CreateAgentSessionResult make_selected_mcp_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        coding_agent::runtime::ToolSelection selection,
        std::vector<coding_agent::mcp::McpStdioServerConfig> servers) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_servers = std::move(servers);
    options.session_facts.tools = std::move(selection.allowed);
    options.session_facts.exclude_tools = std::move(selection.excluded);
    auto models = tests::models_from_provider(std::move(provider));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return std::move(*created);
}

} // namespace

TEST_CASE("tool_name_matches mirrors pi's exact-name and wildcard entry matcher",
        "[coding_agent][tools][issue871][spec]") {
    using coding_agent::runtime::tool_name_matches;

    const std::vector<std::string> entries{"read", "mcp__radius__*", "app.log"};
    CHECK(tool_name_matches(entries, "read"));
    CHECK_FALSE(tool_name_matches(entries, "reader"));
    CHECK(tool_name_matches(entries, "mcp__radius__search"));
    CHECK_FALSE(tool_name_matches(entries, "mcp__other__search"));
    CHECK(tool_name_matches(entries, "app.log"));
    // Anchored: an exact entry matches the whole name, not a substring.
    CHECK_FALSE(tool_name_matches(entries, "xapp.log"));
    // A trailing wildcard is anchored at the end; a leading one at the start.
    CHECK(tool_name_matches(std::vector<std::string>{"*.log"}, "app.log"));
    CHECK_FALSE(tool_name_matches(std::vector<std::string>{"*.log"}, "app.txt"));
    CHECK_FALSE(tool_name_matches(std::vector<std::string>{"read*"}, "xread"));
    CHECK(tool_name_matches(std::vector<std::string>{"*read"}, "xread"));
    CHECK(tool_name_matches(std::vector<std::string>{"*"}, "anything"));
    CHECK_FALSE(tool_name_matches(std::vector<std::string>{}, "read"));
}

TEST_CASE("resolve_tool_selection keeps MCP tools when the allowlist does not name MCP",
        "[coding_agent][tools][issue871][spec]") {
    using coding_agent::runtime::resolve_tool_selection;

    const std::vector<std::string> discovered{"read", "bash", "mcp__echo__echo", "mcp__echo__fail"};

    // `--tools read`: only the built-in is allowlisted; MCP tools stay because
    // no entry names `mcp__` (pi `_allowlistFiltersMcp`).
    auto escaped = resolve_tool_selection(allowed_only({"read"}), discovered);
    REQUIRE(escaped.has_value());
    CHECK(escaped->selected == std::vector<std::string>{"read", "mcp__echo__echo", "mcp__echo__fail"});
    CHECK(escaped->removed == std::vector<std::string>{"bash"});

    // Naming an `mcp__` entry subjects MCP tools to the allowlist.
    auto filtered = resolve_tool_selection(allowed_only({"mcp__echo__echo"}), discovered);
    REQUIRE(filtered.has_value());
    CHECK(filtered->selected == std::vector<std::string>{"mcp__echo__echo"});

    // An engaged empty allowlist allows nothing.
    auto empty = resolve_tool_selection(allowed_only({}), discovered);
    REQUIRE(empty.has_value());
    CHECK(empty->selected.empty());
}

TEST_CASE("resolve_tool_selection lets the denylist win over an allowlist match",
        "[coding_agent][tools][issue871][spec]") {
    using coding_agent::runtime::resolve_tool_selection;

    const std::vector<std::string> discovered{"read", "mcp__echo__echo", "mcp__echo__fail"};
    auto selection = allowed_only({"mcp__echo__*"});
    selection.excluded = std::vector<std::string>{"mcp__echo__fail"};

    auto resolved = resolve_tool_selection(selection, discovered);
    REQUIRE(resolved.has_value());
    // `mcp__echo__fail` string-matches the allow pattern, but selection is over
    // the resolved set: the exclusion removes it.
    CHECK(resolved->selected == std::vector<std::string>{"mcp__echo__echo"});
    CHECK(resolved->removed == std::vector<std::string>{"read", "mcp__echo__fail"});
}

TEST_CASE("resolve_tool_selection rejects a pattern that matches nothing", "[coding_agent][tools][issue871][spec]") {
    using coding_agent::runtime::resolve_tool_selection;

    const std::vector<std::string> discovered{"read", "mcp__echo__echo"};

    auto allowed = resolve_tool_selection(allowed_only({"mcp__nosuch__*"}), discovered);
    REQUIRE_FALSE(allowed.has_value());
    CHECK(allowed.error().code == support::ErrorCode::Validation);
    CHECK(allowed.error().message.find("matched no discovered tool") != std::string::npos);

    auto excluded = resolve_tool_selection(excluded_only({"mcp__nosuch__*"}), discovered);
    REQUIRE_FALSE(excluded.has_value());
    CHECK(excluded.error().code == support::ErrorCode::Validation);
}

TEST_CASE("an allowlisted MCP tool is selected and the rest of the surface is removed with a notice",
        "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{{"text", "hello"}}};
    auto created = make_selected_mcp_session(runtime,
            workspace,
            std::make_shared<ToolCallProvider>("mcp__echo__echo", arguments),
            allowed_only({"mcp__echo__echo"}),
            {echo_server_config()});

    CHECK(session_exposes_tool(*created.session, "mcp__echo__echo"));
    // Built-ins and the server's other tools are gone from the surface.
    CHECK_FALSE(session_exposes_tool(*created.session, "read"));
    CHECK_FALSE(session_exposes_tool(*created.session, "bash"));
    CHECK_FALSE(session_exposes_tool(*created.session, "mcp__echo__fail"));

    const auto* notice = find_diagnostic(created.diagnostics, "tool:selection");
    REQUIRE(notice != nullptr);
    CHECK(notice->message.find("bash") != std::string::npos);
    CHECK(notice->message.find("mcp__echo__fail") != std::string::npos);

    created.session->close();
}

TEST_CASE("an allowlist that names only built-ins keeps MCP tools (pi allowlist escape)",
        "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{}};
    auto created = make_selected_mcp_session(runtime,
            workspace,
            std::make_shared<ToolCallProvider>("mcp__echo__echo", arguments),
            allowed_only({"read"}),
            {echo_server_config()});

    CHECK(session_exposes_tool(*created.session, "read"));
    CHECK(session_exposes_tool(*created.session, "mcp__echo__echo"));
    CHECK(session_exposes_tool(*created.session, "mcp__echo__fail"));
    CHECK_FALSE(session_exposes_tool(*created.session, "bash"));

    created.session->close();
}

TEST_CASE("an MCP tool excluded by a pattern is absent while the built-ins remain",
        "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{}};
    auto created = make_selected_mcp_session(runtime,
            workspace,
            std::make_shared<ToolCallProvider>("mcp__echo__echo", arguments),
            excluded_only({"mcp__echo__*"}),
            {echo_server_config()});

    CHECK(session_exposes_tool(*created.session, "read"));
    CHECK(session_exposes_tool(*created.session, "bash"));
    CHECK_FALSE(session_exposes_tool(*created.session, "mcp__echo__echo"));
    CHECK_FALSE(session_exposes_tool(*created.session, "mcp__echo__fail"));

    const auto* notice = find_diagnostic(created.diagnostics, "tool:selection");
    REQUIRE(notice != nullptr);
    CHECK(notice->message.find("mcp__echo__echo") != std::string::npos);

    created.session->close();
}

TEST_CASE("the denylist removes a tool that an allowlist pattern matches", "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{}};
    auto selection = allowed_only({"mcp__echo__*"});
    selection.excluded = std::vector<std::string>{"mcp__echo__fail"};
    auto created = make_selected_mcp_session(runtime,
            workspace,
            std::make_shared<ToolCallProvider>("mcp__echo__echo", arguments),
            std::move(selection),
            {echo_server_config()});

    // The allow pattern keeps the family, but the exclusion removes one member
    // of it: selection is over the resolved set, not the raw pattern string.
    CHECK(session_exposes_tool(*created.session, "mcp__echo__echo"));
    CHECK(session_exposes_tool(*created.session, "mcp__echo__crash"));
    CHECK_FALSE(session_exposes_tool(*created.session, "mcp__echo__fail"));

    created.session->close();
}

TEST_CASE("a removed tool is unreachable through the ordinary executor path", "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    // The model calls `bash`, which the denylist removed. The call must fail as
    // an unknown tool rather than run: absence from the surface is not enough
    // if the executor can still reach it.
    support::JsonValue arguments{support::JsonValue::object_t{{"command", "echo hi"}}};
    auto created = make_selected_mcp_session(
            runtime, workspace, std::make_shared<ToolCallProvider>("bash", arguments), excluded_only({"bash"}), {});

    CHECK_FALSE(session_exposes_tool(*created.session, "bash"));
    REQUIRE(tests::run_awaitable(runtime, created.session->prompt("run bash")).has_value());
    CHECK(tool_result_is_error(*created.session, "bash"));
    const auto text = tool_result_text(*created.session, "bash").value_or("<none>");
    CHECK(text.find("unknown tool: bash") != std::string::npos);
    CHECK(created.session->is_open());

    created.session->close();
}

TEST_CASE("a selection pattern that matches no discovered tool fails assembly explicitly",
        "[coding_agent][tools][issue871][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{}};
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_servers = {echo_server_config()};
    options.session_facts.tools = std::vector<std::string>{"mcp__echo__*", "mcp__nosuch__*"};
    auto models = tests::models_from_provider(std::make_shared<ToolCallProvider>("mcp__echo__echo", arguments));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == support::ErrorCode::Validation);
    CHECK(created.error().message.find("mcp__nosuch__*") != std::string::npos);
    CHECK(created.error().message.find("matched no discovered tool") != std::string::npos);
}
