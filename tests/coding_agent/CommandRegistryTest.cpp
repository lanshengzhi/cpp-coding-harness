#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/CommandRegistry.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace cch;

namespace {

coding_agent::CommandHandler handler_returning(std::string text = "handled") {
    return [text = std::move(text)](const coding_agent::CommandContext&, std::string_view) {
        return coding_agent::CommandResult{text};
    };
}

} // namespace

TEST_CASE("command registry stores aggregate-friendly canonical metadata", "[coding_agent][command_registry]") {
    static_assert(std::is_aggregate_v<coding_agent::CommandInfo>);

    coding_agent::CommandRegistry registry;
    auto registered = registry.register_command(
        "resume",
        "Show restart instructions for resuming a session",
        "<session-id>",
        handler_returning());

    REQUIRE(registered.has_value());
    const auto info = registry.find_command_info("resume");
    REQUIRE(info.has_value());
    CHECK(info->name == "resume");
    CHECK(info->description == "Show restart instructions for resuming a session");
    CHECK(info->argument_hint == "<session-id>");
    CHECK_FALSE(info->alias_for.has_value());
}

TEST_CASE("command registry convenience registration supplies metadata defaults", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    auto registered = registry.register_command("plain", handler_returning());

    REQUIRE(registered.has_value());
    const auto info = registry.find_command_info("plain");
    REQUIRE(info.has_value());
    CHECK(info->description.empty());
    CHECK(info->argument_hint.empty());
    CHECK_FALSE(info->alias_for.has_value());
}

TEST_CASE("command registry lists canonical commands in deterministic name order", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("zeta", handler_returning()).has_value());
    REQUIRE(registry.register_command("alpha", handler_returning()).has_value());
    REQUIRE(registry.register_command("middle", handler_returning()).has_value());

    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 3);
    CHECK(commands[0].name == "alpha");
    CHECK(commands[1].name == "middle");
    CHECK(commands[2].name == "zeta");
}

TEST_CASE("command registry rejects invalid canonical names without mutation", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("valid", handler_returning()).has_value());

    const std::vector<std::string> invalid_names{"", "/help", "two words", "tab\tname", "line\nname"};
    for (const auto& name : invalid_names) {
        auto registered = registry.register_command(name, handler_returning());
        REQUIRE_FALSE(registered.has_value());
        CHECK(registered.error().code == util::ErrorCode::Validation);
        const auto commands = registry.list_commands();
        REQUIRE(commands.size() == 1);
        CHECK(commands.front().name == "valid");
        CHECK_FALSE(registry.find_command_info(name).has_value());
    }
}

TEST_CASE("command registry rejects an empty handler without mutation", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("valid", handler_returning()).has_value());

    auto registered = registry.register_command("empty", coding_agent::CommandHandler{});

    REQUIRE_FALSE(registered.has_value());
    CHECK(registered.error().code == util::ErrorCode::Validation);
    CHECK(registry.list_commands().size() == 1);
    CHECK_FALSE(registry.find_command_info("empty").has_value());
}

TEST_CASE("command registry rejects duplicate canonical names and preserves original dispatch", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("same", "original", {}, handler_returning("first")).has_value());

    auto duplicate = registry.register_command("same", "replacement", "<arg>", handler_returning("second"));

    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == util::ErrorCode::Validation);
    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 1);
    CHECK(commands.front().description == "original");
    CHECK(commands.front().argument_hint.empty());

    const auto dispatched = registry.dispatch("same", {}, {});
    REQUIRE(dispatched.has_value());
    CHECK(dispatched->display_text == "first");
}

TEST_CASE("command registry preserves move-only canonical handlers", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    auto state = std::make_unique<std::string>("move-only");

    auto registered = registry.register_command(
        "owned",
        [state = std::move(state)](const coding_agent::CommandContext&, std::string_view) {
            return coding_agent::CommandResult{*state};
        });

    REQUIRE(registered.has_value());
    const auto dispatched = registry.dispatch("owned", {}, {});
    REQUIRE(dispatched.has_value());
    CHECK(dispatched->display_text == "move-only");
}

TEST_CASE("command registry aliases dispatch through canonical move-only handlers", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    auto state = std::make_unique<std::string>("aliased move-only");
    REQUIRE(registry.register_command(
        "help",
        "Show command help",
        "[command]",
        [state = std::move(state)](const coding_agent::CommandContext&, std::string_view args) {
            return coding_agent::CommandResult{*state + ":" + std::string{args}};
        }).has_value());

    auto registered = registry.register_alias("commands", "help");

    REQUIRE(registered.has_value());
    const auto dispatched = registry.dispatch("commands", {}, "quit");
    REQUIRE(dispatched.has_value());
    CHECK(dispatched->display_text == "aliased move-only:quit");

    const auto info = registry.find_command_info("commands");
    REQUIRE(info.has_value());
    CHECK(info->name == "commands");
    CHECK(info->description == "Show command help");
    CHECK(info->argument_hint == "[command]");
    REQUIRE(info->alias_for.has_value());
    CHECK(*info->alias_for == "help");
}

