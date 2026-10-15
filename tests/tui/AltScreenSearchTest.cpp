// Named pi-v1.0.4 fullscreen search observations and index contracts (#1010).

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/AltScreenSearch.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/FullscreenKeybindings.hpp"
#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

[[nodiscard]] cch::support::Expected<JsonValue> search_evidence() {
    return cch::tests::read_pi_tui_evidence("alt-screen-search.json");
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::vector<std::string> lines_of(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::vector<std::string> visible_lines(const cch::tui::VirtualTerminal& terminal) {
    auto lines = terminal.screen();
    for (auto& line : lines) {
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
    }
    return lines;
}

[[nodiscard]] cch::tui::TerminalCapabilities search_capabilities() {
    return cch::tui::TerminalCapabilities{
            .synchronized_output = true, .keyboard_protocol = cch::tui::KeyboardProtocol::Kitty};
}

void send(cch::tui::VirtualTerminal& terminal, std::string_view input) {
    REQUIRE(terminal.inject_input(std::string(input)));
}

class SearchEditor final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"editor"}};
    }
    void invalidate() override {}
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        if (const auto* key = std::get_if<cch::tui::KeyEvent>(&event)) inputs.push_back(key->raw_sequence);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::vector<std::string> inputs{};

private:
    bool focused_{false};
};

class SearchOverlayProbe final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"overlay"}};
    }
    void invalidate() override {}
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        if (const auto* key = std::get_if<cch::tui::KeyEvent>(&event)) inputs.push_back(key->raw_sequence);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::vector<std::string> inputs{};

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("fullscreen search normalizes multiline queries and maps ANSI "
          "Unicode columns",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& normalized = scenario_of(*bundle, "search-normalized-multirow");
    const auto expected = normalized.at("expected").get_array().front().at("segments").get_array();
    const auto matches = cch::tui::find_alt_screen_search_matches(
            lines_of(normalized.at("inputs").at("lines")), normalized.at("inputs").at("query").get_string());
    REQUIRE(matches.size() == 1);
    REQUIRE(matches.front().segments.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(matches.front().segments[index].row == static_cast<std::size_t>(expected[index].at("row").get_number()));
        CHECK(matches.front().segments[index].start_col ==
                static_cast<std::size_t>(expected[index].at("startCol").get_number()));
        CHECK(matches.front().segments[index].end_col ==
                static_cast<std::size_t>(expected[index].at("endCol").get_number()));
    }

    const auto& unicode = scenario_of(*bundle, "search-ansi-cjk-columns");
    const auto unicode_expected = unicode.at("expected").get_array().front().at("segments").get_array();
    const auto unicode_matches = cch::tui::find_alt_screen_search_matches(
            lines_of(unicode.at("inputs").at("lines")), unicode.at("inputs").at("query").get_string());
    REQUIRE(unicode_matches.size() == 1);
    REQUIRE(unicode_matches.front().segments.size() == unicode_expected.size());
    for (std::size_t index = 0; index < unicode_expected.size(); ++index) {
        CHECK(unicode_matches.front().segments[index].row ==
                static_cast<std::size_t>(unicode_expected[index].at("row").get_number()));
        CHECK(unicode_matches.front().segments[index].start_col ==
                static_cast<std::size_t>(unicode_expected[index].at("startCol").get_number()));
        CHECK(unicode_matches.front().segments[index].end_col ==
                static_cast<std::size_t>(unicode_expected[index].at("endCol").get_number()));
    }
    CHECK(cch::tui::alt_screen_search_match_key(matches.front()) == "0:6:1:5");
}

TEST_CASE("fullscreen search uses frozen JavaScript whitespace for grapheme clusters and NEL",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto combining_match = cch::tui::find_alt_screen_search_matches({" \xcc\x81"
                                                                           "b"},
            "\xcc\x81");
    REQUIRE(combining_match.size() == 1);
    REQUIRE(combining_match.front().segments.size() == 1);
    CHECK(combining_match.front().segments.front() == cch::tui::SearchSegment{.row = 0, .start_col = 0, .end_col = 1});

    const auto nel_query = cch::tui::find_alt_screen_search_matches({"left right"}, "left\xc2\x85right");
    CHECK(nel_query.empty());
}

