#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"
#include "coding_agent/tui/tool_renderers/WriteRenderer.hpp"
#include "support/Json.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// The `write` arguments as one JSON document. Built through the serializer
/// rather than by hand so a content body carrying real newlines still parses:
/// the host's argument parse is exact, and a hand-escaped literal that forgets
/// one escape silently degrades to the empty-argument case.
[[nodiscard]] std::string write_arguments(support::JsonValue content, std::string path = "notes.txt") {
    support::JsonValue arguments{support::JsonValue::object_t{}};
    arguments.get_object().emplace("path", support::JsonValue(std::move(path)));
    arguments.get_object().emplace("content", std::move(content));
    auto json = support::write_json(arguments);
    REQUIRE(json);
    return *json;
}

[[nodiscard]] std::string numbered_content(std::size_t count) {
    std::string content;
    for (std::size_t line = 1; line <= count; ++line) {
        content += std::format("line {}\n", line);
    }
    return content;
}

[[nodiscard]] std::unique_ptr<coding_agent::tui::ToolExecutionComponent> write_component(
        const coding_agent::tui::LiveTheme& theme,
        const std::shared_ptr<const coding_agent::tui::SharedKeybindings>& keybindings,
        std::string arguments,
        std::string output = {},
        bool is_error = false) {
    auto component = std::make_unique<coding_agent::tui::ToolExecutionComponent>(
            theme, keybindings, "write", "call_1", std::move(arguments), "/workspace");
    if (!output.empty() || is_error) {
        component->update_result(ai::ToolResultMessage{
                .tool_call_id = "call_1",
                .tool_name = "write",
                .content = {ai::text_content(std::move(output))},
                .details = std::nullopt,
                .is_error = is_error,
                .timestamp = 0,
        });
    }
    return component;
}

/// How many times `needle` appears across the whole composed block. A
/// `find(...) != npos` check passes whether a body is shown once or twice;
/// this is the count that tells the two apart.
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

TEST_CASE("the default registry resolves the write renderer pair by name",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    auto& renderer = registry.lookup(std::string_view{"write"});
    CHECK(static_cast<bool>(renderer.render_call));
    CHECK(static_cast<bool>(renderer.render_result));
}