TEST_CASE("command registry rejects invalid alias names without mutation", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("help", handler_returning()).has_value());

    const std::vector<std::string> invalid_names{"", "/commands", "two words", "tab\tname", "line\nname"};
    for (const auto& name : invalid_names) {
        auto registered = registry.register_alias(name, "help");
        REQUIRE_FALSE(registered.has_value());
        CHECK(registered.error().code == util::ErrorCode::Validation);
        CHECK(registry.list_commands().size() == 1);
        CHECK_FALSE(registry.find_command_info(name).has_value());
        CHECK_FALSE(registry.dispatch(name, {}, {}).has_value());
    }
}

TEST_CASE("command registry rejects canonical and alias name collisions without mutation", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("quit", handler_returning("canonical")).has_value());
    REQUIRE(registry.register_alias("exit", "quit").has_value());

    auto alias_over_canonical = registry.register_alias("quit", "quit");
    REQUIRE_FALSE(alias_over_canonical.has_value());
    CHECK(alias_over_canonical.error().code == util::ErrorCode::Validation);

    auto alias_over_alias = registry.register_alias("exit", "quit");
    REQUIRE_FALSE(alias_over_alias.has_value());
    CHECK(alias_over_alias.error().code == util::ErrorCode::Validation);

    auto canonical_over_alias = registry.register_command("exit", handler_returning("replacement"));
    REQUIRE_FALSE(canonical_over_alias.has_value());
    CHECK(canonical_over_alias.error().code == util::ErrorCode::Validation);

    const auto info = registry.find_command_info("exit");
    REQUIRE(info.has_value());
    REQUIRE(info->alias_for.has_value());
    CHECK(*info->alias_for == "quit");
    const auto dispatched = registry.dispatch("exit", {}, {});
    REQUIRE(dispatched.has_value());
    CHECK(dispatched->display_text == "canonical");
}

TEST_CASE("command registry rejects missing and alias targets without partial mutation", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("help", handler_returning()).has_value());
    REQUIRE(registry.register_alias("commands", "help").has_value());

    auto missing_target = registry.register_alias("unknown", "missing");
    REQUIRE_FALSE(missing_target.has_value());
    CHECK(missing_target.error().code == util::ErrorCode::Validation);
    CHECK_FALSE(registry.find_command_info("unknown").has_value());

    auto alias_target = registry.register_alias("shortcut", "commands");
    REQUIRE_FALSE(alias_target.has_value());
    CHECK(alias_target.error().code == util::ErrorCode::Validation);
    CHECK_FALSE(registry.find_command_info("shortcut").has_value());

    const auto original_alias = registry.find_command_info("commands");
    REQUIRE(original_alias.has_value());
    REQUIRE(original_alias->alias_for.has_value());
    CHECK(*original_alias->alias_for == "help");

    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].name == "commands");
    CHECK(commands[1].name == "help");
}

TEST_CASE("command registry lists canonical commands and aliases in deterministic name order", "[coding_agent][command_registry]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(registry.register_command("zeta", "Zeta command", "<z>", handler_returning()).has_value());
    REQUIRE(registry.register_command("alpha", "Alpha command", {}, handler_returning()).has_value());
    REQUIRE(registry.register_command("middle", handler_returning()).has_value());
    REQUIRE(registry.register_alias("commands", "zeta").has_value());
    REQUIRE(registry.register_alias("exit", "alpha").has_value());

    const auto commands = registry.list_commands();

    REQUIRE(commands.size() == 5);
    CHECK(commands[0].name == "alpha");
    CHECK_FALSE(commands[0].alias_for.has_value());
    CHECK(commands[1].name == "commands");
    CHECK(commands[1].description == "Zeta command");
    CHECK(commands[1].argument_hint == "<z>");
    REQUIRE(commands[1].alias_for.has_value());
    CHECK(*commands[1].alias_for == "zeta");
    CHECK(commands[2].name == "exit");
    CHECK(commands[2].description == "Alpha command");
    REQUIRE(commands[2].alias_for.has_value());
    CHECK(*commands[2].alias_for == "alpha");
    CHECK(commands[3].name == "middle");
    CHECK(commands[4].name == "zeta");
}
