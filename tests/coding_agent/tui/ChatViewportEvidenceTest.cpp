// Named pi-v1.0.4 acceptance for the product two-region fullscreen viewport
// (#1036, spec #946). Every frame, cell style, cursor, transcript state and alt
// screen viewport reading comes from the verified `chat-viewport.json` artifact,
// which was captured on a clean frozen checkout at
// `7c10bd4337495ee613f2224843ecdf349b80d1df`; nothing is re-derived here.

#include "coding_agent/tui/ChatViewport.hpp"

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using cch::support::JsonValue;

constexpr std::string_view kFrozenRevision{"7c10bd4337495ee613f2224843ecdf349b80d1df"};

[[nodiscard]] std::size_t size_of(const JsonValue& value) { return static_cast<std::size_t>(value.get_number()); }

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.push_back(entry.get_string());
    return strings;
}

[[nodiscard]] std::string joined(const JsonValue& lines) {
    std::string text;
    for (const auto& line : lines.get_array()) {
        if (!text.empty()) text.push_back('\n');
        text += line.get_string();
    }
    return text;
}

/// pi's `visibleLines` compare: trailing padding is presentation, not content.
[[nodiscard]] std::vector<std::string> visible_rows(std::vector<std::string> rows) {
    for (auto& row : rows) {
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
    }
    return rows;
}

[[nodiscard]] std::string scrollbar_name(cch::tui::ScrollbarMode mode) {
    switch (mode) {
    case cch::tui::ScrollbarMode::Hidden:
        return "hidden";
    case cch::tui::ScrollbarMode::Auto:
        return "auto";
    case cch::tui::ScrollbarMode::Always:
        return "always";
    }
    return "unknown";
}

[[nodiscard]] cch::tui::ScrollbarMode scrollbar_mode(std::string_view name) {
    if (name == "hidden") return cch::tui::ScrollbarMode::Hidden;
    if (name == "always") return cch::tui::ScrollbarMode::Always;
    return cch::tui::ScrollbarMode::Auto;
}

// xterm `getChars()` represents vacant cells as an empty string; C++ pads the
// same cells with U+0020. Both display one blank cell, and every style and
// continuation bit stays in the comparison so a styled blank cannot hide here.
[[nodiscard]] cch::tui::VirtualTerminalCell evidence_cell(cch::tui::VirtualTerminalCell cell) {
    if (cell.grapheme.empty() && !cell.continuation) cell.grapheme = " ";
    return cell;
}

void check_cells(const cch::tui::VirtualTerminal& terminal, const JsonValue& expected) {
    const auto& rows = expected.get_array();
    REQUIRE(terminal.cells().size() == rows.size());
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto& cells = rows[row].get_array();
        REQUIRE(terminal.cells()[row].size() == cells.size());
        for (std::size_t column = 0; column < cells.size(); ++column) {
            INFO(std::format("cell row={}, column={}", row, column));
            const auto& cell = cells[column];
            const auto& style = cell.at("style");
            const cch::tui::VirtualTerminalCell expected_cell{
                    .grapheme = cell.at("grapheme").get_string(),
                    .continuation = cell.at("continuation").get_boolean(),
                    .style =
                            {
                                    .bold = style.at("bold").get_boolean(),
                                    .dim = style.at("dim").get_boolean(),
                                    .italic = style.at("italic").get_boolean(),
                                    .underline = style.at("underline").get_boolean(),
                                    .blink = style.at("blink").get_boolean(),
                                    .inverse = style.at("inverse").get_boolean(),
                                    .hidden = style.at("hidden").get_boolean(),
                                    .strikethrough = style.at("strikethrough").get_boolean(),
                                    .fg_color = style.at("fg_color").get_string(),
                                    .bg_color = style.at("bg_color").get_string(),
                                    .hyperlink = style.at("hyperlink").get_string(),
                                    .hyperlink_params = style.at("hyperlink_params").get_string(),
                            },
            };
            CHECK(evidence_cell(terminal.cells()[row][column]) == evidence_cell(expected_cell));
        }
    }
}

/// Build the viewport from the recorded inputs: the same plain `Text` regions
/// the frozen capture drives. The document and editor addresses stay live
/// because the recorded steps replace their text.
struct BuiltViewport {
    ChatViewport viewport;
    cch::tui::Text* document{nullptr};
    cch::tui::Text* editor{nullptr};
};

[[nodiscard]] cch::support::Expected<BuiltViewport> build_viewport(const JsonValue& inputs) {
    ChatViewportOptions options;
    auto document = std::make_unique<cch::tui::Text>(joined(inputs.at("document")), 0, 0);
    auto editor = std::make_unique<cch::tui::Text>(inputs.at("editor").get_string(), 0, 0);
    auto* const document_text = document.get();
    auto* const editor_text = editor.get();
    options.document = std::move(document);
    options.pending_messages = std::make_unique<cch::tui::Text>(
            inputs.get_object().contains("pending") ? inputs.at("pending").get_string() : "", 0, 0);
    options.status = std::make_unique<cch::tui::Text>(
            inputs.get_object().contains("status") ? inputs.at("status").get_string() : "", 0, 0);
    options.editor = std::move(editor);
    options.footer = std::make_unique<cch::tui::Text>(joined(inputs.at("footer")), 0, 0);
    if (inputs.get_object().contains("widgetsAbove")) {
        options.widgets_above = std::make_unique<cch::tui::Text>(joined(inputs.at("widgetsAbove")), 0, 0);
    }
    if (inputs.get_object().contains("widgetsBelow")) {
        options.widgets_below = std::make_unique<cch::tui::Text>(joined(inputs.at("widgetsBelow")), 0, 0);
    }
    if (inputs.get_object().contains("scrollbar")) {
        options.scrollbar = scrollbar_mode(inputs.at("scrollbar").get_string());
    }
    auto viewport = create_chat_viewport(std::move(options));
    if (!viewport) return std::unexpected(viewport.error());
    return BuiltViewport{.viewport = std::move(*viewport), .document = document_text, .editor = editor_text};
}

