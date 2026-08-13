// P11 end-to-end: the model selector (Ctrl+L), the scoped-models selector
// (`/scoped-models`), Ctrl+P / Shift+Ctrl+P cycling with pi's scoped
// auth-filtered semantics, Shift+Tab thinking cycling, and `/model` argument
// completion — driven through the VirtualTerminal against a session over a
// temp Agent Config Directory with dummy-only models.json values. No live
// credentials, no network validation.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/VirtualTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

using namespace cch;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`PI_CODING_AGENT_DIR`) whose models.json and
/// settings.json drive runtime creation deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;

    Fixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir.path() / "models.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] std::string read_settings() const {
        std::ifstream in(agent_dir.path() / "settings.json", std::ios::binary);
        return std::string{
            std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{}};
    }
};

/// `alpha` (keyed, reasoning) and `beta` (keyed, non-reasoning).
constexpr std::string_view kReasoningAndPlainKeyed = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha Reasoning", "reasoning": true}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1", "name": "Beta Plain", "reasoning": false}]
    }
  }
})";

struct Running {
    // Terminal first: it must outlive the io_context, whose shutdown destroys
    // the interactive-mode coroutine frame (and its Tui) last.
    tui::VirtualTerminal terminal{tui::VirtualTerminalOptions{.columns = 100, .rows = 40}};
    boost::asio::io_context io;
    std::optional<util::ExpectedVoid> run_result;
};

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(
    Fixture& fixture,
    Running& running,
    std::vector<std::string> models = {}) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.models = std::move(models);
    auto created = coding_agent::create_agent_session(std::move(request));
    REQUIRE(created.has_value());

    boost::asio::co_spawn(
        running.io,
        coding_agent::tui::run_interactive_mode(
            *created->session,
            running.terminal,
            {.agent_config_directory = fixture.agent_dir.path()}),
        [&](std::exception_ptr exception, util::ExpectedVoid result) {
            CHECK(exception == nullptr);
            running.run_result.emplace(std::move(result));
        });
    drain_ready(running.io);
    return std::move(created->session);
}

} // namespace

