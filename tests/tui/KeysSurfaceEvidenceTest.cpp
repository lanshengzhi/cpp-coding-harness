#include <cch/tui/Keys.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] bool is_kitty_csi_u(std::string_view sequence) {
    return sequence.size() >= 3 && sequence.starts_with("\x1b[") && sequence.ends_with('u');
}

[[nodiscard]] std::optional<std::string> optional_text(const cch::support::JsonValue& value) {
    if (value.holds<std::nullptr_t>()) return std::nullopt;
    return value.get_string();
}

} // namespace

TEST_CASE("pi-v1.3.0 key constants and modifier helpers replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1221-key-surface", "key-constants.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "key-constants-and-modifier-helpers");
    const auto& expected = scenario.at("expected");
    // pi's `Key` table is the shortcut vocabulary. Every base spelling must
    // parse to a matchable identity; aliases (`esc`/`return`) normalize, so the
    // contract is that the frozen spelling parses and matches itself.
    for (const auto& [name, value] : expected.at("constants").get_object()) {
        INFO(name);
        const auto parsed = cch::tui::parse_key_id(value.get_string());
        REQUIRE(parsed);
        CHECK(cch::tui::matches_key(*parsed, value.get_string()));
    }
    // The modifier helpers are the same vocabulary with a modifier prefix; the
    // helper's own modifier order is not significant.
    for (const auto& [name, value] : expected.at("helpers").get_object()) {
        INFO(name);
        const auto parsed = cch::tui::parse_key_id(value.get_string());
        REQUIRE(parsed);
        CHECK(cch::tui::matches_key(*parsed, value.get_string()));
    }
}

TEST_CASE("pi-v1.3.0 kitty printable text decoding replays", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1221-key-surface", "kitty-text.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "kitty-printable-text-decoding");
    const auto& cases = scenario.at("inputs").at("cases").get_array();
    const auto& expected = scenario.at("expected").at("cases").get_array();
    REQUIRE(cases.size() == expected.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& name = cases[i].at("name").get_string();
        const auto& sequence = cases[i].at("sequence").get_string();
        INFO(name);
        REQUIRE(expected[i].at("name").get_string() == name);
        const auto event = cch::tui::parse_key(sequence, /*kitty_protocol_active=*/true);
        REQUIRE(event);
        const auto frozen = optional_text(expected[i].at("text"));
        if (is_kitty_csi_u(sequence)) {
            // A CSI-u report takes the Kitty printable decoder: its text is
            // the frozen `decodeKittyPrintable` result, including the release
            // case, which pi's decoder does not filter by event type.
            CHECK(event->text == frozen.value_or(std::string{}));
        } else {
            // The frozen decoder rejects a non-CSI-u sequence; the public parse
            // seam decodes those same bytes through its legacy path, whose text
            // is the literal input. The captured null binds the reject class.
            CHECK_FALSE(frozen.has_value());
            CHECK(event->raw_sequence == sequence);
        }
    }
}

TEST_CASE("pi-v1.3.0 key event types replay the registered identity observation", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1221", "keys-kitty-text.json");
    REQUIRE(bundle);
    for (const auto name : {"legacy-key-identity", "kitty-active-key-identity"}) {
        const auto& scenario = cch::tests::find_scenario(*bundle, name);
        const auto& input = scenario.at("inputs");
        const auto& cases = input.at("cases").get_array();
        const auto& expected = scenario.at("expected").get_array();
        REQUIRE(cases.size() == expected.size());
        for (std::size_t i = 0; i < cases.size(); ++i) {
            INFO(name << " case " << i);
            const auto event =
                    cch::tui::parse_key(cases[i].at("sequence").get_string(), input.at("active").get_boolean());
            REQUIRE(event);
            CHECK((event->type == cch::tui::KeyEventType::Repeat) == expected[i].at("repeat").get_boolean());
            CHECK((event->type == cch::tui::KeyEventType::Release) == expected[i].at("release").get_boolean());
        }
    }
}

TEST_CASE("pi-v1.3.0 key release and repeat raw predicates replay", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1221-key-surface", "release-repeat.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "key-release-and-repeat-raw-predicates");
    const auto& cases = scenario.at("inputs").at("cases").get_array();
    const auto& expected = scenario.at("expected").at("cases").get_array();
    REQUIRE(cases.size() == expected.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& name = cases[i].at("name").get_string();
        const auto& data = cases[i].at("data").get_string();
        INFO(name);
        REQUIRE(expected[i].at("name").get_string() == name);
        CHECK(cch::tui::is_key_release(data) == expected[i].at("release").get_boolean());
        CHECK(cch::tui::is_key_repeat(data) == expected[i].at("repeat").get_boolean());
    }
}

