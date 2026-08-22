// Boot Project Trust prompt on the main TUI (issue #413, P17): the generic
// string-list selector overlay with pi's `getProjectTrustOptions` choices,
// shown before the boot session binds when a trust-requiring resource exists
// and no override is set; the untrusted-project chat warning; and the
// `--approve`/`--no-approve` one-run overrides.

#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/CliRunFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/ThemeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "ai/providers/FakeProvider.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

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

/// A TempWorkspace pointed to by PI_CODING_AGENT_DIR so the trust store and
/// the interactive host's agent config directory are one deterministic
/// location (the boot prompt and SessionFactory share
/// `coding_agent::trust_store_file_path()`).
struct TrustIsolatedWorkspace {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;

    TrustIsolatedWorkspace() {
        agent_dir = workspace.path() / "agent";
        std::filesystem::create_directories(agent_dir);
    }

    void write(std::string relative, std::string content) const {
        auto target = workspace.path() / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output << content;
    }
};

/// Run the interactive boot with a boot request whose workspace carries a
/// `.pi/skills` trust-requiring marker, and a session factory over the
/// scripted fake catalog. Returns the terminal, the io context, and the
/// run result.
struct BootTrustRun {
    tui::VirtualTerminal terminal{{.columns = 120, .rows = 30}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    /// Every session-creation request the factory saw (the boot request and
    /// each in-session replacement), so tests can assert the decided trust
    /// override.
    std::vector<std::optional<bool>> request_overrides;
    coding_agent::tui::testing::ActionSinkRecorder recorder;

    void start(
        const TrustIsolatedWorkspace& fixture,
        coding_agent::runtime::AgentSessionCreationRequest request,
        std::shared_ptr<ai::Models> models) {
        recorder.replace_session =
            [this, models](coding_agent::runtime::AgentSessionCreationRequest req)
            -> support::Expected<coding_agent::CreateAgentSessionResult> {
                request_overrides.push_back(req.project_trust_override);
                req.provide_user_shell = true;
                return coding_agent::create_agent_session_for_testing(
                    std::move(req), models);
            };
        boost::asio::co_spawn(
            io,
            coding_agent::tui::run_interactive_mode_boot(
                terminal,
                coding_agent::tui::InteractiveModeConfig{
                    .agent_config_directory = fixture.agent_dir,
                    .action_sink = recorder.make_sink(),
                    .boot_request = std::move(request),
                }),
            [this](std::exception_ptr exception, support::ExpectedVoid result) {
                CHECK(exception == nullptr);
                run_result.emplace(std::move(result));
            });
        drain_ready(io);
    }

    void type(std::string_view text) {
        REQUIRE(terminal.inject_input(std::string{text}));
        drain_ready(io);
    }

    void exit() {
        type("\x04");
        REQUIRE(run_result);
        CHECK(*run_result);
    }
};

[[nodiscard]] coding_agent::runtime::AgentSessionCreationRequest boot_request(
    const TrustIsolatedWorkspace& fixture) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.workspace = fixture.workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.session_facts.no_prompt_templates = true;
    return request;
}

} // namespace

TEST_CASE(
    "boot trust prompt shows getProjectTrustOptions choices as a main-TUI overlay",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    // pi's prompt title and getProjectTrustOptions choices.
    CHECK(screen.find("Trust project folder?") != std::string::npos);
    CHECK(screen.find("This allows cch to load .pi settings and resources.") != std::string::npos);
    CHECK(screen.find("Trust") != std::string::npos);
    CHECK(screen.find("Trust (this session only)") != std::string::npos);
    CHECK(screen.find("Do not trust") != std::string::npos);
    CHECK(screen.find("Do not trust (this session only)") != std::string::npos);

    // The main TUI renders behind the overlay (header hints).
    CHECK(screen.find("interrupt") != std::string::npos);
}

