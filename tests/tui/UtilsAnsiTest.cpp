// #957: frozen pi-v1.0.4 ANSI/OSC 8 boundary order for the public wrapping,
// slicing, truncation and link-lookup utilities.
//
// Every expected value below is read from the digest-verified named bundle
// (`fixtures/pi-tui/bundles/pi-v1.0.4/utils-ansi.json`, captured from
// pi-v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df through
// `fixtures/pi-tui/capture/capture-named-tui.mts`), never from Pike. The
// hand-written cases restate the same bytes where they must fail a cheap
// stand-in: an implementation that renders the right cells while emitting a
// control in the wrong position, closing a link it never kept, or padding
// outside an open span does not pass them.

#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kBelLinkOpen{"\x1b]8;;u\x07"};
constexpr std::string_view kBelLinkClose{"\x1b]8;;\x07"};
constexpr std::string_view kStLinkOpen{"\x1b]8;;u\x1b\\"};
constexpr std::string_view kStLinkClose{"\x1b]8;;\x1b\\"};

/// The one verified `ansi-boundary-order-and-hyperlinks` scenario of the
/// `utils-ansi` family.
[[nodiscard]] support::Expected<support::JsonValue> ansi_evidence_scenario() {
    auto fixture = tests::read_pi_tui_evidence("utils-ansi.json");
    if (!fixture) return std::unexpected(fixture.error());
    return support::JsonValue{fixture->at("scenarios").get_array().front()};
}

/// The row pi reopens with the codes its own tokenizer still considered part of
/// the completed token. The toolkit writes the equivalent cells without the
/// redundant reopen/close pair; the cells are asserted instead of the bytes.
constexpr std::string_view kCosmeticReopenCase{"link-cjk-styled-span"};

} // namespace

TEST_CASE("style_text composes colors and attributes around ANSI and hyperlink content",
        "[tui][colors][issue988][spec]") {
    // Independently evaluated with pi-v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df:
    // styleText emits colors first, attributes in declaration order, preserves the content bytes,
    // then closes attributes and colors in reverse order. A cell-equivalent renderer would miss
    // both the existing red SGR inside the style and the OSC 8 link's exact location.
    const cch::tui::TextStyle style{
            .foreground = cch::tui::Color{cch::tui::RgbColor{.red = 18, .green = 52, .blue = 86}},
            .background = cch::tui::Color{cch::tui::IndexedColor{.index = 9}},
            .bold = true,
            .dim = true,
            .italic = true,
            .underline = true,
            .inverse = true,
            .strikethrough = true,
    };
    CHECK(cch::tui::style_text("\x1b[31mR\x1b[39m", style, cch::tui::TerminalColorMode::TrueColor) ==
            "\x1b[38;2;18;52;86m\x1b[48;5;9m\x1b[1m\x1b[2m\x1b[3m\x1b[4m\x1b[7m\x1b[9m"
            "\x1b[31mR\x1b[39m\x1b[29m\x1b[27m\x1b[24m\x1b[23m\x1b[22m\x1b[49m\x1b[39m");

    const std::string linked = std::string{kBelLinkOpen} + "x" + std::string{kBelLinkClose};
    const cch::tui::TextStyle rgb_style{
            .foreground = cch::tui::Color{cch::tui::RgbColor{.red = 12.5, .green = 34.5, .blue = 56.5}},
            .background = cch::tui::Color{cch::tui::IndexedColor{.index = 42}},
            .bold = true,
            .italic = true,
    };
    CHECK(cch::tui::style_text(linked, rgb_style, cch::tui::TerminalColorMode::TrueColor) ==
            "\x1b[38;2;13;35;57m\x1b[48;5;42m\x1b[1m\x1b[3m" + linked + "\x1b[23m\x1b[22m\x1b[49m\x1b[39m");
}

TEST_CASE("style_text leaves unspecified colors untouched and resets selected attributes",
        "[tui][colors][issue988][spec]") {
    const std::string linked = std::string{kBelLinkOpen} + "x" + std::string{kBelLinkClose};
    CHECK(cch::tui::style_text(linked, {}, cch::tui::TerminalColorMode::TrueColor) == linked);

    const cch::tui::TextStyle attributes{
            .foreground = std::nullopt, .background = std::nullopt, .bold = true, .dim = true, .underline = true};
    CHECK(cch::tui::style_text(linked, attributes, cch::tui::TerminalColorMode::Xterm256) ==
            "\x1b[1m\x1b[2m\x1b[4m" + linked + "\x1b[24m\x1b[22m");
}

