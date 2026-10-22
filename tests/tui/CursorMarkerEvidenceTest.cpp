// Fake/hardware cursor marker contract (B62 / TCursor, #1120). Replays the
// registered frozen pi-v1.3.0 observations in
// fixtures/pi-v1.3.0/audit-1050/cursor-composition.json (revision
// 71023aec1546): `standalone-editor-fake-cursor-is-strippable`,
// `main-screen-focused-hardware-cursor-omits-fake-cursor`,
// `main-screen-unfocused-fake-cursor-stays-visible`,
// `truncated-line-drops-unterminated-fake-cursor` and the two
// `overlay-left-of-cursor-keeps-cursor-*` composition scenarios.
//
// The public caller is the real `Editor`/`Input` component rendered through
// `MainScreen` into a recording terminal: the marker bytes, the stripped text
// and the hardware-mode output are observed where a user would see them. The
// marker layer is the frozen pi contract (pi `tui.ts` `CURSOR_MARKER`,
// `FAKE_CURSOR_START`/`FAKE_CURSOR_END`, `renderFakeCursor`,
// `resolveFakeCursors`); the reverse-video bytes alone are not the contract,
// because a terminal that honours the marker as zero-width never sees them.

#include <cch/tui/Component.hpp>
#include <cch/tui/CursorMarkers.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

/// The frozen pi-v1.3.0 revision the registered audit evidence was captured at.
constexpr std::string_view kFrozenRevision{"71023aec1546d6d677a65669c5665c0927ce0dd0"};
constexpr std::string_view kFrozenSchema{"pike/pi-v1.3.0-audit-evidence/1"};

/// The reverse-video rendering the marker layer resolves to (pi
/// `resolveFakeCursors`).
constexpr std::string_view kReverseVideoOn = "\x1b[7m";
constexpr std::string_view kReverseVideoOff = "\x1b[27m";

[[nodiscard]] const JsonValue& find_scenario(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("frozen scenario not found: " << std::string{name});
    return bundle;
}

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.emplace_back(entry.get_string());
    return strings;
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

/// Every byte the terminal received, concatenated in write order. pi records a
/// frame as a list of writes; the C++ pipeline may split the same bytes at
/// different boundaries, so the comparison is over the concatenation.
[[nodiscard]] std::string all_output(const cch::tui::VirtualTerminal& terminal) {
    std::string joined;
    for (const auto& write : terminal.output())
        joined += write;
    return joined;
}

[[nodiscard]] std::string join(const std::vector<std::string>& writes) {
    std::string joined;
    for (const auto& write : writes)
        joined += write;
    return joined;
}

/// `text` with every escape sequence removed. The general
/// `strip_terminal_sequences` only knows the finals it tracks; a frame also
/// carries the synchronized-output marker (`CSI ?2026h`), whose `h` final it
/// does not, so the frame comparison needs the general form.
[[nodiscard]] std::string without_sequences(std::string_view text) {
    std::string result;
    std::size_t position = 0;
    while (position < text.size()) {
        if (text[position] != '\x1b') {
            result += text[position];
            ++position;
            continue;
        }
        if (position + 1 >= text.size()) break;
        const auto kind = text[position + 1];
        if (kind == '[') {
            std::size_t cursor = position + 2;
            while (cursor < text.size() && (static_cast<unsigned char>(text[cursor]) < 0x40 ||
                                                   static_cast<unsigned char>(text[cursor]) > 0x7E)) {
                ++cursor;
            }
            position = cursor < text.size() ? cursor + 1 : text.size();
            continue;
        }
        std::size_t cursor = position + 2;
        std::size_t terminator = 0;
        while (cursor < text.size()) {
            if (text[cursor] == '\x07') {
                terminator = 1;
                break;
            }
            if (text[cursor] == '\x1b' && cursor + 1 < text.size() && text[cursor + 1] == '\\') {
                terminator = 2;
                break;
            }
            ++cursor;
        }
        position = terminator == 0 ? text.size() : cursor + terminator;
    }
    return result;
}

