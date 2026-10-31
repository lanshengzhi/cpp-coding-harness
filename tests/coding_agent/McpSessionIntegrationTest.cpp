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
#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpStdioServerConfig.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ConnectionGate.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/JsonCompare.hpp"
#include "support/ModelsFixture.hpp"
#include "support/McpTestKit.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
[[nodiscard]] std::string echo_mcp_json(
        std::optional<std::string_view> exposure = std::nullopt, bool auto_enable_codemode = true) {
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
[[nodiscard]] SessionAttempt make_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        coding_agent::SessionTarget target,
        const std::shared_ptr<tests::ScriptedProvider>& provider) {
    tests::ModelsSessionOptions options;
    options.session_target = std::move(target);
    options.session_facts.no_skills = true;
    options.session_facts.no_prompt_templates = true;
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    auto models = tests::models_from_provider(provider);
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    // #1104: session creation queues the server connects in the background
    // (pi `session_start` fires and forgets); tests reading post-connect
    // state settle them through the manager's public wait.
    if (auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*created->session);
            manager != nullptr) {
        REQUIRE(runtime.run(manager->wait_for_pending_connects()).has_value());
    }
    return SessionAttempt{
            .session = std::move(created->session),
            .diagnostics = std::move(created->diagnostics),
            .mcp_servers = std::move(created->mcp_servers),
    };
}

[[nodiscard]] SessionAttempt make_persisted_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        const std::shared_ptr<tests::ScriptedProvider>& provider) {
    return make_session(runtime, workspace, coding_agent::InMemorySessionTarget{}, provider);
}

[[nodiscard]] bool exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::set<std::string> tool_name_set(const std::vector<std::string>& names) {
    return {names.begin(), names.end()};
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

[[nodiscard]] std::optional<bool> tool_result_is_error(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return result->is_error;
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

/// Every recorded result for `name` in transcript order.
[[nodiscard]] std::vector<std::string> tool_result_texts(
        const coding_agent::AgentSession& session, std::string_view name) {
    std::vector<std::string> texts;
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            texts.push_back(ai::text_from_content(result->content));
        }
    }
    return texts;
}

/// The persisted deferred-MCP loadout records in session-tree order: each
/// record's added `mcp__` tool names. The initial System Message records the
/// session's built-in loadout and is out of scope here.
[[nodiscard]] std::vector<std::vector<std::string>> persisted_loadouts(const coding_agent::AgentSession& session) {
    std::vector<std::vector<std::string>> loadouts;
    const auto entries = session.session_entries();
    if (!entries) return loadouts;
    for (const auto& entry : *entries) {
        if (!entry.message) continue;
        const auto* system = std::get_if<ai::SystemMessage>(&*entry.message);
        if (system == nullptr) continue;
        std::vector<std::string> added;
        for (const auto& tool : system->tools_added) {
            if (tool.name.starts_with("mcp__")) added.push_back(tool.name);
        }
        if (!added.empty()) loadouts.push_back(std::move(added));
    }
    return loadouts;
}

/// #1104 Session-level discrimination: a scripted connection whose
/// `connect` suspends until the test opens a gate. Through the real Session
/// Assembly door this proves creation returns while a server is still
/// connecting — the pre-#1104 assembly awaited every connect during
/// `create_agent_session`, which with the gate closed never returns and lets
/// the RuntimeFixture's stall budget fail the test.
class GatedConnection final : public coding_agent::runtime::McpLiveConnection {
public:
    std::string name{"echo"};
    coding_agent::runtime::McpServerState state_value{coding_agent::runtime::McpServerState::Connected};
    std::vector<coding_agent::runtime::McpLiveTool> offered_tools{
            coding_agent::runtime::McpLiveTool{.name = "echo", .description = "echo back"},
    };

