// The LIVE in-session MCP server manager (spec #882, ticket #884): the
// session-level manager pi has in `extensions/mcp/index.ts` + `runtime.ts`.
//
// Every case drives the manager through injected seams (a scripted connection
// factory, a recording tool surface, a scripted OAuth driver), so no process,
// socket, or browser is involved. Each acceptance case is paired with a case
// the property separates: a failed `tools/list` reports failed with the stderr
// tail (not connected) while a healthy server reports connected; a disable
// survives a manager rebuild through the mcp.json round-trip while an enable
// does too; a withdrawn tool re-registers as hidden while a still-offered tool
// keeps its exposure; the resource tools take the widest non-hidden exposure
// while a hidden-only set leaves them hidden.

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpConfigWrite.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/runtime/McpSessionManager.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::mcp::McpConfigEntry;
using coding_agent::mcp::McpConfigLoad;
using coding_agent::mcp::McpExposure;
using coding_agent::mcp::McpStdioServerConfig;
using coding_agent::runtime::McpConnectionFactory;
using coding_agent::runtime::McpLiveConnection;
using coding_agent::runtime::McpLiveTool;
using coding_agent::runtime::McpManagerDependencies;
using coding_agent::runtime::McpRegisteredTool;
using coding_agent::runtime::McpServerSnapshot;
using coding_agent::runtime::McpServerState;
using coding_agent::runtime::McpSessionManager;
using coding_agent::runtime::McpSignInDriver;
using coding_agent::runtime::McpSignInPrompt;
using coding_agent::runtime::McpSurfaceTool;
using coding_agent::runtime::McpToolSurface;

[[nodiscard]] support::Error test_error(std::string message) {
    return support::make_error(support::ErrorCode::Process, std::move(message));
}

// ── Scripted live connection ────────────────────────────────────────────────

class ScriptedConnection final : public McpLiveConnection {
public:
    std::string name;
    McpServerState state_value{McpServerState::Connected};
    std::vector<McpLiveTool> offered_tools;
    bool resources{false};
    std::size_t resource_count_value{0};
    std::optional<std::string> failure;
    std::optional<std::string> stderr;
    bool oauth{false};
    std::string oauth_url_value{"https://mcp.example.com/mcp"};
    bool reconnect_succeeds{true};
    int reconnect_calls{0};
    int sign_out_calls{0};
    int close_calls{0};
    McpLiveConnection::ToolsChangedListener tools_changed;
    McpLiveConnection::ResourcesChangedListener resources_changed;

    [[nodiscard]] const std::string& server_name() const noexcept override { return name; }
    [[nodiscard]] McpServerState state() const noexcept override { return state_value; }
    [[nodiscard]] const std::vector<McpLiveTool>& tools() const noexcept override { return offered_tools; }
    [[nodiscard]] bool has_resources() const noexcept override { return resources; }
    [[nodiscard]] std::size_t resource_count() const noexcept override { return resource_count_value; }
    [[nodiscard]] const std::optional<std::string>& error() const noexcept override { return failure; }
    [[nodiscard]] const std::optional<std::string>& stderr_tail() const noexcept override { return stderr; }
    [[nodiscard]] bool uses_oauth() const noexcept override { return oauth; }
    [[nodiscard]] const std::string& oauth_url() const noexcept override { return oauth_url_value; }

    [[nodiscard]] support::AsyncResult<void> reconnect() override {
        ++reconnect_calls;
        if (!reconnect_succeeds) {
            state_value = McpServerState::Failed;
            failure = "reconnect failed";
            return support::AsyncResult<void>{std::unexpected(test_error("reconnect failed"))};
        }
        state_value = McpServerState::Connected;
        failure.reset();
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    [[nodiscard]] support::AsyncResult<void> sign_out() override {
        ++sign_out_calls;
        state_value = McpServerState::NeedsAuth;
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    void close() noexcept override { ++close_calls; }

    void set_tools_changed_listener(ToolsChangedListener listener) override {
        tools_changed = std::move(listener);
    }
    void set_resources_changed_listener(ResourcesChangedListener listener) override {
        resources_changed = std::move(listener);
    }

    void fire_tools_changed() {
        if (tools_changed) {
            tools_changed();
        }
    }
};

class ScriptedFactory final : public McpConnectionFactory {
public:
    std::map<std::string, std::shared_ptr<ScriptedConnection>> outcomes;
    std::vector<std::string> connect_order;

    [[nodiscard]] support::AsyncResult<std::shared_ptr<McpLiveConnection>> connect(
            const McpConfigEntry& entry) override {
        connect_order.push_back(entry.name);
        const auto found = outcomes.find(entry.name);
        if (found == outcomes.end()) {
            return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                    std::unexpected(test_error("no scripted connection for " + entry.name))};
        }
        return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                std::shared_ptr<McpLiveConnection>{found->second}};
    }
};

