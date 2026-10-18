#include <catch2/catch_test_macros.hpp>

#include "coding_agent/cli/FrontendSelection.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/Settings.hpp>

using namespace cch;

TEST_CASE("frontend selection follows pi precedence", "[cli][selection][issue64][spec]") {
    cli::CliConfig config;
    const cli::FrontendEnvironment interactive{
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
    };

    auto selected = cli::select_frontend(config, interactive);
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Interactive);

    config.print = true;
    selected = cli::select_frontend(config, interactive);
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);
}

TEST_CASE(
        "tui mode selection resolves the explicit flag over the stored setting", "[cli][selection][issue1017][spec]") {
    tests::TempWorkspace workspace;
    const auto cwd = workspace.path();
    const auto agent_dir = workspace.path() / "agent";

    workspace.write("agent/settings.json", R"({"tuiMode": "regular"})");
    auto stored_regular = coding_agent::SettingsManager::create(cwd, agent_dir, /* project_trusted */ false);
    REQUIRE(stored_regular.errors().empty());
    CHECK(stored_regular.get_tui_mode() == coding_agent::TuiMode::Regular);
    CHECK(cli::select_tui_mode(cli::CliConfig{}, stored_regular) == coding_agent::TuiMode::Regular);

    // pi interactive-mode.ts `options.tuiMode ?? settingsManager.getTuiMode()`:
    // the explicit `--tui-mode` wins in both directions.
    cli::CliConfig explicit_fullscreen;
    explicit_fullscreen.tui_mode = coding_agent::TuiMode::Fullscreen;
    CHECK(cli::select_tui_mode(explicit_fullscreen, stored_regular) == coding_agent::TuiMode::Fullscreen);

    workspace.write("agent/settings.json", R"({"tuiMode": "fullscreen"})");
    auto stored_fullscreen = coding_agent::SettingsManager::create(cwd, agent_dir, /* project_trusted */ false);
    REQUIRE(stored_fullscreen.errors().empty());
    CHECK(cli::select_tui_mode(cli::CliConfig{}, stored_fullscreen) == coding_agent::TuiMode::Fullscreen);

    cli::CliConfig explicit_regular;
    explicit_regular.tui_mode = coding_agent::TuiMode::Regular;
    CHECK(cli::select_tui_mode(explicit_regular, stored_fullscreen) == coding_agent::TuiMode::Regular);

    // Neither the flag nor the setting: the #1024 product default is the
    // frozen resolution, fullscreen.
    workspace.write("agent/settings.json", R"({})");
    auto unset = coding_agent::SettingsManager::create(cwd, agent_dir, /* project_trusted */ false);
    REQUIRE(unset.errors().empty());
    CHECK(cli::select_tui_mode(cli::CliConfig{}, unset) == coding_agent::TuiMode::Fullscreen);
}

TEST_CASE("text mode leaves terminal-based frontend selection unchanged", "[cli][selection][issue64][spec]") {
    cli::CliConfig config;

    auto selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Interactive);

    selected = cli::select_frontend(config, {
        .stdin_is_terminal = false,
        .stdout_is_terminal = true,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);

    selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = false,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);
}