    [[nodiscard]] const std::string& server_name() const noexcept override { return name; }
    [[nodiscard]] coding_agent::runtime::McpServerState state() const noexcept override { return state_value; }
    [[nodiscard]] const std::vector<coding_agent::runtime::McpLiveTool>& tools() const noexcept override {
        return offered_tools;
    }
    [[nodiscard]] bool has_resources() const noexcept override { return false; }
    [[nodiscard]] std::size_t resource_count() const noexcept override { return 0; }
    [[nodiscard]] const std::optional<std::string>& error() const noexcept override { return failure_; }
    [[nodiscard]] const std::optional<std::string>& stderr_tail() const noexcept override { return stderr_; }
    [[nodiscard]] bool uses_oauth() const noexcept override { return false; }
    [[nodiscard]] const std::string& oauth_url() const noexcept override { return oauth_url_; }
    [[nodiscard]] const std::optional<std::string>& instructions() const noexcept override { return instructions_; }
    [[nodiscard]] support::AsyncResult<cch::mcp::ToolCallResult> call_tool(
            std::string_view, support::JsonValue, std::stop_token, cch::mcp::Client::ProgressCallback = {}) override {
        return support::AsyncResult<cch::mcp::ToolCallResult>{cch::mcp::ToolCallResult{
                support::JsonValue{support::JsonValue::object_t{{"content", support::JsonValue::array_t{}}}},
                std::nullopt}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string>, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"resources", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string>, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"resourceTemplates", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(std::string, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"contents", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<void> reconnect() override {
        state_value = coding_agent::runtime::McpServerState::Connected;
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    [[nodiscard]] support::AsyncResult<void> sign_out() override {
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    void close() noexcept override {}
    void set_tools_changed_listener(ToolsChangedListener listener) override { tools_changed_ = std::move(listener); }
    void set_resources_changed_listener(ResourcesChangedListener listener) override {
        resources_changed_ = std::move(listener);
    }

private:
    std::optional<std::string> failure_;
    std::optional<std::string> stderr_;
    std::string oauth_url_;
    std::optional<std::string> instructions_;
    ToolsChangedListener tools_changed_;
    ResourcesChangedListener resources_changed_;
};

class GatedFactory final : public coding_agent::runtime::McpConnectionFactory,
                           public std::enable_shared_from_this<GatedFactory> {
public:
    std::shared_ptr<GatedConnection> connection = std::make_shared<GatedConnection>();
    tests::ConnectionGate gate;
    int connect_calls{0};

    void open() { gate.open(); }

    [[nodiscard]] support::AsyncResult<std::shared_ptr<coding_agent::runtime::McpLiveConnection>> connect(
            const coding_agent::mcp::McpConfigEntry& entry) override {
        ++connect_calls;
        return support::detail::make_async_result([self = shared_from_this(), name = entry.name]
                -> boost::asio::awaitable<
                        support::Expected<std::shared_ptr<coding_agent::runtime::McpLiveConnection>>> {
            co_await self->gate.wait();
            self->connection->name = std::move(name);
            co_return std::shared_ptr<coding_agent::runtime::McpLiveConnection>{self->connection};
        });
    }
};

class NoopAuth final : public coding_agent::runtime::McpSignInDriver {
public:
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            const coding_agent::mcp::McpConfigEntry&, const coding_agent::runtime::McpSignInPrompt&) override {
        return support::AsyncResult<std::optional<std::string>>{std::nullopt};
    }
    [[nodiscard]] support::AsyncResult<bool> sign_out(const coding_agent::mcp::McpConfigEntry&) override {
        return support::AsyncResult<bool>{false};
    }
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> stored_tokens(
            const coding_agent::mcp::McpConfigEntry&) override {
        return support::AsyncResult<std::optional<std::string>>{std::nullopt};
    }
};

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

    void set_call(std::string name, support::JsonValue arguments, std::string raw) {
        name_ = std::move(name);
        arguments_ = std::move(arguments);
        raw_ = std::move(raw);
        request_count_ = 0;
    }

    std::vector<std::vector<ai::Tool>> offered_tools;

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions) override {
        offered_tools.push_back(std::move(context.tools));
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

struct ScriptedToolCall {
    std::string name;
    support::JsonValue arguments;
};

/// One real Agent Session provider response per scripted tool call, then a
/// terminal answer. Every request records the actual model-facing offered set.
class ToolSequenceProvider final : public tests::ScriptedProvider {
public:
    explicit ToolSequenceProvider(std::vector<ScriptedToolCall> calls)
        : ScriptedProvider("fake"), calls_(std::move(calls)) {}

    std::vector<std::vector<std::string>> offered_tools;

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions) override {
        std::vector<std::string> offered;
        for (const auto& tool : context.tools)
            offered.push_back(tool.name);
        offered_tools.push_back(std::move(offered));
        const std::size_t request = request_count_++;
        return ai::detail::make_model_stream(
                [this, model = std::move(model), request](ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request < calls_.size()) {
                        const auto& call = calls_[request];
                        round.content = {ai::text_content("calling " + call.name)};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "sequence_" + std::to_string(request + 1),
                                .name = call.name,
                                .arguments = call.arguments,
                                .raw_arguments = support::write_json(call.arguments).value_or("{}"),
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
    std::vector<ScriptedToolCall> calls_;
    std::size_t request_count_{0};
};

} // namespace

TEST_CASE("agent session lists MCP resources only on demand and can call tools after a resource failure",
        "[coding_agent][mcp][issue1105][spec]") {
    const bool fail_resources = GENERATE(false, true);
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace,
            "{\"mcpServers\":{\"echo\":{\"command\":\"python3\",\"args\":[\"" + fixture_dir() +
                    "/resource_isolation_server.py\",\"" + trace.string() + "\",\"" + (fail_resources ? "fail" : "ok") +
                    "\"],\"exposure\":\"direct\"}}}");
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            {"list_mcp_resources", support::JsonValue::object_t{{"server", "echo"}}},
            {"mcp__echo__echo", support::JsonValue::object_t{}},
            {"mcp__echo__echo", support::JsonValue::object_t{}},
    });
    auto attempt = make_persisted_session(runtime, workspace, provider);
    const auto startup_methods = tests::read_text_if_present(trace);
    const auto prompted = tests::run_awaitable(runtime, attempt.session->prompt("list resources then echo twice"));
    const auto listed = tool_result_text(*attempt.session, "list_mcp_resources");
    const auto resource_error = tool_result_is_error(*attempt.session, "list_mcp_resources");
    const auto echoed = tool_result_texts(*attempt.session, "mcp__echo__echo");
    auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    const auto servers = manager->servers();
    attempt.session->close();
    attempt.session.reset();
    runtime.close();

    CHECK(startup_methods == "initialize\nnotifications/initialized\ntools/list\n");
    CHECK(prompted.has_value());
    CHECK(resource_error == fail_resources);
    REQUIRE(listed.has_value());
    CHECK(listed->find(fail_resources ? "resource boom" : "docs://intro") != std::string::npos);
    CHECK(echoed == std::vector<std::string>{"echo after resources", "echo after resources"});
    REQUIRE(servers.size() == 1);
    REQUIRE(servers.front().connection.has_value());
    CHECK(servers.front().connection->state == coding_agent::runtime::McpServerState::Connected);
    CHECK(servers.front().connection->has_resources);
    CHECK(tests::read_text_if_present(trace) == startup_methods + "resources/list\ntools/call\ntools/call\n");
}

TEST_CASE("agent session binds late MCP tools and replaces their callable schemas after an idle notification",
        "[coding_agent][mcp][issue1105][spec]") {
    const bool scripted = GENERATE(false, true);
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace,
            "{\"mcpServers\":{\"echo\":{\"command\":\"python3\",\"args\":[\"" + fixture_dir() +
                    "/resource_isolation_server.py\",\"" + trace.string() + "\",\"change\"],\"exposure\":\"" +
                    (scripted ? "codemode" : "direct") + "\"}}}");
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<SingleCallProvider>("", support::JsonValue{}, "");
    const auto set_call = [provider, scripted](std::string name, std::string field, std::string value) {
        support::JsonValue arguments{support::JsonValue::object_t{{field, value}}};
        if (scripted) {
            arguments = support::JsonValue::object_t{{"code",
                    "const r = await tools." + name + "(" + support::write_json(arguments).value_or("{}") +
                            "); text(r.content[0].text);"}};
            name = "codemode";
        }
        const auto raw = support::write_json(arguments).value_or("{}");
        provider->set_call(std::move(name), std::move(arguments), raw);
    };
    set_call("mcp__echo__echo", "text", "first");
    auto attempt = make_persisted_session(runtime, workspace, provider);
    auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    const auto first = tests::run_awaitable(runtime, attempt.session->prompt("echo first"));
    const auto late_registered = [manager] {
        const auto servers = manager->servers();
        return servers.size() == 1 && servers.front().connection && servers.front().connection->tools.size() == 2;
    };
    // The Session retains the manager until this bounded wait and all prompts settle.
    const auto settled = runtime.run(
            support::detail::make_async_result([late_registered]() -> boost::asio::awaitable<support::ExpectedVoid> {
                const auto executor = co_await boost::asio::this_coro::executor;
                co_await tests::wait_for(executor, late_registered, std::chrono::seconds{2});
                co_return support::ExpectedVoid{};
            }));
    const bool late_ready = late_registered();
    set_call("mcp__echo__late", "message", "bound");
    const auto second = tests::run_awaitable(runtime, attempt.session->prompt("call late tool"));
    set_call("mcp__echo__echo", "message", "second");
    const auto third = tests::run_awaitable(runtime, attempt.session->prompt("call updated echo"));
    const auto late_results = tool_result_texts(*attempt.session, scripted ? "codemode" : "mcp__echo__late");
    const auto echoes = tool_result_texts(*attempt.session, scripted ? "codemode" : "mcp__echo__echo");
    const bool late_declared = exposes_tool(*attempt.session, "mcp__echo__late");
    attempt.session->close();
    attempt.session.reset();
    runtime.close();

    CHECK(first.has_value());
    CHECK(settled.has_value());
    CHECK(late_ready);
    CHECK(second.has_value());
    CHECK(third.has_value());
    CHECK(late_declared == !scripted);
    CHECK(std::ranges::any_of(
            late_results, [](const auto& text) { return text.find("late:bound") != std::string::npos; }));
    REQUIRE_FALSE(echoes.empty());
    CHECK(echoes.front().find("echo:first") != std::string::npos);
    CHECK(echoes.back().find("echo:second") != std::string::npos);
    CHECK(tests::trace_line_count(trace, "tools/list") == 2);
    CHECK(tests::trace_line_count(trace, "tools/call") == 3);
    if (!scripted) {
        REQUIRE_FALSE(provider->offered_tools.empty());
        // The refreshed schema is what the caller must actually send; asserting
        // only the description would pass even if the replacement schema never
        // reached the declaration. `json_mismatch` rejects extra keys, so a
        // stale `text` property left behind by a merge is a failure.
        const auto parsed_schema = support::read_json(
                R"({"type":"object","additionalProperties":false,"required":["message"],"properties":{"message":{"type":"string"}}})");
        REQUIRE(parsed_schema.has_value());
        const auto& replacement_schema = *parsed_schema;
        std::vector<std::string> names;
        for (const auto& tool : provider->offered_tools.back()) {
            if (!tool.name.starts_with("mcp__")) continue;
            names.push_back(tool.name);
            CHECK(tool.description == "updated");
            INFO(tool.name << ": " << tests::json_mismatch(replacement_schema, tool.parameters).value_or(""));
            CHECK_FALSE(tests::json_mismatch(replacement_schema, tool.parameters).has_value());
        }
        CHECK(names == std::vector<std::string>{"mcp__echo__echo", "mcp__echo__late"});
    }
}

TEST_CASE("agent session close discards a pending MCP tool refresh without reopening the transport",
        "[coding_agent][mcp][issue1105][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    const auto trace = workspace.path() / "methods";
    write_global_mcp_json(workspace,
            "{\"mcpServers\":{\"echo\":{\"command\":\"python3\",\"args\":[\"" + fixture_dir() +
                    "/resource_isolation_server.py\",\"" + trace.string() +
                    "\",\"hold-change\"],\"exposure\":\"direct\"}}}");
    tests::RuntimeFixture runtime;
    auto attempt = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>(
                    "mcp__echo__echo", support::JsonValue::object_t{{"text", "first"}}, "{\"text\":\"first\"}"));
    auto manager =
            coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session)->shared_from_this();
    const auto prompted = tests::run_awaitable(runtime, attempt.session->prompt("start a tool refresh"));
    const auto observed =
            runtime.run(support::detail::make_async_result([trace]() -> boost::asio::awaitable<support::ExpectedVoid> {
                const auto executor = co_await boost::asio::this_coro::executor;
                co_await tests::wait_for(
                        executor,
                        [trace] { return tests::trace_line_count(trace, "tools/list") == 2; },
                        std::chrono::seconds{2});
                co_return support::ExpectedVoid{};
            }));
    const bool refresh_pending = tests::trace_line_count(trace, "tools/list") == 2;
    attempt.session->close();
    attempt.session.reset();
    runtime.close();
    const auto servers = manager->servers();

    CHECK(prompted.has_value());
    CHECK(observed.has_value());
    CHECK(refresh_pending);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers.front().connection.has_value());
    CHECK(servers.front().connection->state == coding_agent::runtime::McpServerState::Closed);
    CHECK_FALSE(servers.front().connection->error.has_value());
    CHECK(tests::read_text_if_present(trace) ==
            "initialize\nnotifications/initialized\ntools/list\ntools/call\ntools/list\n");
}

