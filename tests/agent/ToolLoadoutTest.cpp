// #1110 / audit-1047 "replacing state.tools narrows the executable set and
// declares the system tool delta": the transcript declares the tool loadout,
// and each request declares the delta between that declaration and the
// executable set (pi `getCurrentTools`, `getToolStateChanges`,
// `declareToolChanges`). The cases below name the observations the Agent-level
// caller cannot separate cheaply: the ordered content of the delta, the
// removal-plus-addition a changed definition produces, and the position the
// declaration takes in the pending message list.

#include <cch/agent/ToolLoadout.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/Tool.hpp>

#include "support/ToolArgumentContracts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] ai::Tool make_tool(std::string name, std::string description = "A tool") {
    return ai::Tool{
            .name = std::move(name),
            .description = std::move(description),
            .parameters = test::permissive_object_tool_argument_contract(),
    };
}

[[nodiscard]] ai::MessageVariant system_with(std::vector<ai::Tool> added, std::vector<std::string> removed = {}) {
    ai::SystemMessage message;
    message.tools_added = std::move(added);
    for (auto& name : removed) {
        message.tools_removed.push_back(ai::ToolReference{.name = std::move(name)});
    }
    return ai::MessageVariant{std::move(message)};
}

[[nodiscard]] std::vector<std::string> names_of(const std::vector<ai::Tool>& tools) {
    std::vector<std::string> names;
    names.reserve(tools.size());
    for (const auto& tool : tools) {
        names.push_back(tool.name);
    }
    return names;
}

[[nodiscard]] std::vector<std::string> removed_names(const std::vector<ai::ToolReference>& removed) {
    std::vector<std::string> names;
    names.reserve(removed.size());
    for (const auto& reference : removed) {
        names.push_back(reference.name);
    }
    return names;
}

} // namespace

TEST_CASE(
        "current_tools replays removals and additions in transcript order", "[agent][tool-loadout][issue1110][spec]") {
    // pi `getCurrentTools`: removals apply before additions inside one system
    // message, and a redeclared name keeps the position of its first
    // declaration rather than moving to the end.
    const std::vector<ai::MessageVariant> messages{
            system_with({make_tool("a"), make_tool("b")}),
            ai::MessageVariant{ai::user_text_message("go")},
            system_with({make_tool("c")}, {"a"}),
            system_with({make_tool("b", "A redefined tool")}),
    };

    const auto tools = agent::current_tools(messages);
    CHECK(names_of(tools) == std::vector<std::string>{"b", "c"});
    CHECK(tools[0].description == "A redefined tool");
}