TEST_CASE("pi-v1.3.0 shifted-symbol key matching replays the registered observation", "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "keys-and-terminal-replies.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "shifted-symbol-keys-match-produced-character");
    const auto& sequences = scenario.at("inputs").at("sequences");
    const auto& observations = scenario.at("expected").at("observations");
    for (const auto& [name, observation] : observations.get_object()) {
        INFO(name);
        const auto event = cch::tui::parse_key(sequences.at(name).get_string(), /*kitty_protocol_active=*/true);
        REQUIRE(event);
        CHECK(cch::tui::key_id(*event) == observation.at("parseKey").get_string());
        CHECK(cch::tui::matches_key(*event, "+") == observation.at("matchesPlus").get_boolean());
        CHECK(cch::tui::matches_key(*event, "shift+=") == observation.at("matchesShiftEquals").get_boolean());
        CHECK(cch::tui::matches_key(*event, "ctrl++") == observation.at("matchesCtrlPlus").get_boolean());
        CHECK(cch::tui::matches_key(*event, "ctrl+/") == observation.at("matchesCtrlSlash").get_boolean());
    }
    const auto plain = cch::tui::parse_key(scenario.at("inputs").at("plainPlus").get_string(), false);
    REQUIRE(plain);
    CHECK(cch::tui::key_id(*plain) == scenario.at("expected").at("plainPlus").get_string());
    CHECK(cch::tui::matches_key(*plain, "+") == scenario.at("expected").at("plainPlusMatches").get_boolean());
}

TEST_CASE("pi-v1.3.0 an invalid identifier cannot match a produced symbol", "[tui][issue1221][compat-pi]") {
    // The frozen produced-symbol observation binds "+" as the identity of a
    // shifted "=" report. A resolver that accepts arbitrary key names or
    // matches by suffix would still answer true here.
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+unknown"));
    CHECK_FALSE(cch::tui::parse_key_id("not+a+key"));
    const auto plus = cch::tui::parse_key("\x1b[61:43;2u", /*kitty_protocol_active=*/true);
    REQUIRE(plus);
    CHECK_FALSE(cch::tui::matches_key(*plus, "ctrl+unknown"));
    CHECK_FALSE(cch::tui::matches_key(*plus, "shift+unknown"));
}

TEST_CASE(
        "pi-v1.3.0 a bracketed paste containing a release suffix is not a key release", "[tui][issue1221][compat-pi]") {
    // A Bluetooth address inside paste content contains ":3F"; the paste
    // exclusion must win over the suffix matrix.
    CHECK_FALSE(cch::tui::is_key_release("\x1b[200~90:62:3F:A5\x1b[201~"));
    CHECK(cch::tui::is_key_release("\x1b[1;1:3A"));
}

TEST_CASE("pi-v1.3.0 a bracketed paste containing a repeat suffix is not a key repeat", "[tui][issue1221][compat-pi]") {
    CHECK_FALSE(cch::tui::is_key_repeat("\x1b[200~:2F\x1b[201~"));
    CHECK(cch::tui::is_key_repeat("\x1b[1;1:2F"));
}

TEST_CASE("pi-v1.3.0 a ctrl kitty report produces no printable text", "[tui][issue1221][compat-pi]") {
    // The frozen decoder rejects ctrl/alt/super reports; a decoder that
    // inserts the reported codepoint regardless of modifiers fails here while
    // the plain report still has to produce text.
    const auto ctrl = cch::tui::parse_key("\x1b[97;5u", /*kitty_protocol_active=*/true);
    REQUIRE(ctrl);
    CHECK(ctrl->text.empty());
    CHECK_FALSE(cch::tui::printable_text(*ctrl));
    const auto plain = cch::tui::parse_key("\x1b[97u", /*kitty_protocol_active=*/true);
    REQUIRE(plain);
    CHECK(cch::tui::printable_text(*plain) == std::optional<std::string>{"a"});
}

TEST_CASE("pi-v1.3.0 a plain produced symbol does not satisfy the physical shift binding",
        "[tui][issue1221][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1050", "keys-and-terminal-replies.json");
    REQUIRE(bundle);
    const auto& scenario = cch::tests::find_scenario(*bundle, "shifted-symbol-keys-match-produced-character");
    const auto plain = cch::tui::parse_key(scenario.at("inputs").at("plainPlus").get_string(), false);
    REQUIRE(plain);
    CHECK(cch::tui::matches_key(*plain, "+"));
    CHECK_FALSE(cch::tui::matches_key(*plain, "shift+="));
}

TEST_CASE(
        "pi-v1.3.0 a raw LF keeps its diagnostic sequence but not a modifier identity", "[tui][issue1221][compat-pi]") {
    // LF is byte 10 (ctrl+j); a legacy decoder that maps the control byte
    // would invent a ctrl identity and lose the raw diagnostic. The frozen
    // pi-v1.3.0 mode-aware legacy path reports enter (and shift+enter while
    // the Kitty protocol is active).
    const auto legacy = cch::tui::parse_key("\n", /*kitty_protocol_active=*/false);
    REQUIRE(legacy);
    CHECK(legacy->raw_sequence == "\n");
    CHECK(cch::tui::key_id(*legacy) == "enter");
    CHECK_FALSE(legacy->ctrl);
    CHECK_FALSE(legacy->shift);
    CHECK_FALSE(legacy->alt);
    const auto kitty = cch::tui::parse_key("\n", /*kitty_protocol_active=*/true);
    REQUIRE(kitty);
    CHECK(kitty->raw_sequence == "\n");
    CHECK(cch::tui::key_id(*kitty) == "shift+enter");
}
