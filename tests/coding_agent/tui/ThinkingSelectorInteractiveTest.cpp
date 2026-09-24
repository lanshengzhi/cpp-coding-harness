// End-to-end `/thinking` coverage (#774): the command applies a level
// session-only with pi's `Thinking level: <level>` status, opens the
// thinking selector without an argument, saves the highlighted level as the
// global default through Ctrl+S with pi's `Default thinking level: <level>`
// status, and errors with pi's wording for a level outside the active
// model's supported set. Driven through the VirtualTerminal against a
// session over a temp Agent Config Directory with dummy-only models.json
// values — no live credentials, no network validation.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include "support/AgentRootFixture.hpp"

using namespace cch;
using tests::drain_ready;

namespace {

/// One isolated assembly fixture: a temp workspace for the session file and a
/// temp Agent Config Directory (`HOME`) whose models.json and settings.json
/// drive runtime creation deterministically.
struct Fixture {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::filesystem::path session_file;
    tests::RuntimeFixture runtime;
    std::optional<tests::RuntimeLoopDriver> runtime_driver{std::nullopt};

    Fixture() {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        session_file = workspace.path() / "session.jsonl";
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir / "models.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] std::string read_settings() const {
        std::ifstream in(agent_dir / "settings.json", std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> boot(
        Fixture& fixture, Running& running, bool start_on_plain_model = false) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{fixture.session_file};
    request.execution_runtime_target = fixture.runtime.make_target();
    auto created = fixture.runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created.has_value());
    if (start_on_plain_model) {
        const auto model = created->session->model_runtime()->model("beta", "beta-1");
        REQUIRE(model.has_value());
        REQUIRE(created->session->set_model_blocking(*model).has_value());
    }

    auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                       .with_session(*created->session)
                       .with_agent_config_directory(fixture.agent_dir)
                       .build();

    boost::asio::co_spawn(running.io,
            coding_agent::tui::run_interactive_mode(running.terminal, std::move(run)),
            [&](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                running.run_result.emplace(std::move(result));
            });
    fixture.runtime_driver.emplace(fixture.runtime);
    drain_ready(running.io);
    return std::move(created->session);
}

[[nodiscard]] std::optional<std::string> settings_default_thinking_level(const std::string& settings_text) {
    if (settings_text.empty()) return std::nullopt;
    const auto parsed = support::read_json(settings_text);
    REQUIRE(parsed.has_value());
    const auto& object = parsed->get_object();
    const auto found = object.find("defaultThinkingLevel");
    if (found == object.end()) return std::nullopt;
    const auto* level = found->second.get_if<std::string>();
    REQUIRE(level != nullptr);
    return *level;
}

} // namespace

TEST_CASE("/thinking applies a level session-only and saves the default through the selector",
        "[coding_agent][tui][thinking-selector][e2e][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running);
    REQUIRE(session->snapshot().agent_state.thinking_level == "medium");

    // pi `handleThinkingCommand(level)` → `selectThinkingLevel(level, false)`:
    // the level applies to the session with the `Thinking level: <level>`
    // status and writes no settings default.
    REQUIRE(running.terminal.inject_input("/thinking high\r"));
    drain_ready(running.io);
    auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Thinking level: high") != std::string::npos);
    CHECK(session->snapshot().agent_state.thinking_level == "high");
    CHECK(fixture.read_settings().empty());

    // `/thinking` with no argument opens the thinking selector (pi
    // `showThinkingSelector`) with the cycle hint and the save affordance.
    REQUIRE(running.terminal.inject_input("/thinking\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Thinking Level") != std::string::npos);
    CHECK(screen.find("Shift+Tab cycles thinking levels in-session") != std::string::npos);
    CHECK(screen.find("Ctrl+S to set as default") != std::string::npos);

    // Ctrl+S (`app.thinking.save`) saves the highlighted level — the current
    // one — as the global default with pi's `Default thinking level:
    // <level>` status.
    REQUIRE(running.terminal.inject_input("\x13"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Default thinking level: high") != std::string::npos);
    CHECK(settings_default_thinking_level(fixture.read_settings()) == std::optional<std::string>{"high"});

    // A later `/thinking <level>` stays session-only: the persisted default
    // is untouched (pi `selectThinkingLevel(level, false)`).
    REQUIRE(running.terminal.inject_input("/thinking medium\r"));
    drain_ready(running.io);
    screen = visible_screen(running.terminal);
    CHECK(screen.find("Thinking level: medium") != std::string::npos);
    CHECK(session->snapshot().agent_state.thinking_level == "medium");
    CHECK(settings_default_thinking_level(fixture.read_settings()) == std::optional<std::string>{"high"});

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}

TEST_CASE("/thinking rejects a level outside the active model's supported set",
        "[coding_agent][tui][thinking-selector][e2e][issue774][spec]") {
    Fixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    Running running;
    auto session = boot(fixture, running, true);
    REQUIRE(session->snapshot().agent_state.model.id == "beta-1");
    REQUIRE(session->snapshot().agent_state.thinking_level == "off");

    // pi `handleThinkingCommand`: the argument matches the model's available
    // levels, and anything else errors with pi's wording instead of clamping.
    REQUIRE(running.terminal.inject_input("/thinking high\r"));
    drain_ready(running.io);
    const auto screen = visible_screen(running.terminal);
    CHECK(screen.find("Unknown thinking level \"high\". Available levels: off.") != std::string::npos);
    CHECK(session->snapshot().agent_state.thinking_level == "off");
    CHECK(fixture.read_settings().empty());

    REQUIRE(running.terminal.inject_input("\x04"));
    drain_ready(running.io);
    REQUIRE(running.run_result);
    CHECK(*running.run_result);
}
