// Ticket #1118 (spec #1044, ADR 0075): the codemode catalog through the highest
// public caller — a real `AgentSession` assembled by `SessionFactory` with a
// scripted model. The frozen pi-v1.3.0 artifact pins the rendering (see
// `CodemodeCatalogTest`); these cases prove the live path: the next request
// carries the refreshed description after a tool is registered, re-exposed,
// withdrawn or budgeted out, and every listed tool and advertised global is
// actually executable.
//
// Acceptance discipline: each case names the case its cheap stand-in would let
// through. A "description changed" check passes on an empty catalog; a
// "description lists a tool" check passes when the sandbox cannot call it; a
// "globals are declared" check passes while the handler is unreachable. The
// cases below execute the listed tool and call the advertised globals.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/ModelsFixture.hpp"
#include "support/McpTestKit.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextFile.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
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

namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::string fixture_dir() { return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp"; }

/// Write a global `mcp.json` under the XDG root the test's EnvVarGuard points
/// at, mirroring pi's `<agentDir>/mcp.json`.
void write_global_mcp_json(const tests::TempWorkspace& workspace, std::string_view content) {
    const auto agent_dir = workspace.path() / "xdg" / "pike" / "agent";
    std::filesystem::create_directories(agent_dir);
    std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
    output << content;
}

void write_global_settings(const tests::TempWorkspace& workspace, std::string_view content) {
    const auto agent_dir = workspace.path() / "xdg" / "pike" / "agent";
    std::filesystem::create_directories(agent_dir);
    std::ofstream output(agent_dir / "settings.json", std::ios::binary | std::ios::trunc);
    output << content;
}

/// The resource-isolation fixture as a persisted stdio server. `trace` records
/// the JSON-RPC methods so a case can prove the transport was touched; `mode`
/// `change` makes the server offer a late `late` tool after a notification.
[[nodiscard]] std::string isolation_mcp_json(
        const std::filesystem::path& trace, std::string_view mode, std::string_view exposure) {
    return "{\"mcpServers\":{\"echo\":{\"command\":\"python3\",\"args\":[\"" + fixture_dir() +
           "/resource_isolation_server.py\",\"" + trace.string() + "\",\"" + std::string{mode} +
           "\"],\"description\":\"Echo fixture server\",\"exposure\":\"" + std::string{exposure} + "\"}}}";
}

struct SessionAttempt {
    std::unique_ptr<coding_agent::AgentSession> session;
    std::vector<coding_agent::SessionDiagnostic> diagnostics;
};

/// Assemble one in-memory session that picks up the persisted `mcp.json` and
/// settle the background connects through the manager's public wait.
[[nodiscard]] SessionAttempt make_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        const std::shared_ptr<tests::ScriptedProvider>& provider,
        std::optional<std::vector<std::string>> tools = std::nullopt,
        coding_agent::SessionTarget target = coding_agent::InMemorySessionTarget{}) {
    tests::ModelsSessionOptions options;
    options.session_target = std::move(target);
    options.session_facts.no_skills = true;
    options.session_facts.no_prompt_templates = true;
    options.session_facts.tools = std::move(tools);
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    auto models = tests::models_from_provider(provider);
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    if (auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*created->session);
            manager != nullptr) {
        REQUIRE(runtime.run(manager->wait_for_pending_connects()).has_value());
    }
    return SessionAttempt{
            .session = std::move(created->session),
            .diagnostics = std::move(created->diagnostics),
    };
}

/// A scripted provider that records the declared tools of every request and
/// answers plain text; a case can arm one codemode call.
class CapturingProvider final : public tests::ScriptedProvider {
public:
    CapturingProvider() : ScriptedProvider("fake") {}

    /// Arm one `codemode` call whose arguments are `{"code": script}`.
    void arm_script(std::string script) {
        script_ = std::move(script);
        issued_ = false;
    }