TEST_CASE("tool_search ranks a deferred MCP tool, activates only the match, and the next request executes it",
        "[coding_agent][mcp][tool_search][issue1095][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("deferred"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            {"tool_search",
                    support::JsonValue{support::JsonValue::object_t{{"query", "echo incoming text"}, {"limit", 1.0}}}},
            {"mcp__echo__echo", support::JsonValue{support::JsonValue::object_t{{"text", "loaded and called"}}}},
    });
    auto attempt = make_session(runtime, workspace, coding_agent::DefaultPersistedSessionTarget{}, provider);

    const auto initial = attempt.session->snapshot().agent_state.active_tool_names;
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__fail"));
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("find the echo tool and use it")).has_value());
    const auto search_result = tool_result_text(*attempt.session, "tool_search");
    REQUIRE(search_result.has_value());
    CHECK(*search_result == "Loaded 1 tool. They are available from your next call:\n"
                            "- mcp__echo__echo: Echo the incoming text back.");
    CHECK(search_result->find("mcp__echo__fail") == std::string::npos);
    const auto entries = attempt.session->session_entries();
    REQUIRE(entries.has_value());
    const auto loadout = std::ranges::find_if(*entries, [](const auto& entry) {
        if (!entry.message) return false;
        const auto* system = std::get_if<ai::SystemMessage>(&*entry.message);
        return system != nullptr && std::ranges::any_of(system->tools_added,
                                            [](const ai::Tool& tool) { return tool.name == "mcp__echo__echo"; });
    });
    REQUIRE(loadout != entries->end());
    const auto& system = std::get<ai::SystemMessage>(*loadout->message);
    CHECK(std::ranges::count(system.tools_added, "mcp__echo__echo", &ai::Tool::name) == 1);
    CHECK(system.tools_removed.empty());
    REQUIRE(provider->offered_tools.size() == 3);
    CHECK(std::ranges::find(provider->offered_tools[0], "mcp__echo__echo") == provider->offered_tools[0].end());
    CHECK(std::ranges::find(provider->offered_tools[1], "mcp__echo__echo") != provider->offered_tools[1].end());
    CHECK(std::ranges::find(provider->offered_tools[1], "mcp__echo__fail") == provider->offered_tools[1].end());
    CHECK(std::ranges::find(provider->offered_tools[2], "mcp__echo__echo") != provider->offered_tools[2].end());
    CHECK(tool_result_text(*attempt.session, "mcp__echo__echo") == std::optional<std::string>{"loaded and called"});
    CHECK(exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__fail"));
    CHECK(tool_name_set(initial) == tool_name_set(provider->offered_tools[0]));

    attempt.session->close();
}

