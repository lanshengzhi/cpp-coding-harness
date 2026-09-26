#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string numbered_output(std::size_t count) {
    std::string output;
    for (std::size_t line = 1; line <= count; ++line) {
        if (!output.empty()) output.push_back('\n');
        output += std::format("line {}", line);
    }
    return output;
}

[[nodiscard]] ai::ToolResultMessage text_result(std::string output) {
    return ai::ToolResultMessage{
            .tool_call_id = "call_1",
            .tool_name = "custom_tool",
            .content = {ai::text_content(std::move(output))},
            .details = std::nullopt,
            .is_error = false,
            .timestamp = 0,
    };
}

} // namespace

TEST_CASE("a tool with no registered renderer falls back to pi's bold name, blank line, and 2-space-indented argument "
          "JSON",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", R"({"alpha":"beta","gamma":"delta"})", "/workspace");

    const auto screen = tests::render_tool_screen(component, 80);

    // The full composed block, not a substring: a compact one-line JSON, a
    // 4-space indent, or a missing blank row all satisfy a `find("alpha")`
    // check while breaking pi's framing.
    const std::vector<std::string> expected{
            "",
            "custom_tool",
            "",
            "{",
            "  \"alpha\": \"beta\",",
            "  \"gamma\": \"delta\"",
            "}",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("the fallback folds its output at ten lines with pi's remaining-lines hint",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", "{}", "/workspace");
    component.update_result(text_result(numbered_output(13)));

    const auto screen = tests::render_tool_screen(component, 80);

    const std::vector<std::string> expected{
            "",
            "custom_tool",
            "",
            "{}",
            "line 1",
            "line 2",
            "line 3",
            "line 4",
            "line 5",
            "line 6",
            "line 7",
            "line 8",
            "line 9",
            "line 10",
            "... (3 more lines, ctrl+o to expand)",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("the fallback fold takes the head of the output, not its tail",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", "{}", "/workspace");
    component.update_result(text_result(numbered_output(13)));

    const auto screen = tests::render_tool_screen(component, 80);

    // The folded rows are the first ten: a tail fold would satisfy a
    // "3 more lines" hint check while showing the wrong ten lines.
    REQUIRE(screen.visible.size() == 16);
    CHECK(screen.visible[4] == "line 1");
    CHECK(screen.visible[13] == "line 10");
    CHECK(std::find(screen.visible.begin(), screen.visible.end(), "line 11") == screen.visible.end());
}

TEST_CASE("the expanded fallback shows every output line and no fold hint",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", "{}", "/workspace");
    component.update_result(text_result(numbered_output(13)));
    component.set_expanded(true);

    const auto screen = tests::render_tool_screen(component, 80);

    const std::vector<std::string> expected{
            "",
            "custom_tool",
            "",
            "{}",
            "line 1",
            "line 2",
            "line 3",
            "line 4",
            "line 5",
            "line 6",
            "line 7",
            "line 8",
            "line 9",
            "line 10",
            "line 11",
            "line 12",
            "line 13",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("the fallback reads Unbound in the fold hint when the expand key is unbound",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings(/*bound=*/false);
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", "{}", "/workspace");
    component.update_result(text_result(numbered_output(13)));

    const auto screen = tests::render_tool_screen(component, 80);

    // The whole hint, not "Unbound": pi's punctuation is `... (N more lines,
    // <key> to expand)` with three ASCII dots, a comma, and one space.
    REQUIRE(screen.visible.size() == 16);
    CHECK(screen.visible[14] == "... (3 more lines, Unbound to expand)");
}

TEST_CASE("the fallback hint takes the bound key text, not the literal Unbound",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    REQUIRE(keybindings->registry().key_text("app.tools.expand") == "ctrl+o");
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", "{}", "/workspace");
    component.update_result(text_result(numbered_output(13)));

    const auto screen = tests::render_tool_screen(component, 80);

    REQUIRE(screen.visible.size() == 16);
    CHECK(screen.visible[14] == "... (3 more lines, ctrl+o to expand)");
}