    std::vector<std::vector<ai::Tool>> offered_tools;

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions) override {
        offered_tools.push_back(std::move(context.tools));
        const bool issue_call = script_.has_value() && !issued_;
        issued_ = issued_ || issue_call;
        support::JsonValue arguments{support::JsonValue::object_t{}};
        std::string raw = "{}";
        if (issue_call) {
            arguments = support::JsonValue{support::JsonValue::object_t{{"code", *script_}}};
            auto written = support::write_json(arguments);
            REQUIRE(written.has_value());
            raw = std::move(*written);
        }
        return ai::detail::make_model_stream(
                [this, model = std::move(model), issue_call, arguments = std::move(arguments), raw = std::move(raw)](
                        ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "catalog-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (issue_call) {
                        round.content = {ai::text_content("running the script")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_1",
                                .name = "codemode",
                                .arguments = arguments,
                                .raw_arguments = raw,
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
    std::optional<std::string> script_;
    bool issued_{false};
};

/// The `codemode` declaration in the most recent request, if it was declared.
[[nodiscard]] std::optional<ai::Tool> offered_codemode(const std::vector<ai::Tool>& tools) {
    const auto found = std::ranges::find(tools, std::string{extensions::kCodemodeToolName}, &ai::Tool::name);
    return found == tools.end() ? std::nullopt : std::optional<ai::Tool>{*found};
}

[[nodiscard]] std::vector<std::string> offered_names(const std::vector<ai::Tool>& tools) {
    std::vector<std::string> names;
    for (const auto& tool : tools)
        names.push_back(tool.name);
    return names;
}

[[nodiscard]] std::optional<std::string> tool_result_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    std::optional<std::string> last;
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            last = ai::text_from_content(result->content);
        }
    }
    return last;
}

/// The details of the session's latest result for `name` (pi
/// `ToolResultMessage.details`), which carries the nested-call records and the
/// spill path.
[[nodiscard]] std::optional<support::JsonValue> tool_result_details(
        const coding_agent::AgentSession& session, std::string_view name) {
    std::optional<support::JsonValue> last;
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name && result->details.has_value()) {
            last = *result->details;
        }
    }
    return last;
}

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

/// Run one manager action and require it to complete without a failure message
/// (pi `runAction` reports `string | undefined`).
void require_action_ok(tests::RuntimeFixture& runtime, support::AsyncResult<std::optional<std::string>> action) {
    auto outcome = runtime.run(std::move(action));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
}

[[nodiscard]] std::string empty_catalog_description() {
    return extensions::create_codemode_description({}, extensions::CodemodeCatalogSettings{});
}

} // namespace

TEST_CASE("a real session's next request lists MCP tools registered late and executes them",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "codemode"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);

    // The initial request already carries the live catalog: the codemode tool
    // is activated by the `codemode` exposure (pi `ensureDiscoveryActive`) and
    // lists the server's first tool under its namespace.
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    const auto first = offered_codemode(provider->offered_tools.front());
    REQUIRE(first.has_value());
    INFO(first->description);
    CHECK(first->description != empty_catalog_description());
    CHECK(contains(first->description, "Nested tools:"));
    CHECK(contains(first->description, "## mcp__echo\nEcho fixture server"));
    CHECK(contains(first->description, "### `mcp__echo__echo`"));
    // The `models` line stays absent: Pike binds no `models` globals.
    CHECK_FALSE(contains(first->description, "`models`"));

    // The fixture offers `late` only after a `tools/call`, which makes it emit
    // `notifications/tools/list_changed`. Call the first tool through the real
    // nested-call seam, then wait for the re-registration.
    provider->arm_script("const r = await tools.mcp__echo__echo({ text: \"first:call\" });"
                         " text(r.content[0].text); return \"done\";");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("trigger the change")).has_value());
    auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    REQUIRE(manager != nullptr);
    const auto late_registered = [manager] {
        const auto servers = manager->servers();
        return servers.size() == 1 && servers.front().connection && servers.front().connection->tools.size() == 2;
    };
    REQUIRE(runtime.run(support::detail::make_async_result(
                                [late_registered]() -> boost::asio::awaitable<support::ExpectedVoid> {
                                    const auto executor = co_await boost::asio::this_coro::executor;
                                    co_await tests::wait_for(executor, late_registered, std::chrono::seconds{2});
                                    co_return support::ExpectedVoid{};
                                }))
                    .has_value());

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("next request")).has_value());
    const auto refreshed = offered_codemode(provider->offered_tools.back());
    REQUIRE(refreshed.has_value());
    INFO(refreshed->description);
    // The next request sees the addition: the same catalog text with one more
    // section, so the delta is the tool registration, not a reformat.
    CHECK(contains(refreshed->description, "### `mcp__echo__echo`"));
    CHECK(contains(refreshed->description, "### `mcp__echo__late`"));
    // The earlier request's description did not contain it.
    CHECK_FALSE(contains(first->description, "### `mcp__echo__late`"));

    // The separation case: a listed tool must execute. The script calls the
    // late tool through the real nested-call seam.
    provider->arm_script("const r = await tools.mcp__echo__late({ message: \"late:bound\" });"
                         " text(r.content[0].text); return \"done\";");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("run the listed tool")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    CHECK(contains(*result, "late:bound"));

    attempt.session->close();
}