TEST_CASE("a no-match tool_search leaves the offered set unchanged",
        "[coding_agent][mcp][tool_search][issue1095][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("deferred"));
    tests::RuntimeFixture runtime;
    auto provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            {"tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "weather forecast"}}}},
    });
    auto attempt = make_persisted_session(runtime, workspace, provider);
    const auto before = attempt.session->snapshot().agent_state.active_tool_names;

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("find a weather tool")).has_value());
    REQUIRE(provider->offered_tools.size() == 2);
    CHECK(tool_name_set(provider->offered_tools[0]) == tool_name_set(before));
    CHECK(tool_name_set(provider->offered_tools[1]) == tool_name_set(before));
    CHECK(tool_result_text(*attempt.session, "tool_search") == std::optional<std::string>{"No matching tools found."});
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__fail"));

    attempt.session->close();
}

TEST_CASE("repeated tool_search calls in one run load each ranked match once and keep them declared",
        "[coding_agent][mcp][tool_search][issue1095][spec]") {
    // pi's frozen two-search case (`packages/durable/test/tool-search.test.ts`
    // issues a second `tool_search` in the same run and expects both
    // declarations) applied to the C++ session: `respond` ranks slow 0.500,
    // hang 0.408, crash 0.157, so three one-tool searches load those tools in
    // that order, and each search skips the tools it already loaded instead of
    // re-announcing them. A catalog that exposed every deferred tool up front
    // would fail the first offered set; a search that ignored the live loadout
    // would re-announce `slow` on the second call.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("deferred"));
    tests::RuntimeFixture runtime;
    const auto search = [] {
        return ScriptedToolCall{
                "tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "respond"}, {"limit", 1.0}}}};
    };
    auto provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            search(),
            search(),
            search(),
            {"mcp__echo__slow", support::JsonValue{support::JsonValue::object_t{{"text", "loaded slowly"}}}},
    });
    auto attempt = make_session(runtime, workspace, coding_agent::DefaultPersistedSessionTarget{}, provider);

    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("load the response tools one at a time"))
                    .has_value());

    CHECK(tool_result_texts(*attempt.session, "tool_search") ==
            std::vector<std::string>{
                    "Loaded 1 tool. They are available from your next call:\n- mcp__echo__slow: Respond after a delay.",
                    "Loaded 1 tool. They are available from your next call:\n- mcp__echo__hang: Never respond; keep "
                    "streaming after a cancellation.",
                    "Loaded 1 tool. They are available from your next call:\n- mcp__echo__crash: Exit the server "
                    "without "
                    "responding.",
            });
    CHECK(persisted_loadouts(*attempt.session) == std::vector<std::vector<std::string>>{
                                                          {"mcp__echo__slow"},
                                                          {"mcp__echo__hang"},
                                                          {"mcp__echo__crash"},
                                                  });
    REQUIRE(provider->offered_tools.size() == 5);
    const auto offered = [&](std::size_t request, std::string_view name) {
        return std::ranges::find(provider->offered_tools[request], name) != provider->offered_tools[request].end();
    };
    for (const auto name : {"mcp__echo__slow", "mcp__echo__hang", "mcp__echo__crash"}) {
        CHECK_FALSE(offered(0, name));
    }
    CHECK(offered(1, "mcp__echo__slow"));
    CHECK_FALSE(offered(1, "mcp__echo__hang"));
    CHECK_FALSE(offered(1, "mcp__echo__crash"));
    CHECK(offered(2, "mcp__echo__slow"));
    CHECK(offered(2, "mcp__echo__hang"));
    CHECK_FALSE(offered(2, "mcp__echo__crash"));
    CHECK(offered(3, "mcp__echo__slow"));
    CHECK(offered(3, "mcp__echo__hang"));
    CHECK(offered(3, "mcp__echo__crash"));
    // The first loaded tool is callable from the run that loaded it, after two
    // further searches.
    CHECK(tool_result_text(*attempt.session, "mcp__echo__slow") == std::optional<std::string>{"loaded slowly"});
    CHECK(offered(4, "mcp__echo__crash"));
    // The loadout never widens to the server's other deferred tools.
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*attempt.session, "mcp__echo__fail"));

    attempt.session->close();
}