// ── Recording tool surface ──────────────────────────────────────────────────

class RecordingSurface final : public McpToolSurface {
public:
    struct Registration {
        std::string name;
        McpExposure exposure;
    };

    std::vector<Registration> registrations;
    std::optional<McpExposure> resource_exposure;
    std::vector<std::string> resource_servers;
    std::vector<std::string> active;
    std::map<std::string, McpExposure> surface;

    void register_tool(McpRegisteredTool tool) override {
        registrations.push_back(Registration{tool.name, tool.exposure});
        surface[tool.name] = tool.exposure;
    }
    void register_resource_tools(McpExposure exposure, const std::vector<std::string>& servers) override {
        resource_exposure = exposure;
        resource_servers = servers;
        for (const auto name :
                {coding_agent::mcp::kListMcpResourcesTool,
                        coding_agent::mcp::kListMcpResourceTemplatesTool,
                        coding_agent::mcp::kReadMcpResourceTool}) {
            surface[std::string{name}] = exposure;
        }
    }
    void set_active_tools(std::vector<std::string> names) override { active = std::move(names); }
    [[nodiscard]] std::vector<std::string> active_tools() const override { return active; }
    [[nodiscard]] std::vector<McpSurfaceTool> all_tools() const override {
        std::vector<McpSurfaceTool> tools;
        for (const auto& [name, exposure] : surface) {
            tools.push_back(McpSurfaceTool{name, exposure});
        }
        return tools;
    }

    [[nodiscard]] std::optional<McpExposure> last_exposure_of(std::string_view name) const {
        std::optional<McpExposure> exposure;
        for (const auto& registration : registrations) {
            if (registration.name == name) {
                exposure = registration.exposure;
            }
        }
        return exposure;
    }
};

// ── Scripted OAuth driver ───────────────────────────────────────────────────

class ScriptedAuth final : public McpSignInDriver {
public:
    std::optional<std::string> sign_in_result{std::nullopt};
    bool credentials_removed{true};
    int sign_in_calls{0};
    int sign_out_calls{0};

    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            const McpConfigEntry&, const McpSignInPrompt&) override {
        ++sign_in_calls;
        return support::AsyncResult<std::optional<std::string>>{sign_in_result};
    }
    [[nodiscard]] support::AsyncResult<bool> sign_out(const McpConfigEntry&) override {
        ++sign_out_calls;
        return support::AsyncResult<bool>{credentials_removed};
    }
};

// ── Fixtures ────────────────────────────────────────────────────────────────

[[nodiscard]] McpConfigEntry stdio_entry(
        std::string name, bool enabled = true, std::optional<McpExposure> exposure = std::nullopt) {
    McpStdioServerConfig config;
    config.name = name;
    config.command = "python3";
    config.exposure = exposure;
    McpConfigEntry entry;
    entry.name = std::move(name);
    entry.enabled = enabled;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] McpConfigLoad config_with(std::vector<McpConfigEntry> entries) {
    McpConfigLoad load;
    load.servers = std::move(entries);
    return load;
}

struct Harness {
    std::shared_ptr<ScriptedFactory> factory = std::make_shared<ScriptedFactory>();
    std::shared_ptr<RecordingSurface> surface = std::make_shared<RecordingSurface>();
    std::shared_ptr<ScriptedAuth> auth = std::make_shared<ScriptedAuth>();
    std::unique_ptr<McpSessionManager> manager;

