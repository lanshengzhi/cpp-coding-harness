#include "coding_agent/prompt/BuiltinSlashCommands.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace cch;

TEST_CASE(
        "builtin slash autocomplete carries pi's verbatim entries", "[coding_agent][slash-commands][issue419][spec]") {
    const auto& commands = coding_agent::prompt::builtin_slash_commands();

    // The 18 autocomplete entries of pi's 22-command catalog; `thinking` is
    // pi's own entry (issue #791). Router-only spellings (`/clear`, `/help`,
    // `/commands`, `/exit`, `/q`, `/models`) come from the router's spelling
    // table instead of this catalog.
    const std::vector<std::string_view> expected_names{
            "settings",
            "model",
            "scoped-models",
            "copy",
            "name",
            "session",
            "hotkeys",
            "fork",
            "tree",
            "thinking",
            "trust",
            "login",
            "logout",
            "new",
            "compact",
            "resume",
            "reload",
            "quit",
    };
    REQUIRE(commands.size() == expected_names.size());
    for (std::size_t index = 0; index < expected_names.size(); ++index) {
        CHECK(commands[index].name == expected_names[index]);
    }

    // pi-verbatim descriptions and argument hints.
    const auto by_name = [&commands](std::string_view name) -> const coding_agent::prompt::BuiltinSlashCommand* {
        for (const auto& command : commands) {
            if (command.name == name) return &command;
        }
        return nullptr;
    };

    const auto* model = by_name("model");
    REQUIRE(model != nullptr);
    CHECK(model->description == "Select model (opens selector UI)");
    CHECK(model->argument_hint == "<provider/model>");

    const auto* login = by_name("login");
    REQUIRE(login != nullptr);
    CHECK(login->description == "Configure provider authentication");
    CHECK(login->argument_hint == "<provider>");

    const auto* hotkeys = by_name("hotkeys");
    REQUIRE(hotkeys != nullptr);
    CHECK(hotkeys->description == "Show all keyboard shortcuts");

    const auto* fork = by_name("fork");
    REQUIRE(fork != nullptr);
    CHECK(fork->description == "Create a new fork from a previous user message");

    // /reload drops "extensions" (no extensions surface) and /quit uses the
    // C++ binary's own identity (pi `Quit ${APP_NAME}`).
    const auto* reload = by_name("reload");
    REQUIRE(reload != nullptr);
    CHECK(reload->description == "Reload keybindings, skills, prompts, themes, and context files");

    const auto* quit = by_name("quit");
    REQUIRE(quit != nullptr);
    CHECK(quit->description == "Quit pike");
}

TEST_CASE("unported and router-only commands are absent from the autocomplete catalog",
        "[coding_agent][slash-commands][issue419][spec]") {
    const auto& commands = coding_agent::prompt::builtin_slash_commands();
    for (const auto& command : commands) {
        const std::string_view name = command.name;
        CHECK(name != "export");
        CHECK(name != "import");
        CHECK(name != "share");
        // pi added `bug` to its catalog after the 83114817 baseline; it is
        // not-applicable rather than Deferred (it reports to Pi's tracker).
        CHECK(name != "bug");
        CHECK(name != "changelog");
        CHECK(name != "clone");
        CHECK(name != "debug");
        CHECK(name != "help");
        CHECK(name != "commands");
        CHECK(name != "clear");
        CHECK(name != "exit");
        CHECK(name != "arminsayshi");
        CHECK(name != "dementedelves");
    }

    // `/help`, `/commands`, `/clear`, and `/exit` are router-only commands;
    // they are intentionally not part of pi's autocomplete catalog.
}