void check_step(const cch::tui::VirtualTerminal& terminal,
        const cch::tui::AltScreen& screen,
        const cch::tui::ScrollView& transcript,
        const JsonValue& expected) {
    CHECK(visible_rows(terminal.screen()) == visible_rows(strings_of(expected.at("screen"))));
    // Only the styled frames carry a full cell grid and scrollback reading, so
    // an unrecorded grid cannot silently hide a style in the other frames.
    if (expected.get_object().contains("scrollback")) {
        CHECK(terminal.scrollback() == strings_of(expected.at("scrollback")));
    }
    const cch::tui::CursorPosition cursor{
            .column = size_of(expected.at("cursor").at("column")),
            .row = size_of(expected.at("cursor").at("row")),
    };
    INFO(std::format("cursor actual=({},{}), expected=({},{})",
            terminal.cursor().column,
            terminal.cursor().row,
            cursor.column,
            cursor.row));
    CHECK(terminal.cursor() == cursor);
    CHECK(terminal.modes().cursor_visible == expected.at("cursorVisible").get_boolean());

    const auto& frozen = expected.at("transcript");
    CHECK(transcript.scroll_top() == size_of(frozen.at("scrollTop")));
    CHECK(transcript.viewport_height() == size_of(frozen.at("viewportHeight")));
    CHECK(transcript.following_end() == frozen.at("isFollowingEnd").get_boolean());
    INFO(std::format("transcript scrollbar actual={}, expected={}",
            scrollbar_name(transcript.scrollbar()),
            frozen.at("scrollbar").get_string()));
    CHECK(scrollbar_name(transcript.scrollbar()) == frozen.at("scrollbar").get_string());
    CHECK(transcript.is_scrollbar_visible() == frozen.at("isScrollbarVisible").get_boolean());

    // The alt screen's own viewport readings must follow the transcript, not
    // the implicit document some other component would provide.
    CHECK(screen.viewport_top() == size_of(expected.at("tui").at("viewportTop")));
    CHECK(screen.is_following_output() == expected.at("tui").at("isFollowingOutput").get_boolean());
    if (expected.get_object().contains("cells")) check_cells(terminal, expected.at("cells"));
}

} // namespace

TEST_CASE("Named pi-v1.0.4 chat-viewport evidence replays every frozen frame, style and cursor",
        "[coding_agent][tui][fullscreen][viewport][issue1036][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("chat-viewport.json");
    REQUIRE(bundle);
    CHECK(bundle->at("schema").get_string() == "pike/tui-evidence/1");
    CHECK(bundle->at("revision").get_string() == kFrozenRevision);
    CHECK(bundle->at("baseline").get_string() == "pi-v1.0.4");

    const auto scenarios = bundle->at("scenarios").get_array();
    CHECK(scenarios.size() == 10);
    std::size_t styled_steps = 0;
    for (const auto& scenario : scenarios) {
        const auto inputs = scenario.at("inputs");
        INFO(std::format("scenario {}", scenario.at("name").get_string()));

        cch::tui::VirtualTerminal terminal({
                .columns = size_of(inputs.at("columns")),
                .rows = size_of(inputs.at("rows")),
        });
        cch::tui::AltScreen screen(terminal);
        auto built = build_viewport(inputs);
        REQUIRE(built);
        auto* const transcript = built->viewport.transcript;
        screen.set_layout_root(std::move(built->viewport.root));
        REQUIRE(screen.start());

        // The recorded steps reassign the document and editor regions, so the
        // replay keeps their addresses the same way the frozen capture does.
        const auto& actions = inputs.at("steps").get_array();
        const auto& expected = scenario.at("expected").at("steps").get_array();
        REQUIRE(actions.size() == expected.size());
        for (std::size_t index = 0; index < actions.size(); ++index) {
            INFO(std::format("step {} ({})", index, actions[index].at("name").get_string()));
            if (actions[index].get_object().contains("resize")) {
                const auto& resize = actions[index].at("resize");
                REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{
                        .columns = size_of(resize.at("columns")),
                        .rows = size_of(resize.at("rows")),
                }));
            }
            if (actions[index].get_object().contains("document")) {
                built->document->set_text(joined(actions[index].at("document")));
            }
            if (actions[index].get_object().contains("editor")) {
                built->editor->set_text(actions[index].at("editor").get_string());
            }
            if (actions[index].get_object().contains("inputs")) {
                for (const auto& input : actions[index].at("inputs").get_array())
                    REQUIRE(terminal.inject_input(input.get_string()));
            }
            REQUIRE(screen.render());
            if (expected[index].get_object().contains("cells")) ++styled_steps;
            check_step(terminal, screen, *transcript, expected[index]);
        }
        CHECK(screen.stop());
    }
    // The scrollbar scenarios record their styled grid; without one the bar
    // cells could not be compared at all.
    CHECK(styled_steps >= 3);
}

} // namespace cch::coding_agent::tui