    [[nodiscard]] static Harness make(McpConfigLoad config,
            std::vector<std::string> overridden = {},
            std::filesystem::path agent_dir = "/home/u/.pi/agent") {
        Harness harness;
        McpManagerDependencies dependencies;
        dependencies.connections = harness.factory;
        dependencies.tools = harness.surface;
        dependencies.auth = harness.auth;
        harness.manager = std::make_unique<McpSessionManager>(std::move(config),
                std::move(agent_dir),
                "/repo",
                /* project_trusted */ false,
                std::move(overridden),
                std::move(dependencies));
        return harness;
    }
};

[[nodiscard]] std::optional<McpServerSnapshot> snapshot_for(
        const McpSessionManager& manager, std::string_view name) {
    for (const auto& snapshot : manager.servers()) {
        if (snapshot.entry.name == name) {
            return snapshot;
        }
    }
    return std::nullopt;
}

void check_state(const McpSessionManager& manager, std::string_view name, McpServerState expected) {
    const auto snapshot = snapshot_for(manager, name);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->connection.has_value());
    CHECK(snapshot->connection->state == expected);
}

[[nodiscard]] std::shared_ptr<ScriptedConnection> connection_for(
        const Harness& harness, std::string name, McpServerState state = McpServerState::Connected) {
    auto connection = std::make_shared<ScriptedConnection>();
    connection->name = name;
    connection->state_value = state;
    harness.factory->outcomes[name] = connection;
    return connection;
}

/// The `mcp.json` with one enabled stdio server.
[[nodiscard]] std::string stdio_mcp_json(std::string_view name, bool enabled = true) {
    std::string json = "{\n  \"mcpServers\": {\n    \"" + std::string{name} +
                      "\": {\n      \"command\": \"python3\"";
    if (!enabled) {
        json += ",\n      \"enabled\": false";
    }
    json += "\n    }\n  }\n}\n";
    return json;
}

/// Build a harness over a real `mcp.json` written under `workspace` (the
/// persisted path the write half edits), so enable/exposure round-trips.
[[nodiscard]] Harness persisted_harness(tests::TempWorkspace& workspace, std::string_view mcp_json) {
    const auto agent_dir = workspace.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
        output << mcp_json;
    }
    const auto load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), /* project_trusted */ false);
    return Harness::make(load, {}, agent_dir);
}

} // namespace

// ── Connection state ────────────────────────────────────────────────────────

TEST_CASE("a server whose tools/list fails after connect reports failed with the stderr tail",
        "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("dead")}));
    auto connection = connection_for(harness, "dead", McpServerState::Failed);
    connection->failure = "MCP server sent an invalid tools/list result";
    connection->stderr = "traceback line one\ntraceback line two";

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());

    const auto snapshot = snapshot_for(*harness.manager, "dead");
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->connection.has_value());
    // The separation case: a server that connects but cannot list tools is
    // failed, not connected, and its failure carries the stderr tail.
    CHECK(snapshot->connection->state == McpServerState::Failed);
    CHECK(snapshot->connection->error == std::optional<std::string>{"MCP server sent an invalid tools/list result"});
    CHECK(snapshot->connection->stderr_tail == std::optional<std::string>{"traceback line one\ntraceback line two"});
}

TEST_CASE("a healthy server reports connected and registers its tools", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("echo")}));
    auto connection = connection_for(harness, "echo");
    connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());

    const auto snapshot = snapshot_for(*harness.manager, "echo");
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->connection.has_value());
    CHECK(snapshot->connection->state == McpServerState::Connected);
    CHECK(snapshot->connection->tools.size() == 1);
    CHECK(harness.surface->last_exposure_of("mcp__echo__echo") == McpExposure::Codemode);
}

// ── Enable/disable persistence ──────────────────────────────────────────────