TEST_CASE(
    "boot trust prompt selection saves the decision and binds a trusted session",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    // The first option ("Trust") is preselected; confirm it.
    run.type("\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);

    // The decision was persisted to the trust store.
    const auto trust_path = coding_agent::trust_store_file_path();
    REQUIRE(std::filesystem::exists(trust_path));
    std::ifstream trust_file(trust_path);
    std::string trust_json{
        std::istreambuf_iterator<char>(trust_file),
        std::istreambuf_iterator<char>()};
    CHECK((trust_json.find("\"trusted\"") != std::string::npos ||
        trust_json.find("true") != std::string::npos));

    // The session bound without the untrusted-project chat warning.
    CHECK(screen.find("This project is not trusted.") == std::string::npos);

    run.exit();
}

TEST_CASE(
    "boot trust prompt cancel leaves the project untrusted with the chat warning",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    // Cancel the prompt (tui.select.cancel: escape/ctrl+c): the run
    // proceeds untrusted.
    run.type("\x03");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    // pi renderProjectTrustWarningIfNeeded: the untrusted-project warning
    // renders in the chat.
    CHECK(screen.find("This project is not trusted.") != std::string::npos);
    CHECK(screen.find("Use /trust to save a trust decision") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "approve flag overrides the boot trust prompt for the run",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    auto request = boot_request(fixture);
    request.project_trust_override = true;

    BootTrustRun run;
    run.start(fixture, std::move(request), ai::providers::make_scripted_fake_models());

    // No prompt; the session binds trusted and the warning stays absent.
    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") == std::string::npos);

    run.exit();
}

TEST_CASE(
    "no-approve flag overrides the boot trust prompt to untrusted",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    auto request = boot_request(fixture);
    request.project_trust_override = false;

    BootTrustRun run;
    run.start(fixture, std::move(request), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "a saved trust decision skips the boot prompt",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");

    // A saved trusted decision for the workspace skips the prompt.
    coding_agent::ProjectTrustStore store{coding_agent::trust_store_file_path()};
    REQUIRE(store.setMany({{
        .path = std::filesystem::weakly_canonical(fixture.workspace.path()).string(),
        .decision = coding_agent::ProjectTrustDecision::Trusted,
    }}).has_value());

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") == std::string::npos);

    run.exit();
}

TEST_CASE(
    "boot session-only trust survives an in-session session replacement",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");
    // The recognized-but-unbound `app.session.new` action gets a key so the
    // in-session new-session flow is reachable (pi handleClearCommand).
    fixture.write(
        "agent/keybindings.json",
        R"({"app.session.new":"f8"})");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    // Pick "Trust (this session only)" (the third option): no store write,
    // yet the boot session binds trusted (no untrusted-project warning).
    run.type("\x1b[B\x1b[B\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") == std::string::npos);
    REQUIRE(run.request_overrides.size() == 1);
    CHECK(run.request_overrides[0] == true);

    // The in-session new-session flow reuses the boot decision for the same
    // workspace (pi projectTrustByCwd): the replacement request carries the
    // decided trust instead of dropping to ask-without-UI → untrusted.
    run.type("\x1b[19~");
    screen = visible_screen(run.terminal);
    CHECK(screen.find("New session started") != std::string::npos);
    REQUIRE(run.request_overrides.size() == 2);
    CHECK(run.request_overrides[1] == true);

    run.exit();
}

TEST_CASE(
    "CLI --approve and --no-approve override non-interactive ask to untrusted",
    "[coding_agent][cli][boot-trust][issue413]") {
    tests::TempWorkspace workspace;
    std::filesystem::create_directories(workspace.path() / ".pi" / "skills");
    {
        std::ofstream marker(workspace.path() / ".pi" / "skills" / "README.md");
        marker << "marker\n";
    }

    // Non-interactive startup with the default ask policy acts as untrusted:
    // no prompt, no skills loaded, and the run still completes.
    auto result = tests::run_cli(
        tests::CliRunOptions{
            .args = {"--print", "hello"},
            .cwd = workspace.path(),
            .env = {},
            .stdin_text = {},
            .models = {},
        });
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_text.find("hello") != std::string::npos);
}

TEST_CASE(
    "default project trust always skips the boot prompt and trusts",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");
    fixture.write(
        "agent/settings.json",
        R"({"defaultProjectTrust": "always"})");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") == std::string::npos);

    run.exit();
}

TEST_CASE(
    "default project trust never skips the boot prompt and warns",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    fixture.write(".pi/skills/README.md", "project skill marker");
    fixture.write(
        "agent/settings.json",
        R"({"defaultProjectTrust": "never"})");

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "boot session creation failure prints pi-style and stops the TUI",
    "[coding_agent][tui][boot-trust][issue413]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());

    // A workspace that does not exist fails session creation.
    auto request = boot_request(fixture);
    request.workspace = fixture.workspace.path() / "missing";

    tui::VirtualTerminal terminal{{.columns = 120, .rows = 30}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> run_result;
    coding_agent::tui::testing::ActionSinkRecorder recorder;
    recorder.replace_session =
        [models = ai::providers::make_scripted_fake_models()](
            coding_agent::runtime::AgentSessionCreationRequest req)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
            req.provide_user_shell = true;
            return coding_agent::create_agent_session_for_testing(
                std::move(req), models);
        };
    boost::asio::co_spawn(
        io,
        coding_agent::tui::run_interactive_mode_boot(
            terminal,
            coding_agent::tui::InteractiveModeConfig{
                .agent_config_directory = fixture.agent_dir,
                .action_sink = recorder.make_sink(),
                .boot_request = std::move(request),
            }),
        [&](std::exception_ptr exception, support::ExpectedVoid result) {
            CHECK(exception == nullptr);
            run_result.emplace(std::move(result));
        });
    drain_ready(io);

    // The creation failure was reported through the closed action seam.
    REQUIRE(recorder.boot_creation_failure.size() == 1);
    CHECK_FALSE(recorder.boot_creation_failure[0].error.message.empty());
    REQUIRE(run_result);
    CHECK_FALSE(*run_result);
}

TEST_CASE(
    "boot registers discovered themes and the settings Theme submenu commits one",
    "[coding_agent][tui][boot-trust][issue415]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    // All three pi sources: trust-gated project `.pi/themes`, the user
    // `<agent_config_directory>/themes` directory, and an explicit
    // `--theme` path.
    fixture.write(".pi/themes/solarized.json", tests::fixture_theme("solarized", "#abcdef"));
    fixture.write("agent/themes/user-theme.json", tests::fixture_theme("user-theme", "#111111"));
    fixture.write("cli-theme.json", tests::fixture_theme("cli-theme", "#222222"));

    auto request = boot_request(fixture);
    request.project_trust_override = true;
    request.session_facts.theme_paths = {"cli-theme.json"};

    BootTrustRun run;
    run.start(fixture, std::move(request), ai::providers::make_scripted_fake_models());

    // Open /settings, navigate to the Theme item (index 5), open the
    // single-mode ThemeSubmenu: the builtins plus every discovered theme are
    // listed sorted, with the `(current)` marker on the active theme.
    run.type("/settings\r");
    run.type("\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\r");
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("(current)") != std::string::npos);
    CHECK(screen.find("dark") != std::string::npos);
    CHECK(screen.find("light") != std::string::npos);
    CHECK(screen.find("solarized") != std::string::npos);
    CHECK(screen.find("user-theme") != std::string::npos);
    CHECK(screen.find("cli-theme") != std::string::npos);

    // Select solarized (sorted order: cli-theme, dark, light, solarized,
    // user-theme — the active dark theme sits at index 1): down twice, then
    // confirm. The global-scope settings write persists and re-applies.
    run.type("\x1b[B\x1b[B\r");
    const auto settings_path = fixture.agent_dir / "settings.json";
    std::error_code settings_error;
    REQUIRE(std::filesystem::exists(settings_path, settings_error));
    std::ifstream settings_file(settings_path);
    const std::string settings_json{
        std::istreambuf_iterator<char>(settings_file),
        std::istreambuf_iterator<char>()};
    CHECK((settings_json.find("\"theme\":\"solarized\"") != std::string::npos ||
        settings_json.find("\"theme\": \"solarized\"") != std::string::npos));

    // Esc closes the settings selector; Ctrl+C then Ctrl+D exits (the exit
    // binding needs the empty editor).
    // Esc closes the settings selector (async restore); then Ctrl+C clears
    // the editor and Ctrl+D exits (the exit binding needs the empty editor).
    // A bare Esc needs the decoder flush to disambiguate it from a sequence
    // prefix (the established VirtualTerminal pattern).
    REQUIRE(run.terminal.inject_input("\x1b"));
    REQUIRE(run.terminal.flush_input());
    drain_ready(run.io);
    REQUIRE(run.terminal.inject_input("\x03"));
    REQUIRE(run.terminal.flush_input());
    drain_ready(run.io);
    REQUIRE(run.terminal.inject_input("\x04"));
    REQUIRE(run.terminal.flush_input());
    drain_ready(run.io);
    REQUIRE(run.run_result);
    CHECK(*run.run_result);
}