TEST_CASE("disabling a server and changing its exposure reshape the next request's catalog",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "codemode"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    REQUIRE(manager != nullptr);
    require_action_ok(runtime, manager->set_enabled("echo", false));

    // Removal: the disabled server's tools re-register as hidden, so the next
    // request's catalog drops them. The empty text here is reached by removal,
    // not by a catalog that never refreshed — the first request listed the
    // tool, so the same check separates both cases.
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("after disable")).has_value());
    const auto disabled = offered_codemode(provider->offered_tools.back());
    REQUIRE(disabled.has_value());
    INFO(disabled->description);
    CHECK_FALSE(contains(disabled->description, "### `mcp__echo__echo`"));
    CHECK_FALSE(contains(disabled->description, "## mcp__echo"));
    CHECK(disabled->description == empty_catalog_description());

    // Re-enable: the tool returns to the catalog.
    require_action_ok(runtime, manager->set_enabled("echo", true));
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("after enable")).has_value());
    const auto enabled = offered_codemode(provider->offered_tools.back());
    REQUIRE(enabled.has_value());
    CHECK(contains(enabled->description, "### `mcp__echo__echo`"));

    // Exposure change: a `direct` registration declares the tool and removes
    // it from the catalog, so the same server presents differently on the next
    // request without any reconnection.
    require_action_ok(runtime, manager->set_exposure("echo", coding_agent::mcp::McpExposure::Direct));
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("after exposure change")).has_value());
    const auto& changed = provider->offered_tools.back();
    const auto direct = std::ranges::find(changed, std::string{"mcp__echo__echo"}, &ai::Tool::name);
    REQUIRE(direct != changed.end());
    CHECK(direct->description.starts_with("original\n\nCodemode: `tools.mcp__echo__echo(args)` resolves to `"));
    const auto changed_catalog = offered_codemode(changed);
    REQUIRE(changed_catalog.has_value());
    CHECK_FALSE(contains(changed_catalog->description, "### `mcp__echo__echo`"));

    attempt.session->close();
}

TEST_CASE("a direct tool's declaration carries the script-call sentence and stays out of the catalog",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "direct"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    const auto& offered = provider->offered_tools.front();
    // pi `prepareCodemodeLoadout` mode `on`: a declared callable direct tool's
    // description gains the script-call sentence, with the MCP result schema
    // as its output type.
    const auto direct = std::ranges::find(offered, std::string{"mcp__echo__echo"}, &ai::Tool::name);
    REQUIRE(direct != offered.end());
    INFO(direct->description);
    CHECK(direct->description.starts_with("original\n\nCodemode: `tools.mcp__echo__echo(args)` resolves to `"));
    CHECK(contains(direct->description, "{ content"));

    // The direct tool is not repeated in the codemode catalog; a codemode
    // server's tool would be. Codemode itself stays inactive without a
    // `codemode` exposure or `autoEnableCodemode` activation, so the catalog
    // is not part of this request.
    CHECK_FALSE(offered_codemode(offered).has_value());

    attempt.session->close();
}

