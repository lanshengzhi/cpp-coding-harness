#include <cch/tui/Input.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <array>
#include <string_view>

TEST_CASE("canonical baseline key identifiers parse with normalized modifiers", "[tui][input][issue47]") {
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

TEST_CASE("baseline key vocabulary accepts letters digits symbols and special names", "[tui][input][issue47]") {
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

TEST_CASE("invalid canonical key identifiers fail without partial matches", "[tui][input][issue47]") {
    CHECK_FALSE(cch::tui::parse_key_id(""));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+ctrl+c"));
    CHECK_FALSE(cch::tui::parse_key_id("super+c"));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+unknown"));
    CHECK_FALSE(cch::tui::parse_key_id("ctrl+"));

    const cch::tui::KeyEvent ctrl_c{.key = "c", .ctrl = true};
    CHECK_FALSE(cch::tui::matches_key(ctrl_c, "c"));
    CHECK_FALSE(cch::tui::matches_key(ctrl_c, "ctrl+d"));
}
