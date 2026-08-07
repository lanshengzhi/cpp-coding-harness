#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cli/FrontendSelection.hpp"

using namespace cch;

TEST_CASE("frontend selection follows pi precedence", "[cli][selection][issue64]") {
    cli::CliConfig config;
    const cli::FrontendEnvironment interactive{
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
        .native_tui_supported = true,
    };

    auto selected = cli::select_frontend(config, interactive);
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::NativeTui);

    config.print = true;
    selected = cli::select_frontend(config, interactive);
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);
}

TEST_CASE("text mode leaves terminal-based frontend selection unchanged", "[cli][selection][issue64]") {
    cli::CliConfig config;
    config.output_mode = cli::OutputMode::Text;

    auto selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
        .native_tui_supported = true,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::NativeTui);

    selected = cli::select_frontend(config, {
        .stdin_is_terminal = false,
        .stdout_is_terminal = true,
        .native_tui_supported = true,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);

    selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = false,
        .native_tui_supported = true,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);
}

TEST_CASE("unsupported interactive frontend fails before session assembly", "[cli][selection][issue64]") {
    cli::CliConfig config;
    auto selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
        .native_tui_supported = false,
    });

    REQUIRE_FALSE(selected);
    CHECK(selected.error().message.find("Native TUI") != std::string::npos);

    config.print = true;
    selected = cli::select_frontend(config, {
        .stdin_is_terminal = true,
        .stdout_is_terminal = true,
        .native_tui_supported = false,
    });
    REQUIRE(selected);
    CHECK(*selected == cli::Frontend::Print);
}
