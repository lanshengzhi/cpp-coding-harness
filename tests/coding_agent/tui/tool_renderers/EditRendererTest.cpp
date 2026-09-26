#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "coding_agent/tui/tool_renderers/EditRenderer.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"
#include "support/Json.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// pi's `edit` arguments: the two-block form the E2E script drives.
constexpr std::string_view kEditArguments = R"({"path":"notes.txt","edits":[{"oldText":"alpha","newText":"beta"}]})";

/// The display diff `AsyncToolFactories.cpp:452` writes into `details.diff`.
[[nodiscard]] std::optional<support::JsonValue> diff_details(std::string diff) {
    support::JsonValue details{support::JsonValue::object_t{}};
    details.get_object().emplace("diff", support::JsonValue(std::move(diff)));
    return details;
}

[[nodiscard]] std::unique_ptr<coding_agent::tui::ToolExecutionComponent> edit_component(
        const coding_agent::tui::LiveTheme& theme,
        const std::shared_ptr<const coding_agent::tui::SharedKeybindings>& keybindings,
        std::string output = {},
        std::optional<support::JsonValue> details = std::nullopt,
        bool is_error = false) {
    auto component = std::make_unique<coding_agent::tui::ToolExecutionComponent>(
            theme, keybindings, "edit", "call_1", std::string{kEditArguments}, "/workspace");
    if (!output.empty() || details.has_value() || is_error) {
        component->update_result(ai::ToolResultMessage{
                .tool_call_id = "call_1",
                .tool_name = "edit",
                .content = {ai::text_content(std::move(output))},
                .details = std::move(details),
                .is_error = is_error,
                .timestamp = 0,
        });
    }
    return component;
}

/// The escape sequence a theme token contributes, without the text it wraps.
[[nodiscard]] std::string token_marker(
        const coding_agent::tui::LiveTheme& theme, coding_agent::tui::ThemeToken token, bool background) {
    const auto styled = background ? theme.background(token, "MARKER") : theme.foreground(token, "MARKER");
    return styled.substr(0, styled.find("MARKER"));
}

/// The raw (styled) row carrying `needle`, or an empty string when none does.
[[nodiscard]] std::string row_with(const tests::ToolRenderScreen& screen, std::string_view needle) {
    const auto found = std::find_if(screen.raw.begin(), screen.raw.end(), [&](const auto& line) {
        return line.find(needle) != std::string::npos;
    });
    return found == screen.raw.end() ? std::string{} : *found;
}

/// How many times `needle` appears across the whole composed block.
[[nodiscard]] std::size_t occurrences(const tests::ToolRenderScreen& screen, std::string_view needle) {
    std::string composed;
    for (const auto& row : screen.visible) {
        composed += row;
        composed.push_back('\n');
    }
    std::size_t count = 0;
    for (std::size_t at = composed.find(needle); at != std::string::npos; at = composed.find(needle, at + 1))
        ++count;
    return count;
}

} // namespace

TEST_CASE("the default registry resolves the edit renderer pair by name",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    auto& renderer = registry.lookup(std::string_view{"edit"});
    CHECK(static_cast<bool>(renderer.render_call));
    CHECK(static_cast<bool>(renderer.render_result));
}

TEST_CASE("the edit call block is the title alone, with no argument JSON on screen",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(theme, keybindings);

    // pi `edit.ts:83-86` returns the header and nothing else. The argument dump
    // the generic fallback drew is the thing this call half replaced, and it is
    // the string a `find("edit notes.txt")` check would sail straight past.
    const auto screen = tests::render_tool_screen(*component, 80);
    const std::vector<std::string> expected{
            "",
            "edit notes.txt",
            "",
    };
    CHECK(screen.visible == expected);
    for (const auto& row : screen.visible) {
        CHECK(row.find('{') == std::string::npos);
        CHECK(row.find("oldText") == std::string::npos);
    }
}

TEST_CASE("a successful edit still renders details.diff through the diff renderer",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(
            theme, keybindings, "Successfully replaced 1 block(s) in notes.txt.", diff_details("-1 alpha\n+1 beta"));

    const auto screen = tests::render_tool_screen(*component, 80);

    // The preservation requirement, asserted as the full block rather than as
    // `find("-1 alpha")`: the rows, their order, and the fact that the
    // success text never reaches the screen.
    const std::vector<std::string> expected{
            "",
            "edit notes.txt",
            "-1 alpha",
            "+1 beta",
            "",
    };
    CHECK(screen.visible == expected);
    CHECK(occurrences(screen, "Successfully replaced") == 0);
}