TEST_CASE("wrap_text reproduces the frozen wrap boundary order", "[tui][issue957][ansi][wrap][spec]") {
    const auto scenario = ansi_evidence_scenario();
    REQUIRE(scenario);
    const auto& rows = scenario->at("expected").at("wrap").get_array();
    REQUIRE(rows.size() >= 6);
    std::size_t compared = 0;
    for (const auto& row : rows) {
        const auto name = row.at("name").get_string();
        if (name == kCosmeticReopenCase) continue;
        INFO("wrap case " << name);
        const auto wrapped =
                tui::wrap_text(row.at("input").get_string(), static_cast<std::size_t>(row.at("width").get_number()));
        REQUIRE(wrapped);
        const auto& expected = row.at("output").get_array();
        REQUIRE(wrapped->size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK((*wrapped)[index] == expected[index].get_string());
        }
        ++compared;
    }
    REQUIRE(compared == rows.size() - 1);
}

TEST_CASE("wrap_text leaves a staged control off the row a wrap break pushes", "[tui][issue957][ansi][wrap][spec]") {
    // pi holds a control pending until the following grapheme adopts it, so the
    // row pushed before the long token carries no trailing code. A stand-in
    // that renders the same cells but leaves the code on the pushed row writes
    // "中文\x1b[31m" here and fails.
    const auto staged = tui::wrap_text("中文\x1b[31mABCDEFGHIJ", 4);
    REQUIRE(staged);
    const std::vector<std::string> expected{"中文", "\x1b[31mABCD", "\x1b[31mEFGH", "\x1b[31mIJ"};
    CHECK(*staged == expected);

    // An underline is closed at the break it causes, and a hyperlink is closed
    // with the terminator it was opened with.
    const auto underlined = tui::wrap_text("中文\x1b[4mABCDEFGH", 4);
    REQUIRE(underlined);
    const std::vector<std::string> expected_underlined{"中文", "\x1b[4mABCD\x1b[24m", "\x1b[4mEFGH"};
    CHECK(*underlined == expected_underlined);

    const auto linked = tui::wrap_text("中文" + std::string{kBelLinkOpen} + "ABCDEFGH", 4);
    REQUIRE(linked);
    const std::vector<std::string> expected_linked{
            "中文",
            std::string{kBelLinkOpen} + "ABCD" + std::string{kBelLinkClose},
            std::string{kBelLinkOpen} + "EFGH",
    };
    CHECK(*linked == expected_linked);

    const auto st_linked = tui::wrap_text("中文" + std::string{kStLinkOpen} + "ABCDEFGH", 4);
    REQUIRE(st_linked);
    const std::vector<std::string> expected_st_linked{
            "中文",
            std::string{kStLinkOpen} + "ABCD" + std::string{kStLinkClose},
            std::string{kStLinkOpen} + "EFGH",
    };
    CHECK(*st_linked == expected_st_linked);

    // A code no visible grapheme follows stays on the line it closed, including
    // across a logical newline.
    const auto trailing = tui::wrap_text("abc\x1b[31m\ndef", 20);
    REQUIRE(trailing);
    const std::vector<std::string> expected_trailing{"abc\x1b[31m", "\x1b[31mdef"};
    CHECK(*trailing == expected_trailing);
}

TEST_CASE("slice_by_column reproduces the frozen slice-start order and link spans",
        "[tui][issue957][ansi][slice][spec]") {
    const auto scenario = ansi_evidence_scenario();
    REQUIRE(scenario);
    const auto& rows = scenario->at("expected").at("slice").get_array();
    REQUIRE(rows.size() >= 3);
    for (const auto& row : rows) {
        const auto name = row.at("name").get_string();
        INFO("slice case " << name);
        const auto sliced = tui::slice_by_column(row.at("input").get_string(),
                static_cast<std::size_t>(row.at("start").get_number()),
                static_cast<std::size_t>(row.at("length").get_number()),
                row.at("strict").get_boolean());
        REQUIRE(sliced);
        CHECK(*sliced == row.at("output").get_string());
    }
}

TEST_CASE("slice_by_column keeps the v1.0.0 slice-start ordering fix", "[tui][issue957][ansi][slice][spec]") {
    // The pre-existing fix stays: codes from before the range precede the codes
    // at the boundary instead of being replaced by them.
    const auto reset = tui::slice_by_column("\x1b[32mfoo\x1b[39m bar", 3, 4, true);
    REQUIRE(reset);
    CHECK(*reset == "\x1b[32m\x1b[39m bar");

    const auto style = tui::slice_by_column("Another \x1b[35malpha\x1b[39m line with more text", 13, 100, true);
    REQUIRE(style);
    CHECK(*style == "\x1b[35m\x1b[39m line with more text");

    // A slice starting inside an open hyperlink keeps the link open for the
    // cells it covers, including a wide grapheme.
    const auto linked = tui::slice_by_column(std::string{kBelLinkOpen} + "a你bcd" + std::string{kBelLinkClose}, 1, 3);
    REQUIRE(linked);
    CHECK(*linked == std::string{kBelLinkOpen} + "你b");
}