/// The drawn cells of each row of `frame`, right-trimmed.
[[nodiscard]] std::vector<std::string> drawn_rows(std::string_view frame) {
    std::vector<std::string> rows;
    std::size_t start = 0;
    while (start <= frame.size()) {
        const auto end = frame.find('\n', start);
        auto row = without_sequences(
                frame.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        while (!row.empty() && (row.back() == ' ' || row.back() == '\r'))
            row.pop_back();
        rows.push_back(std::move(row));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return rows;
}

/// Every reverse-video SGR byte `frame` draws, in order. This is the fake
/// cursor itself, so it is compared exactly.
[[nodiscard]] std::string reverse_video_bytes(std::string_view frame) {
    std::string bytes;
    for (std::size_t position = frame.find(kReverseVideoOn); position != std::string_view::npos;
            position = frame.find(kReverseVideoOn, position + kReverseVideoOn.size())) {
        bytes.append(kReverseVideoOn);
    }
    for (std::size_t position = frame.find(kReverseVideoOff); position != std::string_view::npos;
            position = frame.find(kReverseVideoOff, position + kReverseVideoOff.size())) {
        bytes.append(kReverseVideoOff);
    }
    return bytes;
}

/// The hardware-cursor positioning `frame` ends with (`CSI n G`), or nothing
/// when it does not reposition the cursor.
[[nodiscard]] std::string cursor_suffix(std::string_view frame) {
    const auto end = frame.rfind('G');
    if (end == std::string_view::npos) return {};
    const auto start = frame.rfind("\x1b[", end);
    if (start == std::string_view::npos) return {};
    return std::string{frame.substr(start, end - start + 1)};
}

/// Compare `output` against the artifact's recorded `writes`.
///
/// The compared properties are the ones this ticket owns: the drawn cells, the
/// reverse-video bytes (the fake cursor) and the hardware-cursor positioning.
/// Two pre-existing frame-writing conventions are not compared, because the C++
/// pipeline already differed from frozen there before the marker layer: it pads
/// every composed row to the terminal width at the frame boundary while frozen
/// pads inside the component, and it closes a spliced region with one
/// `SEGMENT_RESET` where frozen opens and closes the region with one each.
void check_frame_matches_frozen(std::string_view output, const std::vector<std::string>& frozen_writes) {
    const auto frozen = join(frozen_writes);
    INFO("FROZEN: " << frozen);
    CHECK(drawn_rows(output) == drawn_rows(frozen));
    CHECK(reverse_video_bytes(output) == reverse_video_bytes(frozen));
    CHECK(cursor_suffix(output) == cursor_suffix(frozen));
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
        return cch::tui::RenderResult{.lines = {std::string{"abc"} + std::string{cch::tui::kCursorMarker}}};
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
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "cursor-composition.json");
    REQUIRE(bundle.has_value());
    CHECK(bundle->at("schema").get_string() == kFrozenSchema);
    CHECK(bundle->at("revision").get_string() == kFrozenRevision);

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
        marker_count += count_occurrences(line, cch::tui::kFakeCursorStart);
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
    CHECK(count_occurrences(content, cch::tui::kFakeCursorStart) == 1);
    CHECK(count_occurrences(content, cch::tui::kFakeCursorEnd) == 1);
    CHECK(content.find(cch::tui::kFakeCursorStart) < content.find(cch::tui::kFakeCursorEnd));

    // Stripping the markers leaves the frozen visible text; the reverse-video
    // bytes are not part of the standalone contract.
    CHECK(cch::tui::strip_terminal_sequences(content) == "hi        ");
    CHECK_FALSE(contains(content, kReverseVideoOn));
    CHECK_FALSE(contains(content, kReverseVideoOff));
}

TEST_CASE("hardware cursor mode omits the focused fake cursor while the unfocused one stays visible",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "cursor-composition.json");
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
        const auto expected_writes = strings_of(expected.at("writes"));
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();
        const auto expected_hardware_positioned = expected.at("hardwareCursorPositioned").get_boolean();

        const auto output = render_input(true, true);

        // The discriminating assertion: the reverse-video bytes must be
        // ABSENT. A cursor that is strippable but still drawn fails here.
        CHECK_FALSE(contains(output, kReverseVideoOn));
        CHECK_FALSE(contains(output, kReverseVideoOff));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorEnd));
        // The marker is consumed too: it never reaches the terminal.
        CHECK_FALSE(contains(output, cch::tui::kCursorMarker));
        CHECK(expected_fake_cursor_visible == false);
        CHECK(expected_hardware_positioned == true);

        // The recorded frame bytes are the expectation, not a hand-written
        // substring of them.
        REQUIRE_FALSE(expected_writes.empty());
        check_frame_matches_frozen(output, expected_writes);
    }

    SECTION("the focused fake cursor is drawn when the hardware cursor is hidden") {
        const auto& scenario = find_scenario(*bundle, "main-screen-focused-fake-cursor-drawn");
        const auto& expected = scenario.at("expected");
        const auto expected_writes = strings_of(expected.at("writes"));
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();

        const auto output = render_input(false, true);

        CHECK(contains(output, "\x1b[7ma\x1b[27m"));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kCursorMarker));
        CHECK(expected_fake_cursor_visible == true);

        REQUIRE_FALSE(expected_writes.empty());
        check_frame_matches_frozen(output, expected_writes);
    }

    SECTION("an unfocused fake cursor stays visible in hardware cursor mode") {
        const auto& scenario = find_scenario(*bundle, "main-screen-unfocused-fake-cursor-stays-visible");
        const auto& expected = scenario.at("expected");
        const auto expected_writes = strings_of(expected.at("writes"));
        const auto expected_fake_cursor_visible = expected.at("fakeCursorVisible").get_boolean();
        const auto expected_hardware_positioned = expected.at("hardwareCursorPositioned").get_boolean();

        const auto output = render_input(true, false);

        CHECK(contains(output, "\x1b[7ma\x1b[27m"));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kCursorMarker));
        CHECK(expected_fake_cursor_visible == true);
        CHECK(expected_hardware_positioned == false);

        REQUIRE_FALSE(expected_writes.empty());
        check_frame_matches_frozen(output, expected_writes);
    }
}