TEST_CASE("the only loadout hides direct declarations from the request and lists them in the catalog",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_settings(workspace, R"({"codemode":{"mode":"only"}})");
    // A `codemode` exposure activates the tool (pi `ensureDiscoveryActive`), so
    // the presentation mode is observable on a real request.
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "codemode"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    const auto& offered = provider->offered_tools.front();
    const auto codemode = offered_codemode(offered);
    REQUIRE(codemode.has_value());
    INFO(codemode->description);
    // `only` lists every callable tool, including the declared built-ins, with
    // their prompt guidelines.
    CHECK(contains(codemode->description, "### `read`"));
    CHECK(contains(codemode->description, "- Use read to examine files instead of cat or sed."));
    CHECK(contains(codemode->description, "### `bash`"));
    CHECK(contains(codemode->description, "### `mcp__echo__echo`"));
    // ...and the direct declarations leave the request (pi
    // `_installHiddenDeclarationsProjection`), while codemode itself stays.
    const auto names = offered_names(offered);
    CHECK(std::ranges::find(names, std::string{"read"}) == names.end());
    CHECK(std::ranges::find(names, std::string{"bash"}) == names.end());
    CHECK(std::ranges::find(names, std::string{"codemode"}) != names.end());
    // The transcript still declares them, so the active set survives a resume
    // and only the request projection changes.
    const auto& active = attempt.session->snapshot().agent_state.active_tool_names;
    CHECK(std::ranges::find(active, std::string{"read"}) != active.end());

    attempt.session->close();
}

TEST_CASE("the advertised discovery globals are bound and answer over the callable loadout",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "codemode"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    const auto codemode = offered_codemode(provider->offered_tools.front());
    REQUIRE(codemode.has_value());
    // Every global the description advertises is a real bound global.
    CHECK(contains(codemode->description, "`ALL_TOOLS`"));
    CHECK(contains(codemode->description, "`await searchTools("));
    CHECK(contains(codemode->description, "`await describeTool(name)`"));
    CHECK(contains(codemode->description, "`await describeNamespace(name)`"));

    provider->arm_script("const found = await searchTools('echo');"
                         " const doc = await describeTool('mcp__echo__echo');"
                         " const ns = await describeNamespace('echo');"
                         " text(JSON.stringify({ found: found.map((entry) => entry.name),"
                         "   doc: doc.includes('mcp__echo__echo'), ns: ns.name,"
                         "   nsTools: ns.tools, nsDescription: ns.description }));"
                         " return 'done';");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("discover")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    INFO(*result);
    CHECK(contains(*result, "mcp__echo__echo"));
    CHECK(contains(*result, "\"doc\":true"));
    CHECK(contains(*result, "\"ns\":\"mcp__echo\""));
    CHECK(contains(*result, "\"nsDescription\":\"Echo fixture server\""));
    // The namespace answers under its alias too, which is the name a script
    // would guess from the server name, and lists the identifier forms.
    CHECK(contains(*result, "\"nsTools\":[\"mcp__echo__echo\"]"));

    attempt.session->close();
}

TEST_CASE("an unknown discovery name is an explicit rejected call, never a silent success",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, R"({"mcpServers":{}})");
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    // `--tools codemode` is pi's explicit activation path, so the tool is
    // declared and callable without an MCP server.
    auto attempt = make_session(runtime, workspace, provider, std::vector<std::string>{"codemode"});

    // A non-string query is pi's validation error, not an empty result.
    provider->arm_script("return await searchTools(42);");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("bad query")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    CHECK(contains(*result, "Script failed"));
    CHECK(contains(*result, "searchTools() expects a query string"));

    // An unknown namespace is an explicit null result, never a fabricated
    // group. (pi returns `undefined`; this port's host bridge carries JSON text,
    // so the absent value arrives as JSON `null`. The absence, not the JS
    // spelling, is what the catalog contract fixes.)
    provider = std::make_shared<CapturingProvider>();
    attempt.session->close();
    auto second = make_session(runtime, workspace, provider, std::vector<std::string>{"codemode"});
    provider->arm_script("text(String(await describeNamespace('ghost')));"
                         " text(String(await describeTool('ghost'))); return 'done';");
    REQUIRE(tests::run_awaitable(runtime, second.session->prompt("unknown namespace")).has_value());
    const auto second_result = tool_result_text(*second.session, "codemode");
    REQUIRE(second_result.has_value());
    INFO(*second_result);
    CHECK(contains(*second_result, "null"));
    CHECK_FALSE(contains(*second_result, "Script failed"));
    second.session->close();
}

