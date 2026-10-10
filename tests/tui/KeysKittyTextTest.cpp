// #950: the Kitty insertion text and the shortcut identity are one event with
// two distinct values. A non-Latin layout keeps the character the terminal
// reported for insertion, while the base-layout key still answers "is this the
// shortcut?". Both must survive the whole path — decode, Input, Editor and the
// Editor character jump — instead of being reconstructed from each other.

#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace cch;

namespace {

/// The Kitty CSI-u payloads under test, decoded through the public parse seam
/// with the keyboard protocol state a caller holds for its own terminal.
[[nodiscard]] std::optional<tui::KeyEvent> kitty(std::string_view payload) {
    return tui::parse_key(payload, /*kitty_protocol_active=*/true);
}

} // namespace

TEST_CASE("a Kitty non-Latin key inserts its own character and keeps the base-layout shortcut",
        "[tui][input][issue950][spec]") {
    // CSI 1092::97u: the terminal reports U+0444 (Cyrillic ef) whose physical
    // key is the base-layout 'a'.
    const auto event = kitty("\x1b[1092::97u");

    REQUIRE(event);
    CHECK(event->key == "a");
    CHECK(event->text == "\xd1\x84");
    CHECK(tui::matches_key(*event, "a"));
    CHECK(tui::printable_text(*event) == std::optional<std::string>{"\xd1\x84"});
    // The shortcut identity is never the insertion text.
    CHECK(tui::printable_text(*event) != event->key);
}

TEST_CASE("shifted, supplementary and legacy printables keep their own characters", "[tui][input][issue950][spec]") {
    // CSI 49:33;2u: shift-modified '1' takes the shifted key's symbol.
    const auto shifted = kitty("\x1b[49:33;2u");
    REQUIRE(shifted);
    CHECK(shifted->key == "!");
    CHECK(shifted->shift);
    CHECK(shifted->text == "!");
    CHECK(tui::printable_text(*shifted) == std::optional<std::string>{"!"});

    // A supplementary codepoint is text, not a truncated code unit.
    const auto supplementary = kitty("\x1b[128512u");
    REQUIRE(supplementary);
    CHECK(supplementary->text == "\xf0\x9f\x98\x80");
    CHECK(tui::printable_text(*supplementary) == std::optional<std::string>{"\xf0\x9f\x98\x80"});

    // A shift-modified non-Latin layout keeps the shifted layout character as
    // its text while the identity stays the base-layout key.
    const auto shifted_cyrillic = kitty("\x1b[1060:1040:97;2u");
    REQUIRE(shifted_cyrillic);
    CHECK(shifted_cyrillic->key == "a");
    CHECK(shifted_cyrillic->shift);
    CHECK(shifted_cyrillic->text == "\xd0\xa1");
    CHECK(tui::printable_text(*shifted_cyrillic) == std::optional<std::string>{"\xd0\xa1"});

    // The legacy paths still type what the terminal sent: the identifier is
    // canonical, the inserted text keeps the typed case and non-ASCII bytes.
    const auto uppercase = tui::parse_key("A", false);
    REQUIRE(uppercase);
    CHECK(uppercase->key == "a");
    CHECK(uppercase->shift);
    CHECK(tui::printable_text(*uppercase) == std::optional<std::string>{"A"});
    const auto accented = tui::parse_key("\xc3\xa9", false);
    REQUIRE(accented);
    CHECK(tui::printable_text(*accented) == std::optional<std::string>{"\xc3\xa9"});
}

TEST_CASE("repeat inserts, release does not, and non-printing modifiers never carry text",
        "[tui][input][issue950][spec]") {
    const auto press = kitty("\x1b[1092::97;1u");
    REQUIRE(press);
    CHECK(press->type == tui::KeyEventType::Press);
    CHECK(tui::carries_press_behavior(&*press));
    CHECK(tui::printable_text(*press) == std::optional<std::string>{"\xd1\x84"});

    const auto repeat = kitty("\x1b[1092::97;2u");
    REQUIRE(repeat);
    CHECK(repeat->type == tui::KeyEventType::Repeat);
    CHECK(tui::printable_text(*repeat) == std::optional<std::string>{"\xd1\x84"});

    // The release still identifies the shortcut but inserts nothing.
    const auto release = kitty("\x1b[1092::97;3u");
    REQUIRE(release);
    CHECK(release->type == tui::KeyEventType::Release);
    CHECK(tui::matches_key(*release, "a"));
    CHECK_FALSE(tui::carries_press_behavior(&*release));
    CHECK_FALSE(tui::printable_text(*release).has_value());

    // ctrl (5), alt (3) and super (9) combinations are shortcuts even on a
    // non-Latin layout: the same payload never carries insertion text.
    for (const auto payload : {"\x1b[1092::97;5u", "\x1b[1092::97;3u", "\x1b[1092::97;9u"}) {
        const auto modified = kitty(payload);
        REQUIRE(modified);
        CHECK(modified->text.empty());
        CHECK_FALSE(tui::printable_text(*modified).has_value());
    }
}