TEST_CASE("a truncated line drops the unterminated fake cursor marker",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "cursor-composition.json");
    REQUIRE(bundle.has_value());
    const auto& scenario = find_scenario(*bundle, "truncated-line-drops-unterminated-fake-cursor");
    const auto& expected = scenario.at("expected");
    const auto expected_writes = strings_of(expected.at("writes"));
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
    CHECK(contains(output, "abc"));
    CHECK(expected_fake_cursor_visible == false);

    REQUIRE_FALSE(expected_writes.empty());
    check_frame_matches_frozen(output, expected_writes);
}

TEST_CASE("an overlay's focused fake cursor resolves before it is spliced",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // Frozen pi resolves an overlay's own rendered lines before splicing them
    // (pi `tui.ts` `compositeOverlays`: `this.resolveFakeCursors(component.render(width))`).
    // Without that stage the overlay's marker bytes reach the terminal and its
    // drawn cursor loses its reverse video.
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "cursor-composition.json");
    REQUIRE(bundle.has_value());

    const auto render_overlay = [](bool show_hardware_cursor) {
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{terminal, show_hardware_cursor};
        REQUIRE(main_screen.add_child(std::make_unique<FrozenLineChild>("abc" + std::string{cch::tui::kCursorMarker} +
                                                                        std::string{cch::tui::kFakeCursorStart} + "d" +
                                                                        std::string{cch::tui::kFakeCursorEnd} + "ef")));
        cch::tui::OverlayOptions options;
        options.row = 0.0;
        options.col = 0.0;
        options.width = 3.0;
        auto overlay = std::make_unique<cch::tui::Overlay>(options);
        REQUIRE(overlay->add_child(std::make_unique<FrozenLineChild>("XXX")));
        REQUIRE(main_screen.add_overlay(std::move(overlay)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.render());
        return all_output(terminal);
    };

    SECTION("the fake cursor is drawn when the hardware cursor is hidden") {
        const auto& scenario = find_scenario(*bundle, "overlay-left-of-cursor-keeps-cursor-fake");
        const auto& expected = scenario.at("expected");
        const auto expected_writes = strings_of(expected.at("writes"));

        const auto output = render_overlay(false);

        INFO("OUTPUT: " << output);
        CHECK(contains(output, "XXX"));
        // The overlay content must not leak the raw marker bytes: they are
        // resolved to the drawn reverse-video cursor.
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorEnd));
        CHECK(contains(output, "\x1b[7md\x1b[27m"));
        CHECK(expected.at("fakeCursorVisible").get_boolean() == true);
        CHECK(expected.at("hardwareCursorPositioned").get_boolean() == true);

        REQUIRE_FALSE(expected_writes.empty());
        check_frame_matches_frozen(output, expected_writes);
    }

    SECTION("the fake cursor is omitted when the hardware cursor is shown") {
        const auto& scenario = find_scenario(*bundle, "overlay-left-of-cursor-keeps-cursor-hardware");
        const auto& expected = scenario.at("expected");
        const auto expected_writes = strings_of(expected.at("writes"));

        const auto output = render_overlay(true);

        INFO("OUTPUT: " << output);
        CHECK(contains(output, "XXX"));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorEnd));
        CHECK_FALSE(contains(output, kReverseVideoOn));
        CHECK(expected.at("fakeCursorVisible").get_boolean() == false);
        CHECK(expected.at("hardwareCursorPositioned").get_boolean() == true);

        REQUIRE_FALSE(expected_writes.empty());
        check_frame_matches_frozen(output, expected_writes);
    }
}