TEST_CASE("fullscreen search index reuses cached matches until its inputs change",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-index-cache");
    cch::tui::SearchIndex index;
    const auto& inputs = scenario.at("inputs").get_array();
    const auto& expected = scenario.at("expected").get_array();
    REQUIRE(inputs.size() == expected.size());
    for (std::size_t step = 0; step < inputs.size(); ++step) {
        const auto result = index.search(lines_of(inputs[step].at("lines")), inputs[step].at("query").get_string());
        CHECK(result.changed == expected[step].at("changed").get_boolean());
        const auto& expected_matches = expected[step].at("matches").get_array();
        REQUIRE(result.matches.size() == expected_matches.size());
        for (std::size_t match_index = 0; match_index < result.matches.size(); ++match_index) {
            const auto& actual_segments = result.matches[match_index].segments;
            const auto& expected_segments = expected_matches[match_index].at("segments").get_array();
            REQUIRE(actual_segments.size() == expected_segments.size());
            for (std::size_t segment_index = 0; segment_index < actual_segments.size(); ++segment_index) {
                const auto& expected_segment = expected_segments[segment_index];
                CHECK(actual_segments[segment_index].row ==
                        static_cast<std::size_t>(expected_segment.at("row").get_number()));
                CHECK(actual_segments[segment_index].start_col ==
                        static_cast<std::size_t>(expected_segment.at("startCol").get_number()));
                CHECK(actual_segments[segment_index].end_col ==
                        static_cast<std::size_t>(expected_segment.at("endCol").get_number()));
            }
        }
    }
}

TEST_CASE("fullscreen search renders input before applying truncated result width",
        "[tui][alt-screen-search][issue1010]") {
    const auto registry = cch::tests::fullscreen_registry();
    cch::tui::AltScreenSearchComponent component([](std::string) {}, *registry);
    for (const char value : std::string{"abcdefghij"}) {
        const std::string text(1, value);
        const cch::tui::InputEventVariant key = cch::tui::KeyEvent{.key = text, .text = text};
        CHECK(component.handle_input(key) == cch::tui::InputAdmissionOutcome::Consumed);
    }
    component.set_result(0, 1);

    const auto rendered = component.render(12);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 3);
    CHECK(cch::tui::visible_width(rendered->lines[1]) == 12);
    CHECK(rendered->lines[1].find("j") != std::string::npos);
}