TEST_CASE("the edit diff rows are styled with the diff tokens, not with the error token",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(theme, keybindings, "ok", diff_details("-1 alpha\n+1 beta\nsecond line\n"));

    const auto screen = tests::render_tool_screen(*component, 80);

    // A golden byte-comparison cannot express this: a diff rendered in the
    // wrong colour yields the same visible rows. The `error` token is checked
    // too, because a result half that styled its whole body as an error would
    // still pass every visible-text assertion in the case above. The rows are
    // located by their word, not by `-1 alpha`: the word-level highlight wraps
    // the changed token in its own escape, so the prefixed form is not
    // contiguous in the raw output.
    const auto removed = row_with(screen, "alpha");
    const auto added = row_with(screen, "beta");
    const auto removed_token = token_marker(theme, coding_agent::tui::ThemeToken::ToolDiffRemoved, false);
    const auto added_token = token_marker(theme, coding_agent::tui::ThemeToken::ToolDiffAdded, false);
    REQUIRE(!removed.empty());
    REQUIRE(!added.empty());
    CHECK(removed.find(removed_token) != std::string::npos);
    CHECK(added.find(added_token) != std::string::npos);
    // Each row carries its own token and not the other's: one colour for the
    // whole diff passes the two checks above. Neither row carries the
    // `toolOutput` grey, which is the colour the generic result path uses —
    // that separates "went through render_diff" from "fell through to the
    // fallback". The `error` token is deliberately not asserted against
    // `toolDiffRemoved`: both resolve to the theme's `red`, so no screen can
    // tell them apart and asserting it would encode a difference that does
    // not exist.
    CHECK(removed.find(added_token) == std::string::npos);
    CHECK(added.find(removed_token) == std::string::npos);
    const auto output_token = token_marker(theme, coding_agent::tui::ThemeToken::ToolOutput, false);
    CHECK(removed.find(output_token) == std::string::npos);
    CHECK(added.find(output_token) == std::string::npos);
    // The error branch really is reachable in the builtin dark theme, so the
    // error case below is a live assertion and not a colour that never occurs.
    CHECK(token_marker(theme, coding_agent::tui::ThemeToken::Error, false) == removed_token);
}

TEST_CASE("a successful edit with no details renders the title alone",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(theme, keybindings, "nothing to show");

    // The negative twin of the diff case: pi `edit.ts:106` returns nothing
    // when `result.details?.diff` is absent, so the settled success text must
    // not leak into the block either.
    const auto screen = tests::render_tool_screen(*component, 80);
    const std::vector<std::string> expected{
            "",
            "edit notes.txt",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("a failed edit renders the result text in the error colour and no diff",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // The error branch wins over `details.diff`, which the harness does carry
    // on a failed edit: a result half that checked the diff first renders the
    // diff here and passes every success case.
    auto component = edit_component(
            theme, keybindings, "oldText not found in notes.txt.", diff_details("-1 alpha\n+1 beta"), true);

    const auto screen = tests::render_tool_screen(*component, 80);

    const std::vector<std::string> expected{
            "",
            "edit notes.txt",
            "oldText not found in notes.txt.",
            "",
    };
    CHECK(screen.visible == expected);
    CHECK(occurrences(screen, "-1 alpha") == 0);
    const auto row = row_with(screen, "oldText not found");
    REQUIRE(!row.empty());
    CHECK(row.find(token_marker(theme, coding_agent::tui::ThemeToken::Error, false)) != std::string::npos);
}

TEST_CASE("a failed edit with no result text renders nothing at all",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(theme, keybindings, "", std::nullopt, /*is_error=*/true);

    // pi `edit.ts:93`. An error branch entered on `is_error` alone leaves an
    // empty coloured row here.
    const auto screen = tests::render_tool_screen(*component, 80);
    const std::vector<std::string> expected{
            "",
            "edit notes.txt",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("the edit block background follows the execution state",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = edit_component(theme, keybindings);

    // The seam retains pi's state-driven background, and a visible-text
    // assertion cannot see it: all three states render `edit notes.txt`.
    const auto pending = tests::render_tool_screen(*component, 80);
    CHECK(row_with(pending, "notes.txt")
                    .find(token_marker(theme, coding_agent::tui::ThemeToken::ToolPendingBg, true)) !=
            std::string::npos);

    component->update_result(ai::ToolResultMessage{
            .tool_call_id = "call_1",
            .tool_name = "edit",
            .content = {ai::text_content("ok")},
            .details = diff_details("-1 alpha\n+1 beta"),
            .is_error = false,
            .timestamp = 0,
    });
    const auto succeeded = tests::render_tool_screen(*component, 80);
    const auto success_row = row_with(succeeded, "notes.txt");
    REQUIRE(!success_row.empty());
    CHECK(success_row.find(token_marker(theme, coding_agent::tui::ThemeToken::ToolSuccessBg, true)) !=
            std::string::npos);
    CHECK(success_row.find(token_marker(theme, coding_agent::tui::ThemeToken::ToolPendingBg, true)) ==
            std::string::npos);

    component->update_result(ai::ToolResultMessage{
            .tool_call_id = "call_1",
            .tool_name = "edit",
            .content = {ai::text_content("oldText not found")},
            .details = std::nullopt,
            .is_error = true,
            .timestamp = 0,
    });
    const auto failed = tests::render_tool_screen(*component, 80);
    const auto error_row = row_with(failed, "notes.txt");
    REQUIRE(!error_row.empty());
    CHECK(error_row.find(token_marker(theme, coding_agent::tui::ThemeToken::ToolErrorBg, true)) != std::string::npos);
    CHECK(error_row.find(token_marker(theme, coding_agent::tui::ThemeToken::ToolSuccessBg, true)) == std::string::npos);
}