TEST_CASE("reopened sessions load the next-ranked deferred tool and never re-announce an already loaded one",
        "[coding_agent][mcp][tool_search][session_resume][issue1095][spec]") {
    // The discriminating ordering case. `respond` matches three deferred tools
    // with distinct scores (slow 0.500, hang 0.408, crash 0.157), so each
    // one-tool search must load the best-ranked match that is not already
    // loaded, in that order, across successive reopens of the same session. A
    // catalog that exposed every deferred tool up front would fail the first
    // offered-set check; a search that activated every match would fail the
    // result texts; a search that ignored the replayed loadout would
    // re-announce `slow` instead of loading `hang`.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("deferred"));
    tests::RuntimeFixture runtime;
    const auto search = [] {
        return ScriptedToolCall{
                "tool_search", support::JsonValue{support::JsonValue::object_t{{"query", "respond"}, {"limit", 1.0}}}};
    };
    const auto offered = [](const std::shared_ptr<ToolSequenceProvider>& provider,
                                 std::size_t request,
                                 std::string_view name) {
        REQUIRE(provider->offered_tools.size() > request);
        return std::ranges::find(provider->offered_tools[request], name) != provider->offered_tools[request].end();
    };
    const std::string slow_line =
            "Loaded 1 tool. They are available from your next call:\n- mcp__echo__slow: Respond after a delay.";
    const std::string hang_line = "Loaded 1 tool. They are available from your next call:\n"
                                  "- mcp__echo__hang: Never respond; keep streaming after a cancellation.";
    const std::string crash_line = "Loaded 1 tool. They are available from your next call:\n"
                                   "- mcp__echo__crash: Exit the server without responding.";

    // The first session starts with no deferred tool declared and loads only
    // the best-ranked match.
    auto first_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{search()});
    auto first = make_session(runtime, workspace, coding_agent::DefaultPersistedSessionTarget{}, first_provider);
    REQUIRE(first.session->session_path().has_value());
    const auto session_path = *first.session->session_path();
    REQUIRE(tests::run_awaitable(runtime, first.session->prompt("load the best response tool")).has_value());
    CHECK(tool_result_texts(*first.session, "tool_search") == std::vector<std::string>{slow_line});
    CHECK(persisted_loadouts(*first.session) == std::vector<std::vector<std::string>>{{"mcp__echo__slow"}});
    REQUIRE(first_provider->offered_tools.size() == 2);
    for (const auto name : {"mcp__echo__slow", "mcp__echo__hang", "mcp__echo__crash"}) {
        CHECK_FALSE(offered(first_provider, 0, name));
    }
    CHECK_FALSE(offered(first_provider, 1, "mcp__echo__hang"));
    first.session->close();

    // The reopened session replays the loadout before the first request, skips
    // the already loaded tool when ranking, and runs the replayed tool.
    auto second_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            search(),
            {"mcp__echo__slow", support::JsonValue{support::JsonValue::object_t{{"text", "loaded in session one"}}}},
    });
    auto second = make_session(
            runtime, workspace, coding_agent::ExplicitResumeSessionTarget{.path = session_path}, second_provider);
    CHECK(exposes_tool(*second.session, "mcp__echo__slow"));
    CHECK_FALSE(exposes_tool(*second.session, "mcp__echo__hang"));
    REQUIRE(tests::run_awaitable(runtime, second.session->prompt("load the next response tool")).has_value());
    CHECK(offered(second_provider, 0, "mcp__echo__slow"));
    CHECK_FALSE(offered(second_provider, 0, "mcp__echo__hang"));
    CHECK_FALSE(offered(second_provider, 0, "mcp__echo__crash"));
    CHECK(tool_result_texts(*second.session, "tool_search") == std::vector<std::string>{slow_line, hang_line});
    CHECK(persisted_loadouts(*second.session) ==
            std::vector<std::vector<std::string>>{{"mcp__echo__slow"}, {"mcp__echo__hang"}});
    CHECK(tool_result_text(*second.session, "mcp__echo__slow") == std::optional<std::string>{"loaded in session one"});
    second.session->close();

    // The third reopen carries both earlier loadouts and loads the last match.
    auto third_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{search()});
    auto third = make_session(
            runtime, workspace, coding_agent::ExplicitResumeSessionTarget{.path = session_path}, third_provider);
    REQUIRE(tests::run_awaitable(runtime, third.session->prompt("load the last response tool")).has_value());
    CHECK(offered(third_provider, 0, "mcp__echo__slow"));
    CHECK(offered(third_provider, 0, "mcp__echo__hang"));
    CHECK_FALSE(offered(third_provider, 0, "mcp__echo__crash"));
    CHECK(tool_result_texts(*third.session, "tool_search") ==
            std::vector<std::string>{slow_line, hang_line, crash_line});
    CHECK(persisted_loadouts(*third.session) == std::vector<std::vector<std::string>>{
                                                        {"mcp__echo__slow"},
                                                        {"mcp__echo__hang"},
                                                        {"mcp__echo__crash"},
                                                });
    // The loadout never widens to the server's other deferred tools.
    CHECK(exposes_tool(*third.session, "mcp__echo__slow"));
    CHECK(exposes_tool(*third.session, "mcp__echo__hang"));
    CHECK(exposes_tool(*third.session, "mcp__echo__crash"));
    CHECK_FALSE(exposes_tool(*third.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*third.session, "mcp__echo__fail"));

    third.session->close();
}