TEST_CASE("fullscreen search opens and renders its frozen navigation controls",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-render-controls");
    const auto& inputs = scenario.at("inputs");
    const auto& dimensions = scenario.at("dimensions");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(
            [&] {
                std::string text;
                for (const auto& line : inputs.at("transcript").get_array()) {
                    if (!text.empty()) text.push_back('\n');
                    text += line.get_string();
                }
                return text;
            }(),
            0,
            0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    send(terminal, inputs.at("open").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(scenario.at("expected").at("screen")));
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search next and previous shortcuts wrap through frozen matches",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-navigate-wrap");
    const auto& inputs = scenario.at("inputs");
    const auto& dimensions = scenario.at("dimensions");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(
            terminal, cch::tui::AltScreenOptions{.search_navigation_button_style = [](std::string text, bool hovered) {
                return std::string(hovered ? "\x1b[45m" : "\x1b[44m") + text + "\x1b[49m";
            }});
    std::string text;
    for (const auto& line : inputs.at("transcript").get_array()) {
        if (!text.empty()) text.push_back('\n');
        text += line.get_string();
    }
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(std::move(text), 0, 0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    send(terminal, "\x1b[102;6u");
    send(terminal, inputs.at("query").get_string());
    REQUIRE(tui.render());
    const auto& phases = scenario.at("expected").at("phases").get_array();
    REQUIRE(phases.size() == 9);
    CHECK(visible_lines(terminal) == lines_of(phases[0].at("screen")));
    send(terminal, "\r");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[1].at("screen")));
    send(terminal, "\x1b[13;2u");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[2].at("screen")));
    CHECK(!std::ranges::any_of(terminal.output(),
            [](const std::string& output) { return output.find("\x1b[45m↓ Enter\x1b[49m") != std::string::npos; }));
    const auto cursor_before_hover = terminal.cursor();
    send(terminal, inputs.at("hover").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[3].at("screen")));
    CHECK(std::ranges::any_of(terminal.output(),
            [](const std::string& output) { return output.find("\x1b[45m↓ Enter\x1b[49m") != std::string::npos; }));
    const auto& hover_cursor = scenario.at("expected").at("hoverCursor");
    CHECK(terminal.cursor() ==
            cch::tui::CursorPosition{.column = static_cast<std::size_t>(hover_cursor.at("after").at("x").get_number()),
                    .row = static_cast<std::size_t>(hover_cursor.at("after").at("y").get_number())});
    CHECK(cursor_before_hover == terminal.cursor());
    send(terminal, inputs.at("nextClick").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[4].at("screen")));
    send(terminal, "\x7f");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[5].at("screen")));
    send(terminal, "e");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[6].at("screen")));
    send(terminal, "\x1b[102;6u");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[7].at("screen")));
    send(terminal, "\x1b[102;6u");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[8].at("screen")));
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search close restores previous overlay focus and resumes overlay input",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-close-restores-focus");
    const auto& inputs = scenario.at("inputs");
    const auto& dimensions = scenario.at("dimensions");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(terminal);
    std::string transcript_text;
    for (const auto& line : inputs.at("transcript").get_array()) {
        if (!transcript_text.empty()) transcript_text.push_back('\n');
        transcript_text += line.get_string();
    }
    auto transcript =
            std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(std::move(transcript_text), 0, 0),
                    cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto editor = std::make_unique<SearchEditor>();
    auto* editor_pointer = editor.get();
    cch::tui::VStack root;
    REQUIRE(root.add_child(std::move(transcript), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .min_size = 1}));
    REQUIRE(root.add_child(std::move(editor), cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    tui.set_layout_root(std::make_unique<cch::tui::VStack>(std::move(root)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.set_focus(editor_pointer));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    auto other_overlay = std::make_unique<cch::tui::Overlay>();
    auto other_editor = std::make_unique<SearchOverlayProbe>();
    auto* other_editor_pointer = other_editor.get();
    REQUIRE(other_overlay->add_child(std::move(other_editor)));
    REQUIRE(other_overlay->focus_first());
    auto* overlay_pointer = other_overlay.get();
    auto overlay_result = tui.add_overlay(std::move(other_overlay));
    REQUIRE(overlay_result);
    const auto& expected = scenario.at("expected");
    const auto top_before = static_cast<std::size_t>(expected.at("topBefore").get_number());
    const auto top_after_page_up = static_cast<std::size_t>(expected.at("topAfterPageUp").get_number());
    CHECK(tui.viewport_top() == top_before);
    CHECK(top_after_page_up == top_before);
    send(terminal, inputs.at("pageUp").get_string());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == top_after_page_up);
    CHECK(other_editor_pointer->inputs == std::vector<std::string>{inputs.at("pageUp").get_string()});
    send(terminal, inputs.at("open").get_string());
    send(terminal, inputs.at("query").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(expected.at("openScreen")));
    CHECK_FALSE(other_editor_pointer->focused());
    REQUIRE(other_editor_pointer->inputs.size() == 1);
    CHECK(other_editor_pointer->inputs.front() == inputs.at("pageUp").get_string());
    CHECK(std::ranges::any_of(terminal.output(),
            [](const std::string& output) { return output.find("\x1b[1;7mneedle\x1b[22;27m") != std::string::npos; }));
    send(terminal, inputs.at("close").get_string());
    REQUIRE(tui.render());
    CHECK(overlay_pointer->focused());
    send(terminal, inputs.at("resumedInput").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(expected.at("closed").at("screen")));
    CHECK(overlay_pointer->focused());
    CHECK(other_editor_pointer->focused());
    std::vector<std::string> expected_overlay_inputs;
    for (const auto& input : expected.at("closed").at("overlayInputs").get_array())
        expected_overlay_inputs.push_back(input.get_string());
    CHECK(other_editor_pointer->inputs == expected_overlay_inputs);
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search ignores box drawing clicks as navigation controls",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-boxdrawing-not-buttons");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(terminal);
    std::string text;
    for (const auto& line : inputs.at("transcript").get_array()) {
        if (!text.empty()) text.push_back('\n');
        text += line.get_string();
    }
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(std::move(text), 0, 0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    send(terminal, "\x1b[102;6u");
    send(terminal, inputs.at("query").get_string());
    REQUIRE(tui.render());
    const auto& expected = scenario.at("expected");
    CHECK(visible_lines(terminal) == lines_of(expected.at("before")));
    send(terminal, inputs.at("click").get_string());
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(expected.at("after")));
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search preserves current and non-current highlight styles",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-styled-highlights");
    const auto& dimensions = scenario.at("dimensions");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{
                    .search_match_style = [](std::string text) { return "\x1b[41m" + text + "\x1b[49m"; },
                    .search_current_match_style = [](std::string text) { return "\x1b[42m" + text + "\x1b[49m"; }});
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("needle first\nmiddle\nneedle second\nend", 0, 0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    send(terminal, "\x1b[102;6u");
    send(terminal, "needle");
    REQUIRE(tui.render());
    const auto expected_writes = scenario.at("expected").at("writes").get_array();
    CHECK(std::ranges::any_of(terminal.output(),
            [](const std::string& output) { return output.find("\x1b[42mneedle\x1b[49m") != std::string::npos; }));
    CHECK(std::ranges::any_of(terminal.output(),
            [](const std::string& output) { return output.find("\x1b[41mneedle\x1b[49m") != std::string::npos; }));
    CHECK(!expected_writes.empty());
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search refreshes streamed transcript rows and resized overlays",
        "[tui][alt-screen-search][issue1010][compat-pi]") {
    const auto bundle = search_evidence();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "search-streaming-resize");
    const auto& dimensions = scenario.at("dimensions");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
            .capabilities = search_capabilities()});
    cch::tui::AltScreen tui(terminal);
    auto text = std::make_unique<cch::tui::Text>("line 1\nneedle first\nline 3\nneedle second\nline 5", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    send(terminal, "\x1b[102;6u");
    send(terminal, "needle");
    REQUIRE(tui.render());
    const auto& phases = scenario.at("expected").at("phases").get_array();
    REQUIRE(phases.size() == 3);
    CHECK(visible_lines(terminal) == lines_of(phases[0].at("screen")));
    text_pointer->set_text("line 1\nneedle first\nline 3\nneedle second\nline "
                           "5\nneedle third\nline 7");
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[1].at("screen")));
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 40, .rows = 6}));
    REQUIRE(tui.render());
    CHECK(visible_lines(terminal) == lines_of(phases[2].at("screen")));
    REQUIRE(tui.stop());
}

TEST_CASE("fullscreen search component renders the three-row frozen panel", "[tui][alt-screen-search][issue1010]") {
    const auto registry = cch::tests::fullscreen_registry();
    auto context = std::make_shared<cch::tui::KeybindingsContext>(registry);
    cch::tui::AltScreenSearchComponent component([](std::string) {}, *registry, context);
    const auto rendered = component.render(48);
    REQUIRE(rendered);
    CHECK(rendered->lines.size() == 3);
    CHECK(rendered->lines[2].find("Shift+Enter") != std::string::npos);
    CHECK(rendered->lines[2].find("Enter") != std::string::npos);
}