TEST_CASE(
    "Ctrl+L opens the model selector and Enter selects a model with the Model status",
    "[coding_agent][tui][model-selector][e2e][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);
    REQUIRE(session->model() == "alpha-1");

    // Ctrl+L opens the selector with the current model marked and sorted
    // first (pi `app.model.select`).
    REQUIRE(running.terminal.inject_input("\x0c"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Only showing models from configured providers.") != std::string::npos);
    CHECK(screen.find("alpha-1 [alpha] ✓") != std::string::npos);
    CHECK(screen.find("beta-1 [beta]") != std::string::npos);

    // Down + Enter selects beta-1: the selector closes, the session model
    // switches, and the `Model: beta-1` status lands (pi `handleSelect`).
    REQUIRE(running.terminal.inject_input("\x1b[B"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Model: beta-1") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    // Escape cancels the selector without changing the model.
    REQUIRE(running.terminal.inject_input("\x0c"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "Ctrl+P and Shift+Ctrl+P cycle models with pi statuses; Shift+Tab cycles thinking",
    "[coding_agent][tui][model-selector][e2e][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);
    REQUIRE(session->model() == "alpha-1");
    REQUIRE(session->snapshot().agent_state.thinking_level == "medium");

    // Ctrl+P cycles forward to beta-1 (pi `app.model.cycleForward`).
    REQUIRE(running.terminal.inject_input("\x10"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Switched to Beta Plain") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    // Shift+Ctrl+P cycles backward to alpha-1 (pi `app.model.cycleBackward`).
    REQUIRE(running.terminal.inject_input("\x1b[112;5u"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Switched to Alpha Reasoning (thinking: medium)") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "alpha-1");

    // Shift+Tab cycles the thinking level (pi `app.thinking.cycle`):
    // medium → high → off (wrap over the supported set).
    REQUIRE(running.terminal.inject_input("\x1b[Z"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Thinking level: high") != std::string::npos);
    CHECK(session->snapshot().agent_state.thinking_level == "high");
    REQUIRE(running.terminal.inject_input("\x1b[Z"));
    drain_ready(running.io);
    CHECK(session->snapshot().agent_state.thinking_level == "off");

    // Shift+Tab on the non-reasoning beta model reports no support.
    REQUIRE(running.terminal.inject_input("\x10"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x1b[Z"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Current model does not support thinking") != std::string::npos);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "/model switches on an exact reference and opens the selector pre-filtered otherwise",
    "[coding_agent][tui][model-selector][e2e][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);
    REQUIRE(session->model() == "alpha-1");

    // An exact provider/model reference switches immediately (pi
    // `handleModelCommand`). The first Enter applies the open argument
    // completion (pi's editor consumes the confirm while the menu is open);
    // the second Enter submits.
    REQUIRE(running.terminal.inject_input("/model beta/beta-1\r\r"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Model: beta-1") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    // A partial term with no completion match opens the selector with the
    // search input pre-filled (pi `handleModelCommand` fallback).
    REQUIRE(running.terminal.inject_input("/model zzz\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Only showing models from configured providers.") != std::string::npos);
    CHECK(screen.find("zzz") != std::string::npos);
    CHECK(screen.find("No matching models") != std::string::npos);

    // Escape closes the selector.
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "/model argument completion lists the candidate models through model-search",
    "[coding_agent][tui][model-selector][e2e][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);

    // Type `/model `: the argument completion lists the availability
    // snapshot (pi `getModelSearchText` items: label id, description
    // provider).
    REQUIRE(running.terminal.inject_input("/model "));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("beta-1 — beta") != std::string::npos);
    CHECK(screen.find("alpha-1 — alpha") != std::string::npos);

    // Narrow with a provider prefix and apply with Tab.
    REQUIRE(running.terminal.inject_input("beta"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("beta-1 — beta") != std::string::npos);
    REQUIRE(running.terminal.inject_input("\t"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("/model beta/beta-1") != std::string::npos);
    // Enter dispatches the exact switch.
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    CHECK(session->snapshot().agent_state.model.id == "beta-1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE(
    "/scoped-models enables a session scope, saves enabledModels, and cycling honors it",
    "[coding_agent][tui][model-selector][e2e][issue407]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);

    // /scoped-models opens the selector with both models listed.
    REQUIRE(running.terminal.inject_input("/scoped-models\r"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Model Configuration") != std::string::npos);
    CHECK(screen.find("all enabled") != std::string::npos);
    CHECK(screen.find("alpha-1 [alpha]") != std::string::npos);

    // Enter toggles alpha-1 into an explicit one-model scope; Ctrl+S saves
    // it to settings (pi `app.models.save` → `onPersist`).
    REQUIRE(running.terminal.inject_input("\r"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x13"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    if (screen.find("Model selection saved to settings") == std::string::npos) {
        std::cerr << "SCOPED SAVE SCREEN:\n" << screen << "\nEND\n";
    }
    CHECK(screen.find("Model selection saved to settings") != std::string::npos);
    REQUIRE(session->scoped_models().size() == 1);
    CHECK(session->scoped_models()[0].model.id == "alpha-1");

    const auto settings_text = fixture.read_settings();
    CHECK(settings_text.find("\"enabledModels\"") != std::string::npos);
    CHECK(settings_text.find("alpha/alpha-1") != std::string::npos);

    // Escape closes the selector; Ctrl+P then reports the one-model scope
    // (pi `_cycleScopedModel` with a single eligible model).
    REQUIRE(running.terminal.inject_input("\x1b"));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input(""));
    drain_ready(running.io);
    REQUIRE(running.terminal.inject_input("\x10"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Only one model in scope") != std::string::npos);
    CHECK(session->snapshot().agent_state.model.id == "alpha-1");

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
