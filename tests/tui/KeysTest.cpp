#include <cch/tui/Keys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

TEST_CASE("canonical baseline key identifiers parse with normalized modifiers", "[tui][input][issue47][spec]") {
    const auto parsed = cch::tui::parse_key_id("alt+ctrl+shift+enter");

    REQUIRE(parsed);
    CHECK(parsed->key == "enter");
    CHECK(parsed->ctrl);
    CHECK(parsed->shift);
    CHECK(parsed->alt);
    CHECK(parsed->type == cch::tui::KeyEventType::Press);
    CHECK(cch::tui::key_id(*parsed) == "shift+ctrl+alt+enter");
    CHECK(cch::tui::matches_key(*parsed, "ctrl+shift+alt+return"));
}

TEST_CASE("baseline key vocabulary accepts letters digits symbols and special names", "[tui][input][issue47][spec]") {
    constexpr std::array<std::string_view, 28> kSpecialKeys{
        "escape", "esc", "enter", "return", "tab", "space", "backspace", "delete", "insert", "clear",
        "home", "end", "pageUp", "pageDown", "up", "down", "left", "right", "f1", "f2", "f3", "f4",
        "f5", "f6", "f7", "f8", "f9", "f12",
    };
    for (const auto key : kSpecialKeys) CHECK(cch::tui::parse_key_id(key));
    for (char key = 'a'; key <= 'z'; ++key) CHECK(cch::tui::parse_key_id(std::string_view(&key, 1)));
    for (char key = '0'; key <= '9'; ++key) CHECK(cch::tui::parse_key_id(std::string_view(&key, 1)));
    constexpr std::string_view kSymbols = "`-=[]\\;',./!@#$%^&*()_+|~{}:<>?";
    for (const auto key : kSymbols) CHECK(cch::tui::parse_key_id(std::string_view(&key, 1)));
}

TEST_CASE("Super key identifiers parse format and match all modifier combinations", "[tui][input][issue888][spec]") {
    const auto parsed = cch::tui::parse_key_id("ctrl+super+c");

    REQUIRE(parsed);
    CHECK(parsed->ctrl);
    CHECK(parsed->super);
    CHECK_FALSE(parsed->shift);
    CHECK_FALSE(parsed->alt);
    CHECK(cch::tui::key_id(*parsed) == "ctrl+super+c");
    CHECK(cch::tui::matches_key(*parsed, "super+ctrl+c"));
    CHECK_FALSE(cch::tui::matches_key(*parsed, "ctrl+c"));
    CHECK(*parsed == cch::tui::KeyEvent{.key = "c", .ctrl = true, .super = true});
}

TEST_CASE("invalid canonical key identifiers fail without partial matches", "[tui][input][issue47][spec]") {
    CHECK_FALSE(cch::tui::parse_key_id(""));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+ctrl+c"));
    CHECK(cch::tui::parse_key_id("super+c"));
    CHECK_FALSE(cch::tui::parse_key_id("super+super+c"));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+unknown"));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+"));

    const cch::tui::KeyEvent ctrl_c{.key = "c", .ctrl = true};
    CHECK_FALSE(cch::tui::matches_key(ctrl_c, "c"));
    CHECK_FALSE(cch::tui::matches_key(ctrl_c, "ctrl+d"));
}

TEST_CASE("raw sequence is diagnostic and excluded from semantic key identity", "[tui][input][issue962][spec]") {
    // Two encodings of the same key differ only in the raw bytes the terminal
    // used: a legacy byte and a Kitty CSI-u payload both mean ctrl+c.
    const auto legacy = cch::tui::parse_key("\x03", false);
    const auto kitty = cch::tui::parse_key("\x1b[99;5u", true);
    REQUIRE(legacy);
    REQUIRE(kitty);
    CHECK(*legacy == *kitty);
    CHECK(legacy->raw_sequence == "\x03");
    CHECK(kitty->raw_sequence == "\x1b[99;5u");
    CHECK(legacy->raw_sequence != kitty->raw_sequence);
    // The shortcut identity the raw sequence reports is unaffected.
    CHECK(cch::tui::key_id(*legacy) == "ctrl+c");
    CHECK(cch::tui::matches_key(*kitty, "ctrl+c"));

    // An identifier-parsed event carries no raw sequence and still matches
    // the same semantic identity.
    const auto from_identifier = cch::tui::parse_key_id("ctrl+c");
    REQUIRE(from_identifier);
    CHECK(from_identifier->raw_sequence.empty());
    CHECK(*from_identifier == *legacy);

    // The raw LF the raw-sequence bypass submits on is recorded verbatim.
    const auto raw_lf = cch::tui::parse_key("\n", false);
    REQUIRE(raw_lf);
    CHECK(raw_lf->raw_sequence == "\n");

    // A modifyOtherKeys encoding of the same key is again semantically equal.
    const auto modify_other_keys = cch::tui::parse_key("\x1b[27;5;99~", false);
    REQUIRE(modify_other_keys);
    CHECK(*modify_other_keys == *legacy);
    CHECK(modify_other_keys->raw_sequence == "\x1b[27;5;99~");
}