TEST_CASE("an inline budget omits a namespace's tools with pi's marker",
        "[coding_agent][codemode][catalog][issue1118][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_settings(workspace, R"({"codemode":{"inlineBudget":1}})");
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace, isolation_mcp_json(trace, "change", "codemode"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider);

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    const auto codemode = offered_codemode(provider->offered_tools.front());
    REQUIRE(codemode.has_value());
    INFO(codemode->description);
    // Nothing fits one estimated token, so the namespace heading stays with
    // pi's explicit marker instead of silently disappearing.
    CHECK(contains(codemode->description, "Nested tools:"));
    CHECK(contains(codemode->description, "## mcp__echo (tools not listed)"));
    CHECK_FALSE(contains(codemode->description, "### `mcp__echo__echo`"));

    attempt.session->close();
}

TEST_CASE("a session resumes codemode store writes and discards failed script writes",
        "[coding_agent][codemode][issue1119][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    const auto path = workspace.path() / "session.jsonl";
    auto first = make_session(runtime,
            workspace,
            provider,
            std::vector<std::string>{"codemode", "write", "read"},
            coding_agent::ExplicitOpenOrCreateSessionTarget{path});
    provider->arm_script("await tools.write({path: 'answer.txt', content: '42'}); "
                         "store('answer', await tools.read({path: 'answer.txt'})); return load('answer');");
    auto written = tests::run_awaitable(runtime, first.session->prompt("save the answer"));
    const auto initial = tool_result_text(*first.session, "codemode");
    first.session->close();
    first.session.reset();
    auto resumed =
            make_session(runtime, workspace, provider, std::nullopt, coding_agent::ExplicitResumeSessionTarget{path});
    provider->arm_script("text(load('answer')); store('answer', 'wrong'); throw new Error('rollback');");
    auto failed = tests::run_awaitable(runtime, resumed.session->prompt("fail after loading"));
    const auto failure = tool_result_text(*resumed.session, "codemode");
    provider->arm_script("return load('answer');");
    auto loaded = tests::run_awaitable(runtime, resumed.session->prompt("load again"));
    const auto final = tool_result_text(*resumed.session, "codemode");
    resumed.session->close();
    resumed.session.reset();
    runtime.close();
    CHECK(written.has_value());
    CHECK(failed.has_value());
    CHECK(loaded.has_value());
    REQUIRE(initial.has_value());
    REQUIRE(failure.has_value());
    REQUIRE(final.has_value());
    CHECK(contains(*initial, "42"));
    CHECK(contains(*failure, "42"));
    CHECK(contains(*failure, "rollback"));
    CHECK(contains(*final, "42"));
    CHECK_FALSE(contains(*final, "wrong"));
}

