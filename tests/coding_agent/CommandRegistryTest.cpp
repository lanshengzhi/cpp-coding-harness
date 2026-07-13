#include "../../third_party/catch2/catch_test_macros.hpp"

#include <cch/coding_agent/CommandRegistry.hpp>

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
