// Fake/hardware cursor marker contract (B62 / TCursor, #1120). Replays the
// registered frozen pi-v1.3.0 observations in
// fixtures/pi-v1.3.0/audit-1050/cursor-composition.json (revision
// 71023aec1546): `standalone-editor-fake-cursor-is-strippable`,
// `main-screen-focused-hardware-cursor-omits-fake-cursor`,
// `main-screen-unfocused-fake-cursor-stays-visible` and
// `truncated-line-drops-unterminated-fake-cursor`.
//
// The public caller is the real `Editor`/`Input` component rendered through
// `MainScreen` into a recording terminal: the marker bytes, the stripped text
// and the hardware-mode output are observed where a user would see them. The
// marker layer is the frozen pi contract (pi `tui.ts` `CURSOR_MARKER`,
// `FAKE_CURSOR_START`/`FAKE_CURSOR_END`, `renderFakeCursor`,
// `resolveFakeCursors`); the reverse-video bytes alone are not the contract,
// because a terminal that honours the marker as zero-width never sees them.

#include <cch/tui/Component.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

/// The frozen marker bytes (pi `tui.ts` at the registered revision).
constexpr std::string_view kCursorMarker = "\x1b_pi:c\x07";
constexpr std::string_view kFakeCursorStart = "\x1b_pi:fc\x07";
constexpr std::string_view kFakeCursorEnd = "\x1b_pi:/fc\x07";

/// The reverse-video rendering the marker layer resolves to (pi
/// `resolveFakeCursors`).
constexpr std::string_view kReverseVideoOn = "\x1b[7m";
constexpr std::string_view kReverseVideoOff = "\x1b[27m";

/// Read the registered frozen artifact through the named evidence chain, so a
/// tampered or unregistered artifact cannot become the expectation.
[[nodiscard]] cch::support::Expected<JsonValue> read_cursor_composition() {
    const auto runner = std::filesystem::path{CCH_SOURCE_DIR} / "scripts/evidence/audit_evidence.py";
    const std::string command =
            "python3 " + runner.string() + " read --family audit-1050 --artifact cursor-composition.json";
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Unknown, "Failed to start the audit evidence reader"));
    }
    std::string payload;
    std::vector<char> buffer(4096);
    while (std::size_t count = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
        payload.append(buffer.data(), count);
    }
    const bool read_failed = std::ferror(pipe) != 0;
    const int status = ::pclose(pipe);
    if (read_failed || status != 0) {
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Unknown, "Named pi-v1.3.0 cursor evidence verification failed"));
    }
    return cch::support::read_json(payload);
}

[[nodiscard]] const JsonValue& find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("frozen scenario not found: " << std::string{name});
    return bundle;
}

[[nodiscard]] std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t position = haystack.find(needle);
    while (position != std::string_view::npos) {
        ++count;
        position = haystack.find(needle, position + needle.size());
    }
    return count;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// Every byte the terminal received, concatenated in write order.
[[nodiscard]] std::string all_output(const cch::tui::VirtualTerminal& terminal) {
    std::string joined;
    for (const auto& write : terminal.output())
        joined += write;
    return joined;
}

/// A component whose rendered line is frozen text, used to replay the
/// truncation and overlay scenarios that drive the resolution stage directly.
class FrozenLineChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    explicit FrozenLineChild(std::string line) : line_(std::move(line)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {line_}};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

private:
    std::string line_;
    bool focused_{false};
};

/// A component whose rendered line carries the cursor marker after three
/// visible cells while its out-of-band cursor position claims column 0. The
/// two disagree, so which one positions the terminal cursor is observable.
class ConflictingCursorChild final : public cch::tui::Component, public cch::tui::Focusable {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {std::string{"abc"} + std::string{kCursorMarker}}};
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return cch::tui::CursorPosition{.column = 0, .row = 0};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("the standalone editor draws a strippable fake cursor marker",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    const auto bundle = read_cursor_composition();
    REQUIRE(bundle.has_value());
    const auto& scenario = find_scenario(*bundle, "standalone-editor-fake-cursor-is-strippable");
    const auto& expected = scenario.at("expected");
    const auto& frozen_lines = expected.at("rawLines").get_array();
    const auto& frozen_stripped = expected.at("strippedLines").get_array();
    const auto expected_marker_count = static_cast<std::size_t>(expected.at("fakeCursorMarkerCount").get_number());
    const auto width = static_cast<std::size_t>(scenario.at("inputs").at("width").get_number());

    cch::tui::Editor editor;
    editor.set_text("hi");
    // The frozen capture used pi's default editor theme, whose border is the
    // dim text style and whose text style is empty.
    editor.set_border_style([](std::string text) { return "\x1b[2m" + text + "\x1b[22m"; });

    // The standalone editor renders outside a running TUI: the marker pair is
    // part of the component's own rendered line.
    const auto rendered = editor.render(width);
    REQUIRE(rendered.has_value());
    REQUIRE(rendered->lines.size() == frozen_lines.size());

    std::size_t marker_count = 0;
    for (std::size_t index = 0; index < rendered->lines.size(); ++index) {
        const auto& line = rendered->lines[index];
        INFO("editor line " << index);
        // The frozen artifact is the expectation; the C++ line must carry the
        // same marker bytes in the same order.
        CHECK(line == frozen_lines[index].get_string());
        marker_count += count_occurrences(line, kFakeCursorStart);
        // The marker pair is strippable: removing the zero-width APC sequences
        // leaves exactly the frozen visible text.
        CHECK(cch::tui::strip_terminal_sequences(line) == frozen_stripped[index].get_string());
    }
    CHECK(marker_count == expected_marker_count);
}