TEST_CASE("a real session presents codemode output and applies the script budget",
        "[coding_agent][codemode][issue1119][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider, std::vector<std::string>{"codemode"});

    provider->arm_script("console.log('log'); text('first'); text('second'); return 'last';");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("present")).has_value());
    const auto presentation = tool_result_text(*attempt.session, "codemode");

    provider->arm_script("// @options: {\"max_output_tokens\": 2}\ntext('abcdefghijklmnopqrstuvwxyz');");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("budget")).has_value());
    const auto budget = tool_result_text(*attempt.session, "codemode");
    const auto details = tool_result_details(*attempt.session, "codemode");
    attempt.session->close();
    attempt.session.reset();
    runtime.close();

    REQUIRE(presentation.has_value());
    // pi's layout: numbered text items, then one console block.
    CHECK(contains(*presentation, "==> text 1/3 <==\nfirst"));
    CHECK(contains(*presentation, "==> text 2/3 <==\nsecond"));
    CHECK(contains(*presentation, "==> text 3/3 <==\nlast"));
    CHECK(contains(*presentation, "<console_output>\nlog\n</console_output>"));

    REQUIRE(budget.has_value());
    CHECK(contains(*budget, "Warning: truncated output (original token count: 7)"));
    REQUIRE(details.has_value());
    const auto* spilled = details->get_object().at("fullOutputPath").get_if<std::string>();
    REQUIRE(spilled != nullptr);
    // The model-visible result names the spill file; the file holds the full
    // text the budget removed.
    CHECK(tests::read_text_file(*spilled) == "abcdefghijklmnopqrstuvwxyz");
    std::filesystem::remove(*spilled);
}

TEST_CASE(
        "a real session reports a timed-out codemode call as cancelled", "[coding_agent][codemode][issue1119][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider, std::vector<std::string>{"codemode", "write", "bash"});

    provider->arm_script("// @options: {\"timeout_ms\": 100}\n"
                         "await tools.write({path: 'before.txt', content: 'kept'});"
                         "text('before');"
                         "await tools.bash({command: 'sleep 2'});");
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("timeout")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    const auto details = tool_result_details(*attempt.session, "codemode");
    attempt.session->close();
    attempt.session.reset();
    runtime.close();

    REQUIRE(result.has_value());
    CHECK(contains(*result, "Script timed out: Execution timed out after 100 ms"));
    CHECK(contains(*result, "before"));
    CHECK(contains(*result, "write (ok), bash (cancelled)"));
    REQUIRE(details.has_value());
    const auto& calls = details->get_object().at("calls").get_array();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].get_object().at("status").get_string() == "ok");
    CHECK(calls[1].get_object().at("status").get_string() == "cancelled");
    CHECK(calls[1].get_object().at("id").get_string() == "call_1/?");
    CHECK_FALSE(calls[1].get_object().contains("durationMs"));
}

TEST_CASE("a settings document the schema accepts reshapes the request and reload re-reads it",
        "[coding_agent][codemode][catalog][issue1115][spec]") {
    // Ticket #1115 acceptance: a `settings.json` document the published schema
    // accepts is consumed by the real runtime, and `/reload` re-reads it. The
    // discriminating case is the stale value: if the reload did not re-apply
    // `codemode.mode`, the second request would still declare the direct tools.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_settings(workspace, R"({"codemode":{"mode":"on","inlineBudget":3000}})");
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<CapturingProvider>();
    auto attempt = make_session(runtime, workspace, provider, std::vector<std::string>{"codemode", "read"});

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("first request")).has_value());
    REQUIRE_FALSE(provider->offered_tools.empty());
    {
        const auto& offered = provider->offered_tools.front();
        const auto codemode = offered_codemode(offered);
        REQUIRE(codemode.has_value());
        // `on`: the direct `read` declaration stays in the request.
        const auto names = offered_names(offered);
        CHECK(std::ranges::find(names, std::string{"read"}) != names.end());
    }

    // Rewrite the same scope and reload through the public Session operation.
    write_global_settings(workspace, R"({"codemode":{"mode":"only","inlineBudget":3000}})");
    auto reloaded = tests::run_awaitable(runtime, attempt.session->reload());
    REQUIRE(reloaded.has_value());

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("after reload")).has_value());
    const auto& offered = provider->offered_tools.back();
    const auto codemode = offered_codemode(offered);
    REQUIRE(codemode.has_value());
    INFO(codemode->description);
    // `only`: the direct declaration leaves the request and the catalog lists
    // the tool instead, so the reloaded value reached the live catalog.
    const auto names = offered_names(offered);
    CHECK(std::ranges::find(names, std::string{"read"}) == names.end());
    CHECK(contains(codemode->description, "### `read`"));

    attempt.session->close();
}