TEST_CASE("the write call block previews the content from the arguments, before any result settles",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component =
            write_component(theme, keybindings, write_arguments(support::JsonValue(std::string{"alpha\nbeta\n"})));

    // No result exists yet: the body is on screen because the arguments carry
    // it, not because a tool wrote the file.
    const auto screen = tests::render_tool_screen(*component, 80);

    // pi `write.ts:102-116`: the title, a blank line, then the content lines.
    // The full block, so a title-only implementation — which is exactly what a
    // renderer that took the content off the result would produce here —
    // fails this case and passes nothing that merely looks for the title.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
            "alpha",
            "beta",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("a successful write renders nothing, so the content is on screen exactly once",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // The result text repeats the content verbatim. A renderer that fell
    // through to the generic result path, or that echoed the settled result
    // output under the title, shows `alpha` and `beta` a second time — and
    // every presence check still passes.
    auto component = write_component(
            theme, keybindings, write_arguments(support::JsonValue(std::string{"alpha\nbeta\n"})), "alpha\nbeta");

    const auto screen = tests::render_tool_screen(*component, 80);

    // The occurrence count, not the presence: this is the case that separates
    // "the duplicate is fixed" from "the duplicate is still there".
    CHECK(occurrences(screen, "alpha") == 1);
    CHECK(occurrences(screen, "beta") == 1);
    // And the composed block is the call half alone: the result half added no
    // row at all, not even a blank one.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
            "alpha",
            "beta",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("the write content preview folds at ten lines and names both the hidden count and the total",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(theme, keybindings, write_arguments(support::JsonValue{numbered_content(33)}));

    const auto screen = tests::render_tool_screen(*component, 80);

    // The whole hint row, character for character: pi `write.ts:118` renders
    // `... (23 more lines, 33 total, ctrl+o to expand)`, and the `, 33 total`
    // clause is the only thing that distinguishes write's hint from read's.
    // A `find("more lines")` check passes on a hint with a doubled comma or a
    // dropped total.
    REQUIRE(screen.visible.size() == 15);
    CHECK(screen.visible[13] == "... (23 more lines, 33 total, ctrl+o to expand)");
    // The kept ten are the first ten and the eleventh is absent: a tail fold
    // satisfies the hint row while showing the wrong lines.
    CHECK(screen.visible[3] == "line 1");
    CHECK(screen.visible[12] == "line 10");
    CHECK(std::find(screen.visible.begin(), screen.visible.end(), "line 11") == screen.visible.end());
    // The total counts real lines. The content ends in a newline, and pi drops
    // the trailing empty line before counting, so it reads 33 — a count taken
    // before the trim reports 34 and a presence check cannot see it.
    CHECK(occurrences(screen, "line 33") == 0);
    CHECK(occurrences(screen, "34 total") == 0);
}

TEST_CASE("an expanded write shows every content line and no fold hint",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(theme, keybindings, write_arguments(support::JsonValue{numbered_content(13)}));
    component->set_expanded(true);

    const auto screen = tests::render_tool_screen(*component, 80);

    // The negative twin of the fold case: the same thirteen lines, all of
    // them, and no hint row at all.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
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

TEST_CASE("a write whose content argument is not a string renders pi's invalid-content notice",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(theme, keybindings, write_arguments(support::JsonValue(42.0)));

    const auto screen = tests::render_tool_screen(*component, 80);

    // pi `write.ts:105`, behind the blank line. The negative twin is the fold
    // case: this is the one write body that is not content, so an
    // implementation that folds whatever `content` stringifies to would render
    // a preview here instead.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
            "[invalid content arg - expected string]",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("a write with empty content is the title alone, with no body and no blank line",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(theme, keybindings, write_arguments(support::JsonValue(std::string{})));

    const auto screen = tests::render_tool_screen(*component, 80);

    // pi `write.ts:106`: an empty content string is falsy, so the `else if`
    // never runs and no `\n\n` is appended. A renderer that emits the blank
    // line unconditionally leaves a gap under the title of every empty file.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
    };
    CHECK(screen.visible == expected);
}

TEST_CASE("a failed write renders the result text in the error colour, behind a blank line",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(theme,
            keybindings,
            write_arguments(support::JsonValue(std::string{"alpha\n"})),
            "EACCES: permission denied",
            true);

    const auto screen = tests::render_tool_screen(*component, 80);

    // The full block: the content preview is still there and the error sits
    // under it, so an implementation that suppresses the result half
    // regardless of `is_error` fails this case and passes the success one.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
            "alpha",
            "",
            "EACCES: permission denied",
            "",
    };
    CHECK(screen.visible == expected);
    // The error row carries the `error` token, not `toolOutput`: a
    // visible-text check cannot tell a coloured error from plain content.
    const auto styled = theme.foreground(coding_agent::tui::ThemeToken::Error, "MARKER");
    const auto marker = styled.substr(0, styled.find("MARKER"));
    const auto row = std::find_if(screen.raw.begin(), screen.raw.end(), [](const auto& line) {
        return line.find("EACCES: permission denied") != std::string::npos;
    });
    REQUIRE(row != screen.raw.end());
    CHECK(row->find(marker) != std::string::npos);
}

TEST_CASE("a failed write with no result text renders nothing at all",
        "[coding_agent][tui][tool-renderers][issue827][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = write_component(
            theme, keybindings, write_arguments(support::JsonValue(std::string{"alpha\n"})), "", /*is_error=*/true);

    const auto screen = tests::render_tool_screen(*component, 80);

    // pi `write.ts:136`: the second of the two exits that render nothing. A
    // branch entered on `is_error` alone leaves an empty coloured row here,
    // which no assertion about the success case would notice.
    const std::vector<std::string> expected{
            "",
            "write notes.txt",
            "",
            "alpha",
            "",
    };
    CHECK(screen.visible == expected);
}