TEST_CASE("the standalone editor fake cursor marker pair is strippable and ordered",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    cch::tui::Editor editor;
    editor.set_text("hi");
    editor.set_border_style([](std::string text) { return "\x1b[2m" + text + "\x1b[22m"; });

    const auto rendered = editor.render(10);
    REQUIRE(rendered.has_value());
    REQUIRE(rendered->lines.size() == 3);

    const auto& content = rendered->lines[1];
    // The marker pair brackets exactly one grapheme and appears once.
    CHECK(count_occurrences(content, kFakeCursorStart) == 1);
    CHECK(count_occurrences(content, kFakeCursorEnd) == 1);
    CHECK(content.find(kFakeCursorStart) < content.find(kFakeCursorEnd));

    // Stripping the markers leaves the frozen visible text; the reverse-video
    // bytes are not part of the standalone contract.
    CHECK(cch::tui::strip_terminal_sequences(content) == "hi        ");
    CHECK_FALSE(contains(content, kReverseVideoOn));
    CHECK_FALSE(contains(content, kReverseVideoOff));
}

TEST_CASE("hardware cursor mode omits the focused fake cursor while the unfocused one stays visible",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    const auto bundle = read_cursor_composition();
    REQUIRE(bundle.has_value());

    const auto render_input = [](bool show_hardware_cursor, bool focus) {
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{terminal, show_hardware_cursor};
        auto input = std::make_unique<cch::tui::Input>();
        input->set_value("abc");
        auto* input_pointer = input.get();
        REQUIRE(main_screen.add_child(std::move(input)));
        REQUIRE(main_screen.start());
        if (focus) {
            REQUIRE(main_screen.set_focus(input_pointer));
        } else {
            // The C++ main screen focuses its first focusable child when it
            // starts, so the unfocused case clears the focus explicitly.
            REQUIRE(main_screen.set_focus(nullptr));
        }
        REQUIRE(main_screen.render());
        return all_output(terminal);
    };

    SECTION("the focused fake cursor is omitted when the hardware cursor is shown") {
        const auto& scenario = find_scenario(*bundle, "main-screen-focused-hardware-cursor-omits-fake-cursor");
        const auto& expected = scenario.at("expected");
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();
        const auto expected_hardware_positioned = expected.at("hardwareCursorPositioned").get_boolean();

        const auto output = render_input(true, true);

        // The discriminating assertion: the reverse-video bytes must be
        // ABSENT. A cursor that is strippable but still drawn fails here.
        CHECK_FALSE(contains(output, kReverseVideoOn));
        CHECK_FALSE(contains(output, kReverseVideoOff));
        CHECK_FALSE(contains(output, kFakeCursorStart));
        CHECK_FALSE(contains(output, kFakeCursorEnd));
        // The marker is consumed too: it never reaches the terminal.
        CHECK_FALSE(contains(output, kCursorMarker));
        // The visible text is unchanged.
        CHECK(contains(output, "> abc"));
        CHECK(expected_fake_cursor_visible == false);
        CHECK(expected_hardware_positioned == true);
    }

    SECTION("the focused fake cursor is drawn when the hardware cursor is hidden") {
        const auto& scenario = find_scenario(*bundle, "main-screen-focused-fake-cursor-drawn");
        const auto& expected = scenario.at("expected");
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();

        const auto output = render_input(false, true);

        CHECK(contains(output, "\x1b[7ma\x1b[27m"));
        CHECK_FALSE(contains(output, kFakeCursorStart));
        CHECK_FALSE(contains(output, kCursorMarker));
        CHECK(expected_fake_cursor_visible == true);
    }

    SECTION("an unfocused fake cursor stays visible in hardware cursor mode") {
        const auto& scenario = find_scenario(*bundle, "main-screen-unfocused-fake-cursor-stays-visible");
        const auto& expected = scenario.at("expected");
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();
        const auto expected_hardware_positioned = expected.at("hardwareCursorPositioned").get_boolean();

        const auto output = render_input(true, false);

        INFO("OUTPUT: " << output);
        CHECK(contains(output, "\x1b[7ma\x1b[27m"));
        CHECK_FALSE(contains(output, kFakeCursorStart));
        CHECK_FALSE(contains(output, kCursorMarker));
        CHECK(expected_fake_cursor_visible == true);
        CHECK(expected_hardware_positioned == false);
    }
}