TEST_CASE("keyboard parse, match and printable helpers carry per-terminal state", "[tui][input][issue950][spec]") {
    // Two terminals, two states, interleaved: no caller observes the other's
    // negotiated protocol and no process-global state carries between calls.
    const auto legacy_press = tui::parse_key("\r", false);
    const auto kitty_press = tui::parse_key("\r", true);
    REQUIRE(legacy_press);
    REQUIRE(kitty_press);
    CHECK(tui::key_id(*legacy_press) == "enter");
    CHECK(tui::key_id(*kitty_press) == "enter");

    const auto legacy_newline = tui::parse_key("\n", false);
    const auto kitty_newline = tui::parse_key("\n", true);
    REQUIRE(legacy_newline);
    REQUIRE(kitty_newline);
    CHECK(tui::key_id(*legacy_newline) == "enter");
    CHECK(tui::key_id(*kitty_newline) == "shift+enter");
    CHECK_FALSE(tui::printable_text(*legacy_newline).has_value());
    CHECK_FALSE(tui::printable_text(*kitty_newline).has_value());

    // The helper is usable on its own, without a decoder or a terminal.
    CHECK(tui::parse_key("\x1b[1092::97u", false)->text == "\xd1\x84");
    CHECK(tui::parse_key("not a key sequence", false) == std::nullopt);
}

TEST_CASE("Input inserts the Kitty character and ignores its release", "[tui][input][issue950][spec]") {
    tui::Input input;

    REQUIRE(input.handle_input(tui::InputEventVariant{*kitty("\x1b[1092::97u")}) ==
            tui::InputAdmissionOutcome::Consumed);
    REQUIRE(input.handle_input(tui::InputEventVariant{*kitty("\x1b[1092::97;2u")}) ==
            tui::InputAdmissionOutcome::Consumed);
    CHECK(input.value() == "\xd1\x84\xd1\x84");

    // The release identifies the 'a' shortcut but inserts nothing.
    CHECK(input.handle_input(tui::InputEventVariant{*kitty("\x1b[1092::97;3u")}) ==
            tui::InputAdmissionOutcome::Unhandled);
    CHECK(input.value() == "\xd1\x84\xd1\x84");
}

TEST_CASE("Editor inserts and jumps by the Kitty character, not by the shortcut identity",
        "[tui][editor][issue950][spec]") {
    tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    tui::Tui tui(terminal);
    auto editor = std::make_unique<tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));

    // Press and repeat insert the layout's character; the release does not.
    REQUIRE(terminal.inject_input("\x1b[1092::97u"));
    REQUIRE(terminal.inject_input("\x1b[1092::97;2u"));
    REQUIRE(terminal.inject_input("\x1b[1092::97;3u"));
    CHECK(editor_pointer->text() == "\xd1\x84\xd1\x84");

    // A shortcut still matches the base-layout key from the same event.
    editor_pointer->set_text("\xd1\x84x\xd1\x84");
    REQUIRE(terminal.inject_input("\x1b[H")); // move to the line start
    REQUIRE(editor_pointer->cursor().column == 0);
    REQUIRE(terminal.inject_input("\x1b[93;5u")); // ctrl+] arms the jump
    REQUIRE(terminal.inject_input("\x1b[1092::97u"));
    // The jump lands on the typed character. Jumping by the shortcut identity
    // ('a') would find no match and leave the cursor where it started.
    CHECK(editor_pointer->cursor().column == 2);
    CHECK(editor_pointer->text() == "\xd1\x84x\xd1\x84");
}