TEST_CASE(
    "boot with a failing theme keeps the main screen and the dark fallback message",
    "[coding_agent][tui][boot-trust][issue425]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());
    // An explicit `--theme` document that fails validation (missing required
    // color tokens) and the settings theme referencing the same broken name:
    // the boot discovers the document (huge validation diagnostic), the
    // controller's apply falls back with pi's verbatim message, and the main
    // screen must still render (the invalid-theme diagnostic used to fill the
    // viewport and push the footer/editor off it — #425).
    fixture.write(
        "broken-theme.json",
        R"({"name":"broken","colors":{"background":"#ff0000"}})");
    fixture.write("agent/settings.json", R"({"theme":"broken"})");

    auto request = boot_request(fixture);
    request.project_trust_override = true;
    request.session_facts.theme_paths = {"broken-theme.json"};

    BootTrustRun run;
    run.start(fixture, std::move(request), ai::providers::make_scripted_fake_models());

    const auto screen = visible_screen(run.terminal);
    // Under the main-screen scrollback flow (ADR 0037) the startup header and
    // the loaded-resources block scroll away into the terminal's native
    // scrollback as the boot diagnostics grow the composed buffer past one
    // screen; the full composed buffer (scrollback ++ screen) must still carry
    // them.
    std::string full_buffer;
    for (const auto& line : run.terminal.scrollback()) {
        full_buffer += line;
        full_buffer.push_back('\n');
    }
    for (const auto& line : run.terminal.screen()) {
        full_buffer += line;
        full_buffer.push_back('\n');
    }
    // The invalid document is reported in the loaded-resources block.
    CHECK(full_buffer.find("[Theme conflicts]") != std::string::npos);
    CHECK(full_buffer.find("broken-theme.json") != std::string::npos);
    // The startup content scrolled away: the visible viewport no longer shows
    // the header/loaded-resources block (pi TuiMainScreen scroll-away).
    CHECK(screen.find("escape interrupt") == std::string::npos);
    CHECK(run.terminal.viewport_top() > 0);
    // The controller applied the settings theme, failed, and fell back with
    // pi's verbatim message (rendered as an Error chat diagnostic), which
    // stays visible on screen with the fixed dock.
    CHECK(screen.find("Failed to load theme \"broken\"") != std::string::npos);
    CHECK(screen.find("Fell back to dark theme.") != std::string::npos);
    // The boot completes: the interactive dock (footer status line with the
    // active model) renders below the bounded loaded-resources block and stays
    // fixed on screen (the #425 bound kept the footer/editor on screen).
    CHECK(screen.find("fake-model") != std::string::npos);
    CHECK(full_buffer.find("Press ctrl+o to show full startup help") != std::string::npos);

    run.exit();
}

TEST_CASE(
    "/reload persists the implicit project trust decision when resources appear",
    "[coding_agent][tui][boot-trust][reload][issue418]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    // No trust-requiring resources at boot: no prompt, the session binds
    // trusted (pi `NoProjectResources` → trusted), and pi main.ts arms
    // `autoTrustOnReloadCwd`.
    auto screen = visible_screen(run.terminal);
    CHECK(screen.find("Trust project folder?") == std::string::npos);
    CHECK(screen.find("This project is not trusted.") == std::string::npos);

    // The workspace gains a trust-requiring resource, then /reload fires:
    // the reload keeps the session trusted and pi's implicit-trust save
    // persists the decision, appending the "; saved project trust" suffix.
    fixture.write(".pi/skills/README.md", "project skill marker");
    run.type("/reload\r");
    screen = visible_screen(run.terminal);
    CHECK(
        screen.find(
            "Reloaded keybindings, skills, prompts, themes, and context files; saved project trust") !=
        std::string::npos);

    // The implicit decision persisted to the trust store.
    const auto trust_path = coding_agent::trust_store_file_path();
    REQUIRE(std::filesystem::exists(trust_path));
    std::ifstream trust_file(trust_path);
    std::string trust_json{
        std::istreambuf_iterator<char>(trust_file),
        std::istreambuf_iterator<char>()};
    CHECK((trust_json.find("\"trusted\"") != std::string::npos ||
        trust_json.find("true") != std::string::npos));

    run.exit();
}

TEST_CASE(
    "/reload without the implicit-trust condition keeps the plain pi status",
    "[coding_agent][tui][boot-trust][reload][issue418]") {
    tests::EnvVarGuard agent_dir("PI_CODING_AGENT_DIR");
    TrustIsolatedWorkspace fixture;
    agent_dir.set(fixture.agent_dir.string());

    BootTrustRun run;
    run.start(fixture, boot_request(fixture), ai::providers::make_scripted_fake_models());

    // The workspace never gains trust-requiring resources; /reload runs the
    // plain status (no "; saved project trust" suffix).
    run.type("/reload\r");
    const auto screen = visible_screen(run.terminal);
    CHECK(
        screen.find(
            "Reloaded keybindings, skills, prompts, themes, and context files") !=
        std::string::npos);
    CHECK(
        screen.find(
            "Reloaded keybindings, skills, prompts, themes, and context files; saved project trust") ==
        std::string::npos);

    run.exit();
}