TEST_CASE("truncate_text matches the frozen padding, pending-control and terminator order",
        "[tui][issue957][ansi][truncate][spec]") {
    const auto scenario = ansi_evidence_scenario();
    REQUIRE(scenario);
    const auto& rows = scenario->at("expected").at("truncate").get_array();
    REQUIRE(rows.size() >= 7);
    for (const auto& row : rows) {
        const auto name = row.at("name").get_string();
        INFO("truncate case " << name);
        const auto truncated = tui::truncate_text(row.at("input").get_string(),
                static_cast<std::size_t>(row.at("width").get_number()),
                row.at("ellipsis").get_string(),
                row.at("pad").get_boolean());
        REQUIRE(truncated);
        CHECK(*truncated == row.at("output").get_string());
    }
}

TEST_CASE("truncate_text pads inside the span a fitting line leaves open", "[tui][issue957][ansi][truncate][spec]") {
    // pi returns a fitting line unchanged and pads inside its open span; closing
    // underline or the hyperlink first changes the bytes and moves the padding
    // out of the styled run.
    const auto underlined = tui::truncate_text("\x1b[4mabc", 8, "", true);
    REQUIRE(underlined);
    CHECK(*underlined == "\x1b[4mabc     ");

    const auto styled = tui::truncate_text("\x1b[1;4mabc", 8, "...", true);
    REQUIRE(styled);
    CHECK(*styled == "\x1b[1;4mabc     ");

    const auto linked = tui::truncate_text(std::string{kBelLinkOpen} + "abc", 8, "...", true);
    REQUIRE(linked);
    CHECK(*linked == std::string{kBelLinkOpen} + "abc     ");
}

TEST_CASE("truncate_text drops a control that only styles dropped text", "[tui][issue957][ansi][truncate][spec]") {
    // pi holds a control pending until a kept grapheme adopts it. A stand-in
    // that keeps the control, or closes the link it never kept, still renders
    // the same cells and fails these bytes.
    const auto pending_sgr = tui::truncate_text("\x1b[4ma\x1b[31mbcdef", 4);
    REQUIRE(pending_sgr);
    CHECK(*pending_sgr == "\x1b[4ma\x1b[0m...\x1b[0m");

    const auto pending_link = tui::truncate_text("\x1b[4ma" + std::string{kBelLinkOpen} + "bcdef", 4);
    REQUIRE(pending_link);
    CHECK(*pending_link == "\x1b[4ma\x1b[0m...\x1b[0m");

    // A link the kept prefix leaves open is closed before the always-on reset,
    // with the terminator it was opened with.
    const auto open_bel = tui::truncate_text(std::string{kBelLinkOpen} + "abcdefgh", 4);
    REQUIRE(open_bel);
    CHECK(*open_bel == std::string{kBelLinkOpen} + "a" + std::string{kBelLinkClose} + "\x1b[0m...\x1b[0m");

    const auto open_st = tui::truncate_text(std::string{kStLinkOpen} + "abcdefgh", 4);
    REQUIRE(open_st);
    CHECK(*open_st == std::string{kStLinkOpen} + "a" + std::string{kStLinkClose} + "\x1b[0m...\x1b[0m");
}

TEST_CASE("truncate_text emits only the part of the ellipsis that fits", "[tui][issue957][ansi][truncate][spec]") {
    const auto clipped = tui::truncate_text("abcdef", 2, "中");
    REQUIRE(clipped);
    CHECK(*clipped == "\x1b[0m中\x1b[0m");

    // Nothing of the ellipsis fits beside the text at all: pi emits no reset
    // around a missing glyph, and padding fills the width when requested.
    const auto empty = tui::truncate_text("abcdef", 1, "中");
    REQUIRE(empty);
    CHECK(empty->empty());

    const auto padded = tui::truncate_text("abcdef", 1, "中", true);
    REQUIRE(padded);
    CHECK(*padded == " ");

    const auto partial = tui::truncate_text("abcdef", 2, "abc");
    REQUIRE(partial);
    CHECK(*partial == "\x1b[0mab\x1b[0m");

    const auto wide = tui::truncate_text("abcdef", 1, "中中");
    REQUIRE(wide);
    CHECK(wide->empty());
}