TEST_CASE("forked sessions persist independent deferred MCP loadouts across reopen",
        "[coding_agent][mcp][tool_search][session_fork][issue1095][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    write_global_mcp_json(workspace, echo_mcp_json("deferred"));
    tests::RuntimeFixture runtime;

    auto source = make_session(
            runtime, workspace, coding_agent::DefaultPersistedSessionTarget{}, std::make_shared<PlainTextProvider>());
    REQUIRE(source.session->session_path().has_value());
    REQUIRE(tests::run_awaitable(runtime, source.session->prompt("shared fork point")).has_value());
    const auto user_messages = source.session->get_user_messages_for_forking();
    REQUIRE(user_messages.size() == 1);
    const auto fork_echo =
            source.session->prepare_fork(user_messages.front().entry_id, coding_agent::runtime::ForkPosition::At);
    const auto fork_fail =
            source.session->prepare_fork(user_messages.front().entry_id, coding_agent::runtime::ForkPosition::At);
    REQUIRE(fork_echo.has_value());
    REQUIRE(fork_echo->branched_path.has_value());
    REQUIRE(fork_fail.has_value());
    REQUIRE(fork_fail->branched_path.has_value());
    CHECK(*fork_echo->branched_path != *fork_fail->branched_path);

    auto echo_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            {"tool_search",
                    support::JsonValue{support::JsonValue::object_t{{"query", "echo incoming text"}, {"limit", 1.0}}}},
            {"mcp__echo__echo", support::JsonValue{support::JsonValue::object_t{{"text", "branch echo"}}}},
    });
    auto fail_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{
            {"tool_search",
                    support::JsonValue{
                            support::JsonValue::object_t{{"query", "always report error fail"}, {"limit", 1.0}}}},
            {"mcp__echo__fail", support::JsonValue{support::JsonValue::object_t{}}},
    });
    auto echo_branch = make_session(runtime,
            workspace,
            coding_agent::ExplicitResumeSessionTarget{.path = *fork_echo->branched_path},
            echo_provider);
    auto fail_branch = make_session(runtime,
            workspace,
            coding_agent::ExplicitResumeSessionTarget{.path = *fork_fail->branched_path},
            fail_provider);

    REQUIRE(tests::run_awaitable(runtime, echo_branch.session->prompt("load echo")).has_value());
    REQUIRE(tests::run_awaitable(runtime, fail_branch.session->prompt("load fail")).has_value());
    REQUIRE(echo_provider->offered_tools.size() == 3);
    REQUIRE(fail_provider->offered_tools.size() == 3);
    CHECK(std::ranges::find(echo_provider->offered_tools[1], "mcp__echo__echo") !=
            echo_provider->offered_tools[1].end());
    CHECK(std::ranges::find(echo_provider->offered_tools[1], "mcp__echo__fail") ==
            echo_provider->offered_tools[1].end());
    CHECK(std::ranges::find(fail_provider->offered_tools[1], "mcp__echo__fail") !=
            fail_provider->offered_tools[1].end());
    CHECK(std::ranges::find(fail_provider->offered_tools[1], "mcp__echo__echo") ==
            fail_provider->offered_tools[1].end());
    CHECK(tool_result_text(*echo_branch.session, "mcp__echo__echo") == std::optional<std::string>{"branch echo"});
    CHECK(tool_result_text(*fail_branch.session, "mcp__echo__fail") ==
            std::optional<std::string>{"fail: requested failure"});
    CHECK(tool_result_is_error(*fail_branch.session, "mcp__echo__fail") == std::optional<bool>{true});
    CHECK(exposes_tool(*echo_branch.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*echo_branch.session, "mcp__echo__fail"));
    CHECK(exposes_tool(*fail_branch.session, "mcp__echo__fail"));
    CHECK_FALSE(exposes_tool(*fail_branch.session, "mcp__echo__echo"));

    echo_branch.session->close();
    fail_branch.session->close();
    auto reopened_echo_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{});
    auto reopened_fail_provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{});
    auto reopened_echo = make_session(runtime,
            workspace,
            coding_agent::ExplicitResumeSessionTarget{.path = *fork_echo->branched_path},
            reopened_echo_provider);
    auto reopened_fail = make_session(runtime,
            workspace,
            coding_agent::ExplicitResumeSessionTarget{.path = *fork_fail->branched_path},
            reopened_fail_provider);
    CHECK(exposes_tool(*reopened_echo.session, "mcp__echo__echo"));
    CHECK_FALSE(exposes_tool(*reopened_echo.session, "mcp__echo__fail"));
    CHECK(exposes_tool(*reopened_fail.session, "mcp__echo__fail"));
    CHECK_FALSE(exposes_tool(*reopened_fail.session, "mcp__echo__echo"));
    REQUIRE(tests::run_awaitable(runtime, reopened_echo.session->prompt("verify echo loadout")).has_value());
    REQUIRE(tests::run_awaitable(runtime, reopened_fail.session->prompt("verify fail loadout")).has_value());
    REQUIRE(reopened_echo_provider->offered_tools.size() == 1);
    REQUIRE(reopened_fail_provider->offered_tools.size() == 1);
    CHECK(std::ranges::find(reopened_echo_provider->offered_tools[0], "mcp__echo__echo") !=
            reopened_echo_provider->offered_tools[0].end());
    CHECK(std::ranges::find(reopened_echo_provider->offered_tools[0], "mcp__echo__fail") ==
            reopened_echo_provider->offered_tools[0].end());
    CHECK(std::ranges::find(reopened_fail_provider->offered_tools[0], "mcp__echo__fail") !=
            reopened_fail_provider->offered_tools[0].end());
    CHECK(std::ranges::find(reopened_fail_provider->offered_tools[0], "mcp__echo__echo") ==
            reopened_fail_provider->offered_tools[0].end());

    reopened_echo.session->close();
    reopened_fail.session->close();
    source.session->close();
}

