// The `/mcp` manager presenter (spec #882, ticket #884): the manager-side
// producer `McpManagerView`'s documented seam expects. It maps the live
// `McpSessionManager` onto the panel's `McpServerView`, builds the screens
// through the panel's pure menu builders, and executes a confirmed
// `McpServerAction` against the live manager.
//
// The manager is driven by scripted seams here too, so the presenter's mapping
// and action routing are asserted without a process, socket, or browser.

#include "coding_agent/mcp/McpConfigFile.hpp"
#include "coding_agent/mcp/McpExposure.hpp"
#include "coding_agent/runtime/McpSessionManager.hpp"
#include "coding_agent/tui/McpManagerPresenter.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
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
using coding_agent::runtime::McpServerState;
using coding_agent::runtime::McpSessionManager;
using coding_agent::runtime::McpSignInDriver;
using coding_agent::runtime::McpSignInPrompt;
using coding_agent::runtime::McpSurfaceTool;
using coding_agent::runtime::McpToolSurface;

class TestConnection final : public McpLiveConnection {
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
        state_value = McpServerState::Connected;
        failure.reset();
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    [[nodiscard]] support::AsyncResult<void> sign_out() override { return support::AsyncResult<void>{support::ExpectedVoid{}}; }
    void close() noexcept override {}
    void set_tools_changed_listener(ToolsChangedListener listener) override { tools_changed = std::move(listener); }
    void set_resources_changed_listener(ResourcesChangedListener listener) override {
        resources_changed = std::move(listener);
    }

    ToolsChangedListener tools_changed;
    ResourcesChangedListener resources_changed;
};

class TestFactory final : public McpConnectionFactory {
public:
    std::map<std::string, std::shared_ptr<TestConnection>> outcomes;
    std::vector<std::string> connect_order;
    [[nodiscard]] support::AsyncResult<std::shared_ptr<McpLiveConnection>> connect(const McpConfigEntry& entry) override {
        connect_order.push_back(entry.name);
        const auto found = outcomes.find(entry.name);
        if (found == outcomes.end()) {
            return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                    std::unexpected(support::make_error(support::ErrorCode::Process, "no connection"))};
        }
        return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                std::shared_ptr<McpLiveConnection>{found->second}};
    }
};

class TestSurface final : public McpToolSurface {
public:
    std::vector<std::string> active;
    std::map<std::string, McpExposure> surface;
    void register_tool(McpRegisteredTool tool) override { surface[tool.name] = tool.exposure; }
    void register_resource_tools(McpExposure, const std::vector<std::string>&) override {}
    void set_active_tools(std::vector<std::string> names) override { active = std::move(names); }
    [[nodiscard]] std::vector<std::string> active_tools() const override { return active; }
    [[nodiscard]] std::vector<McpSurfaceTool> all_tools() const override {
        std::vector<McpSurfaceTool> tools;
        for (const auto& [name, exposure] : surface) {
            tools.push_back(McpSurfaceTool{name, exposure});
        }
        return tools;
    }
};

class TestAuth final : public McpSignInDriver {
public:
    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            const McpConfigEntry&, const McpSignInPrompt&) override {
        return support::AsyncResult<std::optional<std::string>>{std::nullopt};
    }
    [[nodiscard]] support::AsyncResult<bool> sign_out(const McpConfigEntry&) override {
        return support::AsyncResult<bool>{true};
    }
};