TEST_CASE("shifted-symbol reports consume the implicit shift and keep the physical spelling",
        "[tui][input][issue1122][spec]") {
    // Frozen pi-v1.3.0 observation: fixtures/pi-v1.3.0/audit-1050/keys-and-terminal-replies.json,
    // scenario shifted-symbol-keys-match-produced-character (revision 71023aec).
    // A Kitty shifted "=" report names the produced "+" and consumes the implicit
    // Shift; the physical "shift+=" spelling stays matchable, and ctrl+shift+= or
    // modifyOtherKeys ctrl+plus is the ctrl++ identity rather than "+".
    struct ShiftedSymbolCase {
        std::string_view report;
        std::string_view identity;
        std::optional<std::string> text;
        bool matches_plus;
        bool matches_shift_equals;
        bool matches_ctrl_plus;
        bool matches_ctrl_slash;
    };
    const ShiftedSymbolCase kCases[]{
            {"\x1b[61:43;2u", "+", "+", true, true, false, false},
            {"\x1b[61:43;6u", "ctrl++", std::nullopt, false, false, true, false},
            {"\x1b[27;6;43~", "ctrl++", std::nullopt, false, false, true, false},
            {"\x1b[27;5;47~", "ctrl+/", std::nullopt, false, false, false, true},
    };

    for (const auto& test : kCases) {
        INFO(std::string(test.report));
        const auto event = cch::tui::parse_key(test.report, /*kitty_protocol_active=*/true);
        REQUIRE(event);
        CHECK(cch::tui::key_id(*event) == test.identity);
        CHECK(cch::tui::matches_key(*event, "+") == test.matches_plus);
        CHECK(cch::tui::matches_key(*event, "shift+=") == test.matches_shift_equals);
        CHECK(cch::tui::matches_key(*event, "ctrl++") == test.matches_ctrl_plus);
        CHECK(cch::tui::matches_key(*event, "ctrl+/") == test.matches_ctrl_slash);
        // pi-v1.3.0 `decodeKittyPrintable` prefers the reported shifted key
        // for insertion text; a ctrl shortcut never carries text.
        CHECK(event->text == test.text.value_or(std::string{}));
        CHECK(cch::tui::printable_text(*event) == test.text);
    }

    // A plain legacy "+" is the produced symbol with no shift report behind it,
    // so it must not satisfy the physical "shift+=" binding.
    const auto plain = cch::tui::parse_key("+", false);
    REQUIRE(plain);
    CHECK(cch::tui::key_id(*plain) == "+");
    CHECK(cch::tui::matches_key(*plain, "+"));
    CHECK_FALSE(cch::tui::matches_key(*plain, "shift+="));
    CHECK_FALSE(cch::tui::matches_key(*plain, "ctrl++"));
}

TEST_CASE("keypad alternate shifted-symbol reports normalize before matching and printing",
        "[tui][input][issue1122][spec]") {
    // Frozen pi-v1.3.0 public calls (revision 71023aec, checkout
    // /tmp/cch-1050-pi-frozen): `getLogicalShiftedSymbolCandidate` normalizes
    // Kitty functional/keypad codepoints before testing the symbol set, and
    // `decodeKittyPrintable` normalizes the effective insertion codepoint.
    // Observed: `ESC[61:57413;2u` -> parseKey "+", matches "+"/"shift+=",
    // decode "+"; the ctrl form -> "ctrl++" with no printable text; plain
    // KP_ADD -> "+"; modifyOtherKeys shift+KP_ADD -> "+".
    const auto shifted = cch::tui::parse_key("\x1b[61:57413;2u", /*kitty_protocol_active=*/true);
    REQUIRE(shifted);
    CHECK(cch::tui::key_id(*shifted) == "+");
    CHECK(shifted->text == "+");
    CHECK(cch::tui::printable_text(*shifted) == std::optional<std::string>{"+"});
    CHECK(cch::tui::matches_key(*shifted, "+"));
    CHECK(cch::tui::matches_key(*shifted, "shift+="));
    CHECK_FALSE(cch::tui::matches_key(*shifted, "ctrl++"));

    const auto ctrl_shifted = cch::tui::parse_key("\x1b[61:57413;6u", /*kitty_protocol_active=*/true);
    REQUIRE(ctrl_shifted);
    CHECK(cch::tui::key_id(*ctrl_shifted) == "ctrl++");
    CHECK(ctrl_shifted->text.empty());
    CHECK_FALSE(cch::tui::printable_text(*ctrl_shifted));
    CHECK(cch::tui::matches_key(*ctrl_shifted, "ctrl++"));

    const auto plain_keypad = cch::tui::parse_key("\x1b[57413u", /*kitty_protocol_active=*/true);
    REQUIRE(plain_keypad);
    CHECK(cch::tui::key_id(*plain_keypad) == "+");
    CHECK(plain_keypad->text == "+");
    CHECK_FALSE(cch::tui::matches_key(*plain_keypad, "shift+="));

    const auto modify_other_keys = cch::tui::parse_key("\x1b[27;2;57413~", /*kitty_protocol_active=*/true);
    REQUIRE(modify_other_keys);
    CHECK(cch::tui::key_id(*modify_other_keys) == "+");
    CHECK(cch::tui::matches_key(*modify_other_keys, "+"));
    CHECK_FALSE(cch::tui::matches_key(*modify_other_keys, "shift+="));
}