TEST_CASE("a codemode-exposure server registers its tools undeclared, activates codemode, and renders the mcp_servers "
          "section",
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

    // #1104: session creation no longer awaits the connect phase, so the
    // creation-time status snapshot may still read "connecting"; the
    // background connect settles right after creation (the helper waits it)
    // and the live manager reports the server running.
    const auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
    REQUIRE(manager != nullptr);
    const auto snapshots = manager->servers();
    const auto echo =
            std::ranges::find_if(snapshots, [](const auto& snapshot) { return snapshot.entry.name == "echo"; });
    REQUIRE(echo != snapshots.end());
    REQUIRE(echo->connection.has_value());
    CHECK(echo->connection->state == coding_agent::runtime::McpServerState::Connected);

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

    // pi `toScriptValue`: every MCP tool declares the MCP result output
    // schema, so the call resolves to pi `convertMcpResult`'s CallToolResult
    // object — the script reads the echoed text out of `content` (the echo
    // fixture returns no structuredContent of its own).
    const std::string script = "const r = await tools.mcp__echo__echo({ text: \"from-script\" });"
                               " text(\"echoed:\" + r.content[0].text);"
                               " return \"done\";";
    support::JsonValue arguments{support::JsonValue::object_t{{"code", script}}};
    auto raw = support::write_json(arguments);
    REQUIRE(raw.has_value());
    auto attempt = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>("codemode", std::move(arguments), std::move(*raw)));

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
    auto* manager = coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*attempt.session);
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
                    "list_mcp_resources", support::JsonValue{support::JsonValue::object_t{}}, "{}"));
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
    const std::string script = "const r = await tools.list_mcp_resources({ server: \"echo\" });"
                               " text(r);"
                               " return \"done\";";
    support::JsonValue arguments{support::JsonValue::object_t{{"code", script}}};
    auto raw = support::write_json(arguments);
    REQUIRE(raw.has_value());
    auto attempt = make_persisted_session(runtime,
            workspace,
            std::make_shared<SingleCallProvider>("codemode", std::move(arguments), std::move(*raw)));

    CHECK_FALSE(exposes_tool(*attempt.session, "list_mcp_resources"));
    REQUIRE(tests::run_awaitable(runtime, attempt.session->prompt("list resources through a script")).has_value());
    const auto result = tool_result_text(*attempt.session, "codemode");
    REQUIRE(result.has_value());
    CHECK(result->find("file:///docs/readme.md") != std::string::npos);

    attempt.session->close();
}

