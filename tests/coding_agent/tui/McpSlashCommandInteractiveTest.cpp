// E2E `/mcp` slash wiring (#884): drive the Native TUI against the live MCP
// manager assembled from a persisted mcp.json and the scripted local echo
// server. No live provider credentials or network access.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/VirtualTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using namespace cch;

namespace {

struct Fixture {
    tests::TempWorkspace workspace;
    tests::EnvVarGuard xdg_guard{"XDG_CONFIG_HOME"};
    std::filesystem::path xdg_directory;
    std::filesystem::path agent_dir;
    tests::RuntimeFixture runtime;
    std::optional<tests::RuntimeLoopDriver> runtime_driver;
    std::unique_ptr<coding_agent::AgentSession> session;

    Fixture() {
        xdg_directory = workspace.path() / "xdg";
        xdg_guard.set(xdg_directory.string());
        agent_dir = tests::agent_root_under_xdg(xdg_directory);
        std::error_code error;
        std::filesystem::create_directories(agent_dir, error);
        REQUIRE_FALSE(error);
    }

    void write_mcp_config(bool auto_enable_codemode) const {
        const auto server = std::filesystem::path{CCH_SOURCE_DIR} / "fixtures/pi-mcp/echo_server.py";
        std::string json = "{\n"
                           "  \"mcpServers\": {\n"
                           "    \"echo\": {\n"
                           "      \"command\": \"python3\",\n"
                           "      \"args\": [\"" +
                           server.string() +
                           "\"]\n"
                           "    }\n"
                           "  }";
        if (!auto_enable_codemode) {
            json += ",\n  \"autoEnableCodemode\": false";
        }
        json += "\n}\n";

        std::ofstream output(agent_dir / "mcp.json", std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << json;
        REQUIRE(output.good());
    }

    [[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create_session(bool auto_enable_codemode = true) {
        write_mcp_config(auto_enable_codemode);

        tests::ModelsSessionOptions options;
        options.session_facts.no_skills = true;
        options.session_facts.no_prompt_templates = true;
        options.session_target = coding_agent::InMemorySessionTarget{};
        options.workspace = workspace.path();
        options.request_model = tests::scripted_request_model("fake", "fake-model");
        options.execution_runtime_target = runtime.make_target();
        auto models = tests::models_from_provider(tests::make_scripted_fake_provider());
        auto created = runtime.run(coding_agent::create_agent_session_async(
                std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
        REQUIRE(created.has_value());

        runtime_driver.emplace(runtime);
        return std::move(created->session);
    }
};

struct Running {
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 100, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

void start_interactive(Fixture& fixture, Running& running, bool auto_enable_codemode = true) {
    fixture.session = fixture.create_session(auto_enable_codemode);
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*fixture.session)
                       .with_agent_config_directory(fixture.agent_dir)
                       .build();
    boost::asio::co_spawn(running.io,
            coding_agent::tui::run_interactive_mode(running.terminal, std::move(run)),
            [&running](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                running.run_result.emplace(std::move(result));
            });
    tests::drain_ready(running.io);
}

[[nodiscard]] bool wait_for_text(Running& running, std::string_view text) {
    return tests::pump_until(
            running.io,
            [&running, text] { return visible_screen(running.terminal).find(text) != std::string::npos; },
            std::chrono::seconds{20});
}

[[nodiscard]] bool wait_for_text_to_disappear(Running& running, std::string_view text) {
    return tests::pump_until(
            running.io,
            [&running, text] { return visible_screen(running.terminal).find(text) == std::string::npos; },
            std::chrono::seconds{5});
}

void finish_interactive(Running& running) {
    REQUIRE(running.terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(
            running.io, [&running] { return running.run_result.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

} // namespace

TEST_CASE("/mcp opens the servers menu and Escape closes it", "[coding_agent][tui][mcp][e2e][issue884][spec]") {
    Fixture fixture;
    Running running;
    start_interactive(fixture, running);

    REQUIRE(running.terminal.inject_input("/mcp\r"));
    REQUIRE(wait_for_text(running, "MCP servers"));
    CHECK(visible_screen(running.terminal).find("echo") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x1b"));
    REQUIRE(running.terminal.inject_input(""));
    REQUIRE(wait_for_text_to_disappear(running, "MCP servers"));

    finish_interactive(running);
}

TEST_CASE("/mcp reconnect prints pi's server status line", "[coding_agent][tui][mcp][e2e][issue884][spec]") {
    Fixture fixture;
    Running running;
    start_interactive(fixture, running);

    // The first Enter accepts the exact server-name completion; the second
    // submits the command through the slash router.
    REQUIRE(running.terminal.inject_input("/mcp reconnect echo\r\r"));
    // pi's describeState appends the resource count (the echo fixture
    // advertises two resources alongside its five tools).
    constexpr std::string_view expected = "Reconnected to MCP server \"echo\" (connected · 5 tools · 2 resources).";
    REQUIRE(wait_for_text(running, expected));
    CHECK(visible_screen(running.terminal).find(expected) != std::string::npos);

    finish_interactive(running);
}

TEST_CASE("/mcp reports pi's usage warning for an unknown action", "[coding_agent][tui][mcp][e2e][issue884][spec]") {
    Fixture fixture;
    Running running;
    start_interactive(fixture, running);

    constexpr std::string_view usage =
            "Usage: /mcp, /mcp login [server], /mcp logout [server], /mcp reconnect [server]";
    REQUIRE(running.terminal.inject_input("/mcp frobnicate\r"));
    REQUIRE(wait_for_text(running, usage));
    CHECK(visible_screen(running.terminal).find(usage) != std::string::npos);

    finish_interactive(running);
}

TEST_CASE("MCP boot warning is surfaced when codemode auto-enable is disabled",
        "[coding_agent][tui][mcp][e2e][issue884][spec]") {
    Fixture fixture;
    Running running;
    start_interactive(fixture, running, /* auto_enable_codemode */ false);

    // The verbatim warning wraps across the 100-column screen, so assert the
    // head and the reason tail as single rendered lines.
    constexpr std::string_view warning_head = "MCP tools are only reachable from the codemode or tool_search tool,";
    constexpr std::string_view warning_tail = "(autoEnableCodemode is false); they cannot be called.";
    REQUIRE(wait_for_text(running, warning_tail));
    const std::string screen = visible_screen(running.terminal);
    CHECK(screen.find(warning_head) != std::string::npos);
    CHECK(screen.find(warning_tail) != std::string::npos);

    finish_interactive(running);
}