TEST_CASE("a disabled server stays disabled across a manager rebuild via the mcp.json round-trip",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
        output << "{\n  \"mcpServers\": {\n    \"echo\": {\n      \"command\": \"python3\"\n    }\n  }\n}\n";
    }

    const auto first_load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), /* project_trusted */ false);
    REQUIRE(first_load.errors.empty());
    auto first = Harness::make(first_load, {}, agent_dir);
    auto connection = connection_for(first, "echo");

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(first.manager->start()).has_value());
    auto outcome = runtime.run(first.manager->set_enabled("echo", false));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(connection->close_calls == 1);

    // A fresh manager over the reloaded file (the rebuild) sees the disable:
    // the server is stopped, not connected, and never connects.
    const auto second_load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), false);
    REQUIRE(second_load.errors.empty());
    REQUIRE(second_load.servers.size() == 1);
    CHECK_FALSE(second_load.servers.front().enabled);

    auto second = Harness::make(second_load, {}, agent_dir);
    REQUIRE(runtime.run(second.manager->start()).has_value());
    CHECK(second.factory->connect_order.empty());
    const auto snapshot = snapshot_for(*second.manager, "echo");
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->connection.has_value());
}

TEST_CASE("an enable persists across a manager rebuild too", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto agent_dir = workspace.path() / "agent";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
        output << "{\n  \"mcpServers\": {\n    \"echo\": {\n      \"command\": \"python3\",\n      \"enabled\": false\n    }\n"
                  "  }\n}\n";
    }

    const auto first_load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), false);
    auto first = Harness::make(first_load, {}, agent_dir);
    auto connection = connection_for(first, "echo");

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(first.manager->start()).has_value());
    CHECK(first.factory->connect_order.empty());
    auto outcome = runtime.run(first.manager->set_enabled("echo", true));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(first.factory->connect_order == std::vector<std::string>{"echo"});

    const auto second_load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), false);
    REQUIRE(second_load.servers.size() == 1);
    CHECK(second_load.servers.front().enabled);
}

// ── Reconnect ───────────────────────────────────────────────────────────────

TEST_CASE("reconnect restores a failed server and registers its tools", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("echo")}));
    auto connection = connection_for(harness, "echo", McpServerState::Failed);
    connection->failure = "boom";
    connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    check_state(*harness.manager, "echo", McpServerState::Failed);

    auto outcome = runtime.run(harness.manager->reconnect("echo"));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(connection->reconnect_calls == 1);
    check_state(*harness.manager, "echo", McpServerState::Connected);
    CHECK(harness.surface->last_exposure_of("mcp__echo__echo") == McpExposure::Codemode);
}

TEST_CASE("reconnect of a disabled server reports pi's message", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("off", /* enabled */ false)}));
    tests::RuntimeFixture runtime;
    auto outcome = runtime.run(harness.manager->reconnect("off"));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK(**outcome == "MCP server \"off\" is disabled.");
}

// ── Withdrawal as hidden ────────────────────────────────────────────────────

TEST_CASE("a withdrawn tool re-registers as hidden while the kept tool keeps its exposure",
        "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("echo")}));
    auto connection = connection_for(harness, "echo");
    connection->offered_tools = {
            McpLiveTool{.name = "alpha", .description = "a"},
            McpLiveTool{.name = "beta", .description = "b"},
    };

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    CHECK(harness.surface->last_exposure_of("mcp__echo__alpha") == McpExposure::Codemode);
    CHECK(harness.surface->last_exposure_of("mcp__echo__beta") == McpExposure::Codemode);

    // The server drops `beta` and reports `tools/list_changed`.
    connection->offered_tools = {McpLiveTool{.name = "alpha", .description = "a"}};
    connection->fire_tools_changed();

    CHECK(harness.surface->last_exposure_of("mcp__echo__alpha") == McpExposure::Codemode);
    CHECK(harness.surface->last_exposure_of("mcp__echo__beta") == McpExposure::Hidden);
}

TEST_CASE("disabling a server hides every tool it registered", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    auto harness = persisted_harness(workspace, stdio_mcp_json("echo"));
    auto connection = connection_for(harness, "echo");
    connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    auto outcome = runtime.run(harness.manager->set_enabled("echo", false));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(harness.surface->last_exposure_of("mcp__echo__echo") == McpExposure::Hidden);
}

// ── Exposure ────────────────────────────────────────────────────────────────

TEST_CASE("an exposure change re-registers the server's tools at the new exposure",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    auto harness = persisted_harness(workspace, stdio_mcp_json("echo"));
    auto connection = connection_for(harness, "echo");
    connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    CHECK(harness.surface->last_exposure_of("mcp__echo__echo") == McpExposure::Codemode);

    auto outcome = runtime.run(harness.manager->set_exposure("echo", McpExposure::Direct));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(harness.surface->last_exposure_of("mcp__echo__echo") == McpExposure::Direct);
}