TEST_CASE("agent session creation returns while a gated MCP server connects and the manager stays manageable",
        "[coding_agent][mcp][issue1104][spec]") {
    // Frozen pi v1.3.0 (audit-1104): `session_start` fires the connects and
    // forgets them, so session creation never blocks on a handshake, and the
    // management surface answers while a connect is pending. The gated
    // scripted factory makes the discrimination explicit: an assembly that
    // still awaited connects during creation would hang on the closed gate
    // and the RuntimeFixture's stall budget would fail the test before any
    // assertion below ran.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard xdg{"XDG_CONFIG_HOME", (workspace.path() / "xdg").string()};
    tests::RuntimeFixture runtime;

    auto factory = std::make_shared<GatedFactory>();
    coding_agent::mcp::McpStdioServerConfig config;
    config.name = "echo";
    config.command = "gated";
    config.exposure = coding_agent::mcp::McpExposure::Direct;
    coding_agent::mcp::McpConfigEntry entry;
    entry.name = "echo";
    entry.enabled = true;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    coding_agent::mcp::McpConfigLoad load;
    load.servers = {std::move(entry)};
    coding_agent::runtime::McpManagerDependencies dependencies;
    dependencies.connections = factory;
    dependencies.auth = std::make_shared<NoopAuth>();
    auto manager = std::make_shared<coding_agent::runtime::McpSessionManager>(
            std::move(load), "/home/u/.pi/agent", std::vector<std::string>{}, std::move(dependencies));

    // No scripted calls: every request records the offered tool set and
    // answers terminally, which is all the post-settle prompt needs.
    auto provider = std::make_shared<ToolSequenceProvider>(std::vector<ScriptedToolCall>{});
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::DefaultPersistedSessionTarget{};
    options.session_facts.no_skills = true;
    options.session_facts.no_prompt_templates = true;
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    auto overrides = tests::cli_fake_overrides(tests::models_from_provider(provider));
    overrides.mcp_manager = manager;
    auto created = runtime.run(
            coding_agent::create_agent_session_async(std::move(options), std::nullopt, std::move(overrides)));
    REQUIRE(created.has_value());

    // Assembly bound the caller-built manager (the test-only seam) instead of
    // the production one...
    REQUIRE(coding_agent::detail::AgentSessionInteractiveAccess::mcp_manager(*created->session) == manager.get());
    // ...and returned while the connect was still pending: creation did not
    // await the handshake.
    CHECK(manager->pending_start_count() == 1);
    // The management surface reads the pending server as connecting (pi
    // `startConnection` publishes a connecting connection immediately), and
    // management queries answer while the connect is in flight.
    const auto pending = manager->servers();
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().entry.name == "echo");
    REQUIRE(pending.front().connection.has_value());
    CHECK(pending.front().connection->state == coding_agent::runtime::McpServerState::Connecting);
    CHECK(manager->search_tools("echo", 4).empty());
    // The creation result reports the unsettled server as connecting rather
    // than blocking (audit-1104): the diagnostic list keeps it visible.
    REQUIRE(created->mcp_servers.size() == 1);
    CHECK(created->mcp_servers.front().name == "echo");
    CHECK(created->mcp_servers.front().state == coding_agent::mcp::McpServerState::Starting);

    // Open the gate: the queued background connect lands (exactly one factory
    // call), the direct-exposure tool declares, and the first prompt offers
    // it to the model.
    factory->open();
    REQUIRE(runtime.run(manager->wait_for_pending_connects()).has_value());
    CHECK(factory->connect_calls == 1);
    const auto settled = manager->servers();
    REQUIRE(settled.size() == 1);
    REQUIRE(settled.front().connection.has_value());
    CHECK(settled.front().connection->state == coding_agent::runtime::McpServerState::Connected);
    CHECK(exposes_tool(*created->session, "mcp__echo__echo"));

    REQUIRE(tests::run_awaitable(runtime, created->session->prompt("ping")).has_value());
    REQUIRE(provider->offered_tools.size() == 1);
    CHECK(std::ranges::find(provider->offered_tools.front(), "mcp__echo__echo") !=
            provider->offered_tools.front().end());

    // A management write after the settle still round-trips: disable stops
    // the server and hides its tools, enable reconnects it and the tool is
    // offered to the model again. (The session snapshot's active set is a
    // run-published projection; the live manager and the model-facing
    // offered set are the observable seams here.)
    REQUIRE(runtime.run(manager->set_enabled("echo", false)).has_value());
    CHECK(manager->search_tools("echo", 4).empty());
    REQUIRE(runtime.run(manager->set_enabled("echo", true)).has_value());
    REQUIRE(tests::run_awaitable(runtime, created->session->prompt("ping again")).has_value());
    REQUIRE(provider->offered_tools.size() == 2);
    CHECK(std::ranges::find(provider->offered_tools.back(), "mcp__echo__echo") != provider->offered_tools.back().end());

    created->session->close();
}
