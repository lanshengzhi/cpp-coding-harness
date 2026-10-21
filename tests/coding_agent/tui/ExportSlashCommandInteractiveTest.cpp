// E2E `/export` dispatch (#1044): pi v1.3.0 `handleExportCommand` semantics
// — a ".jsonl" destination takes the transcript lane; every other spelling
// (including no destination at all) exports pi-faithful HTML, defaulting to
// `${APP_NAME}-session-${basename}.html` next to the session file, with pi's
// status line `Session exported to: <path>`. The HTML assertions open the
// produced file: every named template slot must be substituted and the
// base64 SESSION_DATA payload must be present (the harness has no
// `renderedTools` emitters, so pi's client degrades to the tool-name header).

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
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
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace cch;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

struct Fixture {
    tests::TempWorkspace workspace;
    std::filesystem::path session_path{workspace.path() / "sessions" / "dispatch-test.jsonl"};
    tests::RuntimeFixture runtime;
    std::optional<tests::RuntimeLoopDriver> runtime_driver;
    std::unique_ptr<coding_agent::AgentSession> session;

    void create_session() {
        tests::ModelsSessionOptions options;
        options.session_facts.no_skills = true;
        options.session_facts.no_prompt_templates = true;
        // A persisted session: the export lane requires the session file.
        options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{.path = session_path};
        options.workspace = workspace.path();
        options.request_model = tests::scripted_request_model("fake", "fake-model");
        options.execution_runtime_target = runtime.make_target();
        auto models = tests::models_from_provider(tests::make_scripted_fake_provider());
        auto created = runtime.run(coding_agent::create_agent_session_async(
                std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
        REQUIRE(created.has_value());
        runtime_driver.emplace(runtime);
        session = std::move(created->session);
    }
};

struct Running {
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 120, .rows = 40}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
};

void start_interactive(Fixture& fixture, Running& running) {
    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*fixture.session)
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

void finish_interactive(Running& running) {
    REQUIRE(running.terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(
            running.io, [&running] { return running.run_result.has_value(); }, std::chrono::seconds{10}));
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

} // namespace

TEST_CASE("/export with no destination writes pi's default HTML next to the session file",
        "[coding_agent][tui][export][e2e][issue1044][spec]") {
    Fixture fixture;
    fixture.create_session();
    Running running;
    start_interactive(fixture, running);

    // One scripted exchange so the exported payload carries real entries.
    REQUIRE(running.terminal.inject_input("export me\r"));
    REQUIRE(wait_for_text(running, "fake: export me"));

    REQUIRE(running.terminal.inject_input("/export\r"));
    const auto expected = (fixture.session_path.parent_path() / "pike-session-dispatch-test.html").string();
    REQUIRE(wait_for_text(running, "Session exported to: " + expected));

    // Open the produced file: pi's substitution contract holds on the real
    // wiring (the vendored assets resolve from the source tree here).
    REQUIRE(std::filesystem::exists(expected));
    const auto html = read_file(expected);
    CHECK(html.starts_with("<!DOCTYPE html>"));
    for (const std::string_view slot : {"{{CSS}}", "{{JS}}", "{{SESSION_DATA}}", "{{MARKED_JS}}",
                 "{{THEME_VARS}}", "{{BODY_BG}}", "{{CONTAINER_BG}}", "{{INFO_BG}}"}) {
        CAPTURE(slot);
        CHECK(html.find(slot) == std::string::npos);
    }
    CHECK(html.find("const sessionData = \"") != std::string::npos);
    CHECK(html.find("--exportPageBg: #100014;") != std::string::npos);

    finish_interactive(running);
}

TEST_CASE("/export with a .jsonl destination keeps the transcript lane",
        "[coding_agent][tui][export][e2e][issue1044][spec]") {
    Fixture fixture;
    fixture.create_session();
    Running running;
    start_interactive(fixture, running);

    REQUIRE(running.terminal.inject_input("export me\r"));
    REQUIRE(wait_for_text(running, "fake: export me"));

    const auto destination = (fixture.workspace.path() / "transcript.jsonl").string();
    REQUIRE(running.terminal.inject_input("/export " + destination + "\r"));
    REQUIRE(wait_for_text(running, "Transcript exported to " + destination));

    REQUIRE(std::filesystem::exists(destination));
    const auto transcript = read_file(destination);
    CHECK(transcript.starts_with(R"({"type":"summary")"));
    CHECK(transcript.find(R"("fake: export me")") != std::string::npos);

    finish_interactive(running);
}