TEST_CASE("osc8_link_at_column exposes the frozen link for every cell it covers", "[tui][issue957][hyperlink][spec]") {
    const auto scenario = ansi_evidence_scenario();
    REQUIRE(scenario);
    const auto& rows = scenario->at("expected").at("linkColumns").get_array();
    REQUIRE(rows.size() >= 4);
    for (const auto& row : rows) {
        const auto name = row.at("name").get_string();
        INFO("link column case " << name);
        const auto link = tui::osc8_link_at_column(
                row.at("input").get_string(), static_cast<std::size_t>(row.at("column").get_number()));
        if (row.at("link").holds<support::JsonValue::null_t>()) {
            CHECK_FALSE(link.has_value());
        } else {
            REQUIRE(link.has_value());
            CHECK(*link == row.at("link").get_string());
        }
    }

    const std::string linked{"a \x1b]8;;https://x\x07你b\x1b]8;;\x07 c"};
    CHECK(tui::osc8_link_at_column(linked, 0) == std::nullopt);
    CHECK(tui::osc8_link_at_column(linked, 2) == std::optional<std::string>{"https://x"});
    CHECK(tui::osc8_link_at_column(linked, 3) == std::optional<std::string>{"https://x"});
    CHECK(tui::osc8_link_at_column(linked, 4) == std::optional<std::string>{"https://x"});
    CHECK(tui::osc8_link_at_column(linked, 5) == std::nullopt);
    CHECK(tui::osc8_link_at_column(linked, 99) == std::nullopt);
}

TEST_CASE("wrapped and truncated lines keep the frozen cells, styles and links after composition",
        "[tui][issue957][hyperlink][ansi][composition][spec]") {
    const auto wrapped = tui::wrap_text("ab \x1b]8;;https://x\x07\x1b[31m中文\x1b[0m\x1b]8;;\x07 cd", 4);
    REQUIRE(wrapped);
    REQUIRE(wrapped->size() == 3);

    tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    for (std::size_t row = 0; row < wrapped->size(); ++row) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = row}));
        REQUIRE(terminal.write((*wrapped)[row]));
    }

    const auto& cells = terminal.cells();
    REQUIRE(cells.size() == 3);
    REQUIRE(cells[0].size() == 8);
    CHECK(cells[0][0].grapheme == "a");
    CHECK(cells[0][1].grapheme == "b");
    CHECK(cells[0][1].style.hyperlink.empty());
    // The CJK link covers both of its cells and carries the foreground too.
    REQUIRE(cells[1].size() == 8);
    CHECK(cells[1][0].grapheme == "中");
    CHECK(cells[1][0].style.hyperlink == "https://x");
    CHECK(cells[1][0].style.fg_color == "31");
    CHECK(cells[1][1].style.hyperlink == "https://x");
    CHECK(cells[1][1].style.fg_color == "31");
    // The row behind the styled span is outside the link and the foreground:
    // pi writes the reopened codes and closes them again within the row.
    // The whitespace separating "中文" and "cd" is consumed at the wrap break.
    CHECK(cells[2][0].grapheme == "c");
    CHECK(cells[2][0].style.hyperlink.empty());
    CHECK(cells[2][0].style.fg_color.empty());
    CHECK(cells[2][1].grapheme == "d");
    CHECK(terminal.final_style() == tui::TerminalStyle{});

    // The wrapped row reports the frozen link at each covered cell column.
    CHECK(tui::osc8_link_at_column((*wrapped)[1], 0) == std::optional<std::string>{"https://x"});
    CHECK(tui::osc8_link_at_column((*wrapped)[1], 1) == std::optional<std::string>{"https://x"});
    CHECK(tui::osc8_link_at_column((*wrapped)[0], 0) == std::nullopt);

    // A truncated hyperlink closes before the ellipsis, so the ellipsis cell
    // carries no link while the kept cells do.
    const auto truncated = tui::truncate_text("\x1b]8;;https://x\x07"
                                              "abcdefg\x1b]8;;\x07",
            6,
            ".");
    REQUIRE(truncated);
    tui::VirtualTerminal truncated_terminal({.columns = 6, .rows = 1});
    REQUIRE(truncated_terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    REQUIRE(truncated_terminal.write(*truncated));
    const auto& truncated_cells = truncated_terminal.cells();
    REQUIRE(truncated_cells.size() == 1);
    REQUIRE(truncated_cells[0].size() == 6);
    REQUIRE(truncated_cells[0][4].grapheme == "e");
    CHECK(truncated_cells[0][4].style.hyperlink == "https://x");
    CHECK(truncated_cells[0][5].grapheme == ".");
    CHECK(truncated_cells[0][5].style.hyperlink.empty());
    CHECK(truncated_cells[0][0].style.hyperlink == "https://x");
}

TEST_CASE("strip_terminal_sequences removes the OSC 8 and OSC title bytes pi removes", "[tui][issue957][ansi][spec]") {
    CHECK(tui::strip_terminal_sequences("\x1b]8;;https://x\x07link\x1b]8;;\x07") == "link");
    CHECK(tui::strip_terminal_sequences("\x1b]8;;https://x\x1b\\link\x1b]8;;\x1b\\") == "link");
    CHECK(tui::strip_terminal_sequences("\x1b]0;title\x07"
                                        "body") == "body");
}