[[nodiscard]] McpConfigEntry stdio_entry(std::string name, bool enabled = true) {
    McpStdioServerConfig config;
    config.name = name;
    config.command = "python3";
    McpConfigEntry entry;
    entry.name = std::move(name);
    entry.enabled = enabled;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

struct Fixture {
    std::shared_ptr<TestFactory> factory = std::make_shared<TestFactory>();
    std::shared_ptr<TestSurface> surface = std::make_shared<TestSurface>();
    std::shared_ptr<TestAuth> auth = std::make_shared<TestAuth>();
    std::unique_ptr<McpSessionManager> manager;
    std::unique_ptr<coding_agent::tui::McpManagerPresenter> presenter;
    std::shared_ptr<TestConnection> connection;

    static Fixture make(std::vector<McpConfigEntry> entries) {
        Fixture fixture;
        const std::string first_name = entries.empty() ? std::string{} : entries.front().name;
        McpConfigLoad load;
        load.servers = std::move(entries);
        return build(std::move(fixture), std::move(load), "/home/u/.pi/agent", first_name);
    }

    /// Build a fixture over a real `mcp.json` under `workspace`, so the
    /// enable/disable write half edits a file that exists.
    static Fixture make_persisted(tests::TempWorkspace& workspace, std::string_view server_name, bool enabled) {
        const auto agent_dir = workspace.path() / "agent";
        std::filesystem::create_directories(agent_dir);
        std::string json = "{\n  \"mcpServers\": {\n    \"" + std::string{server_name} + "\": {\n      \"command\": \"python3\"";
        if (!enabled) {
            json += ",\n      \"enabled\": false";
        }
        json += "\n    }\n  }\n}\n";
        {
            std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
            output << json;
        }
        auto load = coding_agent::mcp::load_mcp_config(agent_dir, workspace.path(), false);
        McpConfigLoad config;
        config.servers = std::move(load.servers);
        std::string name{server_name};
        return build(Fixture{}, std::move(config), agent_dir.string(), name);
    }

private:
    static Fixture build(Fixture fixture, McpConfigLoad load, std::string agent_dir, std::string first_name) {
        McpManagerDependencies dependencies;
        dependencies.connections = fixture.factory;
        dependencies.tools = fixture.surface;
        dependencies.auth = fixture.auth;
        fixture.manager = std::make_unique<McpSessionManager>(
                std::move(load), std::move(agent_dir), "/repo", false, std::vector<std::string>{}, std::move(dependencies));
        fixture.presenter = std::make_unique<coding_agent::tui::McpManagerPresenter>(*fixture.manager);
        fixture.connection = std::make_shared<TestConnection>();
        fixture.connection->name = std::move(first_name);
        fixture.factory->outcomes[fixture.connection->name] = fixture.connection;
        return fixture;
    }
};

} // namespace

TEST_CASE("McpManagerPresenter maps manager state onto the panel's server view",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto fixture = Fixture::make({stdio_entry("echo")});
    fixture.connection->offered_tools = {
            McpLiveTool{.name = "alpha", .description = "a"},
            McpLiveTool{.name = "beta", .description = "b"},
    };
    fixture.connection->resources = true;
    fixture.connection->resource_count_value = 2;
    fixture.connection->oauth = true;

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(fixture.manager->start()).has_value());

    const auto views = fixture.presenter->servers();
    REQUIRE(views.size() == 1);
    REQUIRE(views.front().connection.has_value());
    CHECK(views.front().connection->state == coding_agent::tui::McpServerViewState::Connected);
    CHECK(views.front().connection->tools.size() == 2);
    CHECK(views.front().connection->resource_count == 2);
    CHECK(views.front().connection->oauth);

    const auto menu = fixture.presenter->servers_menu();
    REQUIRE(menu.items.size() == 1);
    REQUIRE(menu.items.front().description.has_value());
    CHECK(*menu.items.front().description == "connected · 2 tools · 2 resources · codemode · global");

    const auto server = fixture.presenter->server_menu("echo");
    CHECK(server.title == "MCP server echo");
    CHECK(server.confirm_label == "select");
}

TEST_CASE("McpManagerPresenter exposes the tools and exposure sub-menus",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto fixture = Fixture::make({stdio_entry("echo")});
    fixture.connection->offered_tools = {McpLiveTool{.name = "echo", .description = "echoes"}};

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(fixture.manager->start()).has_value());

    auto tools = runtime.run(fixture.presenter->run_action("echo", coding_agent::tui::McpServerAction::Tools));
    REQUIRE(tools.has_value());
    REQUIRE(tools->next_menu.has_value());
    CHECK(tools->next_menu->title == "Tools of echo");
    CHECK_FALSE(tools->message.has_value());

    auto exposure = runtime.run(fixture.presenter->run_action("echo", coding_agent::tui::McpServerAction::Exposure));
    REQUIRE(exposure.has_value());
    REQUIRE(exposure->next_menu.has_value());
    CHECK(exposure->next_menu->title == "Exposure of echo");
}

TEST_CASE("McpManagerPresenter runs the enable action against the manager",
        "[coding_agent][tui][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    auto fixture = Fixture::make_persisted(workspace, "off", /* enabled */ false);

    tests::RuntimeFixture runtime;
    REQUIRE(runtime.run(fixture.manager->start()).has_value());
    CHECK(fixture.factory->connect_order.empty());

    auto outcome = runtime.run(fixture.presenter->run_action("off", coding_agent::tui::McpServerAction::Enable));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->message.has_value());
    CHECK(fixture.factory->connect_order == std::vector<std::string>{"off"});
}

TEST_CASE("McpManagerPresenter reports pi's reconnect message for a disabled server",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto fixture = Fixture::make({stdio_entry("off", /* enabled */ false)});
    tests::RuntimeFixture runtime;
    auto outcome = runtime.run(fixture.presenter->run_action("off", coding_agent::tui::McpServerAction::Reconnect));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->message.has_value());
    CHECK(*outcome->message == "MCP server \"off\" is disabled.");
}