// ── Resource tools ──────────────────────────────────────────────────────────

TEST_CASE("the resource tools take the widest non-hidden exposure of resource-bearing servers",
        "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({
            stdio_entry("codemode", true, McpExposure::Codemode),
            stdio_entry("direct", true, McpExposure::Direct),
    }));
    auto first = connection_for(harness, "codemode");
    first->resources = true;
    first->resource_count_value = 2;
    auto second = connection_for(harness, "direct");
    second->resources = true;
    second->resource_count_value = 1;

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    REQUIRE(harness.surface->resource_exposure.has_value());
    CHECK(*harness.surface->resource_exposure == McpExposure::Direct);
    CHECK(harness.surface->resource_servers == std::vector<std::string>{"codemode", "direct"});

    // Separation: when only the codemode server bears resources, the resource
    // tools fall back to codemode (never direct).
    auto only_codemode = Harness::make(config_with({stdio_entry("codemode", true, McpExposure::Codemode)}));
    auto connection = connection_for(only_codemode, "codemode");
    connection->resources = true;
    REQUIRE(runtime.run(only_codemode.manager->start()).has_value());
    REQUIRE(only_codemode.surface->resource_exposure.has_value());
    CHECK(*only_codemode.surface->resource_exposure == McpExposure::Codemode);
}

TEST_CASE("the notification router maps the two list_changed notifications to the connection listeners",
        "[coding_agent][mcp][issue884][spec]") {
    int tools_changes = 0;
    int resource_changes = 0;
    coding_agent::runtime::McpLiveNotificationRouter router(
            [&tools_changes]() { ++tools_changes; }, [&resource_changes]() { ++resource_changes; });

    const support::JsonValue params{support::JsonValue::object_t{}};
    router.dispatch("notifications/tools/list_changed", params);
    CHECK(tools_changes == 1);
    CHECK(resource_changes == 0);

    router.dispatch("notifications/resources/list_changed", params);
    CHECK(resource_changes == 1);
    // The resource lane's consumer shape routes to the same listener.
    router.on_resources_changed();
    CHECK(resource_changes == 2);

    // An unrelated notification is ignored: the separation case.
    router.dispatch("notifications/message", params);
    CHECK(tools_changes == 1);
    CHECK(resource_changes == 2);
}

// ── OAuth actions ───────────────────────────────────────────────────────────

TEST_CASE("sign-in runs the flow and reconnects", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("remote")}));
    auto connection = connection_for(harness, "remote", McpServerState::NeedsAuth);
    connection->oauth = true;
    connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};
    harness.auth->sign_in_result = std::nullopt;

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    auto outcome = runtime.run(harness.manager->sign_in("remote", McpSignInPrompt{}));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->has_value());
    CHECK(harness.auth->sign_in_calls == 1);
    CHECK(connection->reconnect_calls == 1);
    check_state(*harness.manager, "remote", McpServerState::Connected);
    CHECK(harness.surface->last_exposure_of("mcp__remote__echo") == McpExposure::Codemode);
}

TEST_CASE("sign-in of a non-OAuth server reports pi's message", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("plain")}));
    static_cast<void>(connection_for(harness, "plain"));
    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    auto outcome = runtime.run(harness.manager->sign_in("plain", McpSignInPrompt{}));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->has_value());
    CHECK(**outcome == "MCP server \"plain\" does not use OAuth.");
    CHECK(harness.auth->sign_in_calls == 0);
}

TEST_CASE("sign-out removes credentials and signs the connection out", "[coding_agent][mcp][issue884][spec]") {
    auto harness = Harness::make(config_with({stdio_entry("remote")}));
    auto connection = connection_for(harness, "remote");
    connection->oauth = true;

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(harness.manager->start()).has_value());
    auto outcome = runtime.run(harness.manager->sign_out("remote"));
    REQUIRE(outcome.has_value());
    CHECK(*outcome);
    CHECK(harness.auth->sign_out_calls == 1);
    CHECK(connection->sign_out_calls == 1);
}