TEST_CASE("an overlay's own focused child resolves its fake cursor before the splice",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // Frozen pi resolves the OVERLAY's own rendered lines before splicing them
    // (pi `tui.ts` `compositeOverlays`:
    // `let overlayLines = this.resolveFakeCursors(component.render(width));`).
    // The registered `overlay-left-of-cursor-*` scenarios cover a base-line
    // marker surviving the splice; this case covers the overlay's own content,
    // where an unresolved marker ships its APC bytes to the terminal and the
    // overlay's drawn cursor loses the reverse video.
    const auto render_focused_overlay = [](bool show_hardware_cursor) {
        cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{terminal, show_hardware_cursor};
        REQUIRE(main_screen.add_child(std::make_unique<FrozenLineChild>("base")));
        cch::tui::OverlayOptions options;
        options.row = 0.0;
        options.col = 0.0;
        options.width = 5.0;
        auto overlay = std::make_unique<cch::tui::Overlay>(options);
        // The overlay's focused child draws its cursor cell in the overlay.
        REQUIRE(overlay->add_child(std::make_unique<FrozenLineChild>("X" + std::string{cch::tui::kCursorMarker} +
                                                                     std::string{cch::tui::kFakeCursorStart} + "d" +
                                                                     std::string{cch::tui::kFakeCursorEnd} + "Y")));
        REQUIRE(overlay->focus_first());
        REQUIRE(main_screen.add_overlay(std::move(overlay)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.render());
        return all_output(terminal);
    };

    SECTION("the overlay's fake cursor is drawn when the hardware cursor is hidden") {
        const auto output = render_focused_overlay(false);
        INFO("OUTPUT: " << output);
        // The overlay's marker bytes are resolved, not forwarded.
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorEnd));
        CHECK(contains(output, "X\x1b[7md\x1b[27mY"));
    }

    SECTION("the overlay's focused fake cursor is omitted in hardware-cursor mode") {
        const auto output = render_focused_overlay(true);
        INFO("OUTPUT: " << output);
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorStart));
        CHECK_FALSE(contains(output, cch::tui::kFakeCursorEnd));
        CHECK_FALSE(contains(output, kReverseVideoOn));
        CHECK(contains(output, "XdY"));
    }
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

TEST_CASE("a fake cursor after an unpaired first marker is still suppressed",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // Frozen pi scans for the first `CURSOR_MARKER` that is IMMEDIATELY
    // FOLLOWED by `FAKE_CURSOR_START` (pi `FOCUSED_FAKE_CURSOR`,
    // `tui.ts:202`), not for the first marker anywhere in the line. A line
    // whose first marker is unpaired must therefore still lose the reverse
    // video of the later, paired fake cursor.
    const std::string line = std::string{cch::tui::kCursorMarker} + "x" + std::string{cch::tui::kCursorMarker} +
                             std::string{cch::tui::kFakeCursorStart} + "a" + std::string{cch::tui::kFakeCursorEnd};

    cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
    cch::tui::MainScreen main_screen{terminal, true};
    auto child = std::make_unique<FrozenLineChild>(line);
    auto* child_pointer = child.get();
    REQUIRE(main_screen.add_child(std::move(child)));
    REQUIRE(main_screen.start());
    REQUIRE(main_screen.set_focus(child_pointer));
    REQUIRE(main_screen.render());

    const auto output = all_output(terminal);
    INFO("OUTPUT: " << output);
    // The paired fake cursor is drawn as the hardware cursor there, so its
    // reverse-video wrappers must not reach the terminal.
    CHECK_FALSE(contains(output, kReverseVideoOn));
    CHECK_FALSE(contains(output, kReverseVideoOff));
}

TEST_CASE("an overlay's marker cursor wins over its out-of-band component cursor",
        "[tui][cursor-marker][issue1120][compat-pi][spec]") {
    // The same precedence holds for a focused overlay: the overlay reports its
    // focused child's `cursor_location()` out of band, but the marker the child
    // rendered is the authority (pi `extractCursorPosition` runs on the
    // composited lines and has no out-of-band alternative).
    cch::tui::VirtualTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 20, .rows = 5});
    cch::tui::MainScreen main_screen{terminal, true};
    REQUIRE(main_screen.add_child(std::make_unique<FrozenLineChild>("base")));
    cch::tui::OverlayOptions options;
    options.row = 0.0;
    options.col = 0.0;
    options.width = 3.0;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<ConflictingCursorChild>()));
    REQUIRE(overlay->focus_first());
    REQUIRE(main_screen.add_overlay(std::move(overlay)));
    REQUIRE(main_screen.start());
    REQUIRE(main_screen.render());

    // The overlay's child renders "abc" + marker at column 0, so the marker
    // places the terminal cursor in column 3; its out-of-band position claims
    // column 0.
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
    CHECK_FALSE(contains(output, cch::tui::kCursorMarker));
    // The prompt is followed by the value, with no marker between them.
    CHECK(contains(output, "> abc"));
}