TEST_CASE("a truncated line drops the unterminated fake cursor marker",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    const auto bundle = read_cursor_composition();
    REQUIRE(bundle.has_value());
    const auto& scenario = find_scenario(*bundle, "truncated-line-drops-unterminated-fake-cursor");
    const auto& expected = scenario.at("expected");
    const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();

    // The frozen line is truncated right after the cursor cell, which drops the
    // closing marker.
    // The trailing cell follows the marker terminator, so the literal splits
    // it: a C++ hex escape swallows every following hex digit, and
    // "\x07c" would be one escape (0x7c) instead of BEL followed by 'c'.
    const std::string truncated = "ab\x1b_pi:c\x07"
                                  "\x1b_pi:fc\x07"
                                  "c";

    cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
    cch::tui::MainScreen main_screen{terminal, true};
    auto child = std::make_unique<FrozenLineChild>(truncated);
    auto* child_pointer = child.get();
    REQUIRE(main_screen.add_child(std::move(child)));
    REQUIRE(main_screen.start());
    REQUIRE(main_screen.set_focus(child_pointer));
    const auto render_result = main_screen.render();
    REQUIRE(render_result);

    const auto output = all_output(terminal);
    // An unterminated fake cursor must not leave reverse-video bytes behind.
    CHECK_FALSE(contains(output, kReverseVideoOn));
    CHECK_FALSE(contains(output, kReverseVideoOff));
    // The visible text before the truncation survives.
    CHECK(contains(output, "ab"));
    CHECK(expected_fake_cursor_visible == false);
}

TEST_CASE("the marker cursor wins over the out-of-band component cursor",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // The marker is measured on the rendered text, so it is the authority for
    // the terminal cursor position (pi `extractCursorPosition`). The
    // out-of-band component position is only the fallback for a frame that
    // carries no marker. The child below reports an out-of-band position that
    // disagrees with its own marker, so the precedence is observable.
    cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
    cch::tui::MainScreen main_screen{terminal, true};
    auto child = std::make_unique<ConflictingCursorChild>();
    auto* child_pointer = child.get();
    REQUIRE(main_screen.add_child(std::move(child)));
    REQUIRE(main_screen.start());
    REQUIRE(main_screen.set_focus(child_pointer));
    REQUIRE(main_screen.render());

    // The marker sits after three visible cells, so the terminal cursor is
    // in column 3; the out-of-band position claims column 0.
    CHECK(terminal.cursor().row == 0);
    CHECK(terminal.cursor().column == 3);
}

TEST_CASE("the cursor marker is stripped before the column is measured",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // The frozen pi contract extracts the cursor column from the visible text
    // before the marker and strips the marker from the emitted line
    // (pi `tui.ts` `extractCursorPosition`).
    cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
    cch::tui::MainScreen main_screen{terminal, true};
    auto input = std::make_unique<cch::tui::Input>();
    input->set_value("abc");
    auto* input_pointer = input.get();
    REQUIRE(main_screen.add_child(std::move(input)));
    REQUIRE(main_screen.start());
    REQUIRE(main_screen.set_focus(input_pointer));
    REQUIRE(main_screen.render());

    // The prompt occupies two columns, so the cursor column is 2 (0-based),
    // measured on the text before the marker.
    REQUIRE(terminal.cursor().column == 2);
    CHECK(terminal.cursor().row == 0);

    const auto output = all_output(terminal);
    INFO("OUTPUT: " << output);
    // The marker never reaches the terminal: it is consumed by the extraction.
    CHECK_FALSE(contains(output, kCursorMarker));
    // The prompt is followed by the value, with no marker between them.
    CHECK(contains(output, "> abc"));
}