TEST_CASE("tool_state_changes reports additions in current order and removals in previous order",
        "[agent][tool-loadout][issue1110][spec]") {
    const std::vector<ai::Tool> previous{make_tool("a"), make_tool("b"), make_tool("c")};
    const std::vector<ai::Tool> current{make_tool("d"), make_tool("c")};

    const auto changes = agent::tool_state_changes(previous, current);
    CHECK(names_of(changes.tools_added) == std::vector<std::string>{"d"});
    CHECK(removed_names(changes.tools_removed) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("tool_state_changes treats a changed definition as a removal and an addition",
        "[agent][tool-loadout][issue1110][spec]") {
    const std::vector<ai::Tool> previous{make_tool("a", "old description"), make_tool("b")};
    const std::vector<ai::Tool> current{make_tool("a", "new description"), make_tool("b")};

    const auto changes = agent::tool_state_changes(previous, current);
    REQUIRE(changes.tools_added.size() == 1);
    CHECK(changes.tools_added[0].name == "a");
    CHECK(changes.tools_added[0].description == "new description");
    CHECK(removed_names(changes.tools_removed) == std::vector<std::string>{"a"});
}

TEST_CASE(
        "tool_state_changes ignores reordering and executable-only fields", "[agent][tool-loadout][issue1110][spec]") {
    // A name-set-equal reorder is not a declaration change, and `outputSchema`
    // is executable metadata: pi's `toToolDeclaration` drops it, so it must not
    // produce a spurious delta.
    auto with_output_schema = make_tool("a");
    with_output_schema.output_schema = support::JsonValue{support::JsonValue::object_t{{"type", "object"}}};
    const std::vector<ai::Tool> previous{make_tool("a"), make_tool("b")};
    const std::vector<ai::Tool> current{make_tool("b"), with_output_schema};

    CHECK(agent::tool_state_changes(previous, current).empty());
    CHECK_FALSE(agent::declarations_equal(make_tool("a"), make_tool("a", "different")));
}

TEST_CASE("current_system_message replays prompt text, section patches and tool declarations",
        "[agent][tool-loadout][issue1108][spec]") {
    // pi `getCurrentSystemMessage` (`packages/ai/src/utils/transcript.ts`):
    // every System Message folds into one baseline. Text joins in order, a
    // named section keeps its first position while later values replace it, an
    // explicit removal drops it, and the tool fields are the replayed current
    // declarations. The first System Message's timestamp wins.
    ai::SystemMessage first;
    first.content = "base";
    first.timestamp = 5;
    first.tools_added.push_back(make_tool("a"));
    ai::SystemMessage second;
    second.content = "next";
    second.timestamp = 9;
    second.sections.push_back(ai::SystemMessageSection{.name = "one", .text = "first value"});
    second.sections.push_back(ai::SystemMessageSection{.name = "two", .text = "second value"});
    second.tools_removed.push_back(ai::ToolReference{.name = "a"});
    second.tools_added.push_back(make_tool("b"));
    ai::SystemMessage third;
    third.timestamp = 11;
    third.sections.push_back(ai::SystemMessageSection{.name = "one", .text = std::nullopt});
    third.sections.push_back(ai::SystemMessageSection{.name = "two", .text = "patched"});
    third.sections.push_back(ai::SystemMessageSection{.name = "three", .text = "added"});
    const std::vector<ai::MessageVariant> messages{
            ai::MessageVariant{first},
            ai::MessageVariant{ai::user_text_message("go")},
            ai::MessageVariant{second},
            ai::MessageVariant{third},
    };

    const auto baseline = agent::current_system_message(messages);
    REQUIRE(baseline.has_value());
    CHECK(baseline->content == "base\n\nnext");
    CHECK(baseline->timestamp == 5);
    REQUIRE(baseline->sections.size() == 2);
    CHECK(baseline->sections[0].name == "two");
    CHECK(baseline->sections[0].text == std::optional<std::string>{"patched"});
    CHECK(baseline->sections[1].name == "three");
    CHECK(baseline->sections[1].text == std::optional<std::string>{"added"});
    CHECK(names_of(baseline->tools_added) == std::vector<std::string>{"b"});
    CHECK(baseline->tools_removed.empty());

    // A transcript without a System Message has no baseline, and an empty
    // transcript likewise (pi: `timestamp === undefined && tools.length === 0`).
    CHECK_FALSE(agent::current_system_message({}).has_value());
    CHECK_FALSE(agent::current_system_message({ai::MessageVariant{ai::user_text_message("go")}}).has_value());
}

TEST_CASE("declare_tool_changes inserts the delta before the first non-system message",
        "[agent][tool-loadout][issue1110][spec]") {
    const std::vector<ai::MessageVariant> transcript{system_with({make_tool("a"), make_tool("b")})};
    std::vector<ai::MessageVariant> pending{ai::MessageVariant{ai::user_text_message("go")}};

    const auto declared = agent::declare_tool_changes(transcript, std::move(pending), {make_tool("b")});

    REQUIRE(declared.size() == 2);
    const auto* system = std::get_if<ai::SystemMessage>(&declared[0]);
    REQUIRE(system != nullptr);
    CHECK(names_of(system->tools_added).empty());
    CHECK(removed_names(system->tools_removed) == std::vector<std::string>{"a"});
    CHECK(std::holds_alternative<ai::UserMessage>(declared[1]));
}

TEST_CASE("declare_tool_changes leaves the pending list alone when nothing changed",
        "[agent][tool-loadout][issue1110][spec]") {
    const std::vector<ai::MessageVariant> transcript{system_with({make_tool("a")})};
    std::vector<ai::MessageVariant> pending{ai::MessageVariant{ai::user_text_message("go")}};

    const auto declared = agent::declare_tool_changes(transcript, std::move(pending), {make_tool("a")});

    REQUIRE(declared.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(declared[0]));
}

TEST_CASE("declare_tool_changes patches a pending system message instead of adding another",
        "[agent][tool-loadout][issue1110][spec]") {
    // The session hands the loop a prompt-section system message; pi treats its
    // tool fields as the delta's anchor and replaces them, so one message
    // carries both the section patch and the tool delta.
    const std::vector<ai::MessageVariant> transcript{system_with({make_tool("a"), make_tool("b")})};
    ai::SystemMessage section_patch;
    section_patch.sections.push_back(ai::SystemMessageSection{.name = "tools", .text = "patched"});
    std::vector<ai::MessageVariant> pending{ai::MessageVariant{std::move(section_patch)}};

    const auto declared = agent::declare_tool_changes(transcript, std::move(pending), {make_tool("b")});

    REQUIRE(declared.size() == 1);
    const auto* system = std::get_if<ai::SystemMessage>(&declared[0]);
    REQUIRE(system != nullptr);
    REQUIRE(system->sections.size() == 1);
    CHECK(system->sections[0].name == "tools");
    CHECK(system->sections[0].text == std::optional<std::string>{"patched"});
    CHECK(removed_names(system->tools_removed) == std::vector<std::string>{"a"});
}
