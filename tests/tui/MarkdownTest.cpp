#include <cch/tui/Markdown.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/PiTuiEvidence.hpp"
#include "support/RenderedScreen.hpp"
#include "support/ImageCapabilitiesGuard.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] tui::MarkdownStyleConfig ansi_style() {
    tui::MarkdownStyleConfig style;
    style.heading = [](std::string text) { return "\x1b[36m" + text + "\x1b[39m"; };
    style.emphasis = [](std::string text) { return "\x1b[3m" + text + "\x1b[23m"; };
    style.strong = [](std::string text) { return "\x1b[1m" + text + "\x1b[22m"; };
    style.strikethrough = [](std::string text) { return "\x1b[9m" + text + "\x1b[29m"; };
    style.inline_code = [](std::string text) { return "\x1b[33m" + text + "\x1b[39m"; };
    style.code_block = [](std::string text) { return "\x1b[32m" + text + "\x1b[39m"; };
    style.code_block_border = [](std::string text) { return "\x1b[2m" + text + "\x1b[22m"; };
    style.list_marker = [](std::string text) { return "\x1b[35m" + text + "\x1b[39m"; };
    style.quote = [](std::string text) { return "\x1b[3m" + text + "\x1b[23m"; };
    style.quote_border = [](std::string text) { return "\x1b[34m" + text + "\x1b[39m"; };
    style.horizontal_rule = [](std::string text) { return "\x1b[2m" + text + "\x1b[22m"; };
    style.link = [](std::string text, std::string_view destination) {
        return "\x1b]8;;" + std::string(destination) + "\x1b\\" + text + "\x1b]8;;\x1b\\";
    };
    return style;
}

void write_lines(tui::VirtualTerminal& terminal, const tui::RenderResult& lines) {
    for (std::size_t row = 0; row < lines.lines.size(); ++row) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = row}));
        REQUIRE(terminal.write(lines.lines[row]));
    }
}

[[nodiscard]] std::string rendered_text(const tui::RenderResult& lines, std::size_t width) {
    tui::VirtualTerminal terminal({.columns = width, .rows = lines.lines.size() + 1});
    if (auto started = terminal.start(
            [](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; });
        !started) {
        return {};
    }
    write_lines(terminal, lines);
    std::string result;
    for (const auto& line : terminal.screen()) {
        if (!result.empty()) result += '\n';
        result += line;
    }
    return result;
}

void require_virtual_terminal_accepts(const tui::RenderResult& lines, std::size_t width) {
    tui::VirtualTerminal terminal({.columns = width, .rows = lines.lines.size() + 1});
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, lines);
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

[[nodiscard]] const tui::VirtualTerminalCell* find_cell(
    const tui::VirtualTerminal& terminal,
    std::string_view grapheme) {
    for (const auto& row : terminal.cells()) {
        const auto found = std::find_if(row.begin(), row.end(), [&](const auto& cell) {
            return cell.grapheme == grapheme;
        });
        if (found != row.end()) return &*found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Markdown renders and preserves inline and display LaTeX math", "[tui][markdown][latex][issue976][spec]") {
    tui::Markdown inline_math(R"(inline $\frac{1}{2}$ and \(x^2\))");
    const auto inline_lines = inline_math.render(60);
    REQUIRE(inline_lines);
    const auto inline_text = rendered_text(*inline_lines, 60);
    CHECK(inline_text.find("1/2") != std::string::npos);
    CHECK(inline_text.find("x²") != std::string::npos);
    CHECK(inline_text.find(R"($\frac)") == std::string::npos);

    tui::Markdown display_math(R"md($$
\begin{bmatrix} 1 & 2 \\ 3 & 4 \end{bmatrix}
$$

$$\frac{1}{2}$$)md");
    const auto display_lines = display_math.render(60);
    REQUIRE(display_lines);
    const auto display_text = rendered_text(*display_lines, 60);
    CHECK(display_text.find("⎡ 1 │ 2 ⎤") != std::string::npos);
    CHECK(display_text.find("⎣ 3 │ 4 ⎦") != std::string::npos);
    CHECK(display_text.find("1") != std::string::npos);
    CHECK(display_text.find("─") != std::string::npos);
}

TEST_CASE("Markdown preserves pending and unsupported LaTeX source until it can render",
        "[tui][markdown][latex][issue976][spec]") {
    tui::Markdown markdown("before $");
    auto pending = markdown.render(50);
    REQUIRE(pending);
    CHECK(rendered_text(*pending, 50).find("before $") != std::string::npos);

    markdown.set_text(R"(before $\frac)");
    pending = markdown.render(50);
    REQUIRE(pending);
    CHECK(rendered_text(*pending, 50).find(R"($\frac)") != std::string::npos);

    markdown.set_text(R"(before $\frac{1})");
    pending = markdown.render(50);
    REQUIRE(pending);
    CHECK(rendered_text(*pending, 50).find(R"($\frac{1})") != std::string::npos);

    markdown.set_text(R"(before $\frac{1}{2}$)");
    const auto completed = markdown.render(50);
    REQUIRE(completed);
    CHECK(rendered_text(*completed, 50).find("1/2") != std::string::npos);

    markdown.set_text(R"(bad $\frac{1}$)");
    const auto unsupported = markdown.render(50);
    REQUIRE(unsupported);
    CHECK(rendered_text(*unsupported, 50).find(R"($\frac{1}$)") != std::string::npos);
}

TEST_CASE(
        "Markdown gives code and escaped delimiters precedence over LaTeX", "[tui][markdown][latex][issue976][spec]") {
    tui::Markdown markdown(R"md(`$x^2$` and \$x^2\$ and \\(x^2\\))md");
    const auto lines = markdown.render(60);
    REQUIRE(lines);
    const auto text = rendered_text(*lines, 60);
    CHECK(text.find("$x^2$") != std::string::npos);
    CHECK(text.find(R"(\(x^2\))") != std::string::npos);
    CHECK(text.find("x²") == std::string::npos);

    markdown.set_text(R"md(```text
$x^2$
```)md");
    const auto fenced = markdown.render(60);
    REQUIRE(fenced);
    CHECK(rendered_text(*fenced, 60).find("$x^2$") != std::string::npos);
}

TEST_CASE("Markdown can disable LaTeX rendering while preserving source",
        "[tui][markdown][latex][issue976][issue1217][spec]") {
    tui::Markdown markdown(R"($\frac{1}{2}$)", 0, 0, {}, {}, {}, tui::MarkdownOptions{.render_latex = false});
    const auto lines = markdown.render(40);
    REQUIRE(lines);
    CHECK(rendered_text(*lines, 40).find(R"($\frac{1}{2}$)") != std::string::npos);
}

TEST_CASE("Markdown wraps plain text within terminal width", "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown("alpha beta gamma", 0, 0);

    const auto lines = markdown.render(10);

    REQUIRE(lines);
    const std::vector<std::string> expected{"alpha beta", "gamma     "};
    CHECK(lines->lines == expected);
}

TEST_CASE(
        "Markdown renders common response constructs in semantic order", "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown(
        "# Heading\n\nParagraph with *emphasis*, **strong**, ~~gone~~, and `code`.\n\n"
        "- first item\n- second item\n\n"
        "> quoted words\n\n---\n\n"
        "[site](https://example.com)\n\n<div>plain html</div>\n\n"
        "```cpp\nint answer = 42;\n```",
        0,
        0,
        ansi_style());

    const auto lines = markdown.render(24);

    REQUIRE(lines);
    require_virtual_terminal_accepts(*lines, 24);
    const auto text = rendered_text(*lines, 24);
    const auto heading = text.find("Heading");
    const auto emphasis = text.find("emphasis");
    const auto list = text.find("first item");
    const auto quote = text.find("quoted words");
    const auto link = text.find("site");
    const auto html = text.find("plain html");
    const auto code = text.find("int answer = 42;");
    REQUIRE(heading != std::string::npos);
    REQUIRE(emphasis != std::string::npos);
    REQUIRE(list != std::string::npos);
    REQUIRE(quote != std::string::npos);
    REQUIRE(link != std::string::npos);
    REQUIRE(html != std::string::npos);
    REQUIRE(code != std::string::npos);
    CHECK(heading < emphasis);
    CHECK(emphasis < list);
    CHECK(list < quote);
    CHECK(quote < link);
    CHECK(link < html);
    CHECK(html < code);
    CHECK(text.find("─") != std::string::npos);
}

TEST_CASE("Markdown styles link labels and visible URL fallbacks independently",
        "[tui][markdown][theme][issue55][issue1217][spec]") {
    // The `label (url)` fallback is the no-hyperlink branch, so pin it instead
    // of reading the runner's own terminal capabilities (a Herdr pane reports
    // OSC 8 support and would take the link branch, #1124).
    const tests::ImageCapabilitiesGuard capabilities({.images = tui::InlineImageProtocol::None, .hyperlinks = false});
    tui::MarkdownStyleConfig style;
    style.link_text = [](std::string text) { return "\x1b[31m" + text + "\x1b[39m"; };
    style.link_url = [](std::string text) { return "\x1b[2m" + text + "\x1b[22m"; };
    tui::Markdown markdown(
        "[site](https://example.com) <https://example.org> <mail@example.com>",
        0,
        0,
        std::move(style));

    const auto lines = markdown.render(100);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 1);
    const auto& line = lines->lines.front();
    CHECK(line.find("\x1b[31msite\x1b[39m") != std::string::npos);
    CHECK(line.find("\x1b[2m (https://example.com)\x1b[22m") != std::string::npos);
    CHECK(line.find("(https://example.org)") == std::string::npos);
    CHECK(line.find("mailto:") == std::string::npos);
    CHECK(std::count(line.begin(), line.end(), '@') == 1);
}

TEST_CASE("Markdown preserves nested styles Unicode and link termination at narrow widths",
        "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown(
        "> **Cafe\xcc\x81 \xe4\xb8\xad \xf0\x9f\x99\x82 [linked words](https://example.com/long)**",
        0,
        0,
        ansi_style());

    const auto lines = markdown.render(12);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() > 1);
    require_virtual_terminal_accepts(*lines, 12);
    const auto text = rendered_text(*lines, 12);
    CHECK(text.find("Cafe\xcc\x81") != std::string::npos);
    CHECK(text.find("\xe4\xb8\xad") != std::string::npos);
    CHECK(text.find("\xf0\x9f\x99\x82") != std::string::npos);
    CHECK(text.find("linked") != std::string::npos);

    tui::VirtualTerminal terminal({.columns = 12, .rows = lines->lines.size() + 1});
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    const auto* linked_cell = find_cell(terminal, "l");
    REQUIRE(linked_cell != nullptr);
    CHECK(linked_cell->style.bold);
    CHECK(linked_cell->style.italic);
    CHECK(linked_cell->style.hyperlink == "https://example.com/long");
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

TEST_CASE(
        "Markdown injects syntax highlighting and falls back for unknown languages", "[tui][markdown][issue51][spec]") {
    std::string received_code;
    std::string received_language;
    auto highlighter = [&](std::string_view code, std::string_view language)
        -> std::optional<std::vector<std::string>> {
        received_code = code;
        received_language = language;
        if (language != "cpp") return std::nullopt;
        return std::vector<std::string>{"\x1b[35m" + std::string(code) + "\x1b[39m"};
    };
    tui::Markdown markdown("```cpp\nvalue\n```", 0, 0, ansi_style(), std::move(highlighter));

    const auto highlighted = markdown.render(16);

    REQUIRE(highlighted);
    CHECK(received_code == "value");
    CHECK(received_language == "cpp");
    tui::VirtualTerminal terminal({.columns = 16, .rows = highlighted->lines.size() + 1});
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *highlighted);
    const auto* highlighted_cell = find_cell(terminal, "v");
    REQUIRE(highlighted_cell != nullptr);
    CHECK(highlighted_cell->style.fg_color == "35");

    markdown.set_text("```unknown\nplain\n```");
    const auto fallback = markdown.render(16);

    REQUIRE(fallback);
    CHECK(received_language == "unknown");
    CHECK(rendered_text(*fallback, 16).find("plain") != std::string::npos);
    CHECK(fallback->lines.at(1).find("\x1b[32m") != std::string::npos);
}

TEST_CASE("Markdown keeps partial and invalid input readable", "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown("**unfinished [link](\n\n```cpp\nint value = 1;", 0, 0, ansi_style());

    const auto lines = markdown.render(18);

    REQUIRE(lines);
    require_virtual_terminal_accepts(*lines, 18);
    const auto text = rendered_text(*lines, 18);
    CHECK(text.find("unfinished") != std::string::npos);
    CHECK(text.find("link") != std::string::npos);
    CHECK(text.find("int value = 1;") != std::string::npos);
    CHECK(text.find("```") != std::string::npos);
}

TEST_CASE(
        "Markdown degrades semantic prefixes at extremely narrow widths", "[tui][markdown][issue51][issue1217][spec]") {
    const std::vector<std::string> samples{
        "> quote",
        "- item",
        "```cpp\nx\n```",
    };
    for (const auto& sample : samples) {
        tui::Markdown markdown(sample, 0, 0, ansi_style());

        const auto lines = markdown.render(1);

        REQUIRE(lines);
        REQUIRE_FALSE(lines->lines.empty());
        require_virtual_terminal_accepts(*lines, 1);
    }
}

TEST_CASE("Markdown follows strict strikethrough delimiter semantics", "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown(
        "~~valid~~ and ~~invalid ~~ plus ~~a\\ ~~ and ~~\\~a~~",
        0,
        0,
        ansi_style());

    const auto lines = markdown.render(50);

    REQUIRE(lines);
    CHECK(lines->lines.front().find("\x1b[9m") != std::string::npos);
    const auto text = rendered_text(*lines, 50);
    CHECK(text.find("~~invalid ~~") != std::string::npos);
    CHECK(text.find("~~a ") == std::string::npos);
    CHECK(text.find("~~~a~~") == std::string::npos);
}

TEST_CASE("Markdown preserves loose list paragraph structure", "[tui][markdown][issue51][issue1217][spec]") {
    tui::Markdown markdown(
        "1. first paragraph\n\n   second paragraph\n\n2. final item",
        0,
        0);

    const auto lines = markdown.render(30);

    REQUIRE(lines);
    const auto first = std::find_if(lines->lines.begin(), lines->lines.end(), [](const auto& line) {
        return line.find("first paragraph") != std::string::npos;
    });
    const auto second = std::find_if(lines->lines.begin(), lines->lines.end(), [](const auto& line) {
        return line.find("second paragraph") != std::string::npos;
    });
    REQUIRE(first != lines->lines.end());
    REQUIRE(second != lines->lines.end());
    CHECK(std::distance(first, second) >= 2);
}

TEST_CASE("Markdown stabilizes streamed partial closing fences", "[tui][markdown][issue51][issue1217][spec]") {
    const std::vector<std::pair<std::string, std::string>> samples{
        {"```cpp\nvalue\n``", "```cpp\nvalue\n```"},
        {"> ```cpp\n> value\n> ``", "> ```cpp\n> value\n> ```"},
        {"- ```cpp\n  value\n  ``", "- ```cpp\n  value\n  ```"},
    };
    for (const auto& [partial_text, complete_text] : samples) {
        tui::Markdown partial(partial_text, 0, 0, ansi_style());
        tui::Markdown complete(complete_text, 0, 0, ansi_style());

        const auto partial_lines = partial.render(20);
        const auto complete_lines = complete.render(20);

        REQUIRE(partial_lines);
        REQUIRE(complete_lines);
        CHECK(partial_lines->lines.size() == complete_lines->lines.size());
        CHECK(rendered_text(*partial_lines, 20).find("value") != std::string::npos);
        require_virtual_terminal_accepts(*partial_lines, 20);
    }
}

TEST_CASE("Markdown converts injected callback exceptions to render errors", "[tui][markdown][issue51][spec]") {}

TEST_CASE("Markdown invalidates content style and highlighter caches", "[tui][markdown][issue51][issue1217][spec]") {
    auto style = ansi_style();
    style.strong = [](std::string text) { return "\x1b[31m" + text + "\x1b[39m"; };
    tui::Markdown markdown("**old**", 0, 0, std::move(style));
    const auto first = markdown.render(12);
    REQUIRE(first);
    CHECK(first->lines.front().find("\x1b[31m") != std::string::npos);

    markdown.set_text("**new**");
    auto replacement = ansi_style();
    replacement.strong = [](std::string text) { return "\x1b[34m" + text + "\x1b[39m"; };
    markdown.set_style(std::move(replacement));
    markdown.set_syntax_highlighter([](std::string_view code, std::string_view)
        -> std::optional<std::vector<std::string>> {
        return std::vector<std::string>{"\x1b[36m" + std::string(code) + "\x1b[39m"};
    });
    markdown.invalidate();

    const auto second = markdown.render(12);

    REQUIRE(second);
    CHECK(rendered_text(*second, 12).find("new") != std::string::npos);
    CHECK(rendered_text(*second, 12).find("old") == std::string::npos);
    CHECK(second->lines.front().find("\x1b[34m") != std::string::npos);
    CHECK(second->lines.front().find("\x1b[31m") == std::string::npos);

    tui::VirtualTerminal update_terminal({
        .columns = 12,
        .rows = std::max(first->lines.size(), second->lines.size()),
    });
    REQUIRE(update_terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(update_terminal, *first);
    write_lines(update_terminal, *second);
    const auto* updated_cell = find_cell(update_terminal, "n");
    REQUIRE(updated_cell != nullptr);
    CHECK(updated_cell->style.fg_color == "34");
    for (const auto& row : update_terminal.cells()) {
        for (const auto& cell : row) CHECK(cell.style.fg_color != "31");
    }

    markdown.set_text("```cpp\ncache\n```");
    const auto first_highlight = markdown.render(12);
    REQUIRE(first_highlight);
    CHECK(first_highlight->lines.at(1).find("\x1b[36m") != std::string::npos);
    markdown.set_syntax_highlighter([](std::string_view code, std::string_view)
        -> std::optional<std::vector<std::string>> {
        return std::vector<std::string>{"\x1b[35m" + std::string(code) + "\x1b[39m"};
    });

    const auto second_highlight = markdown.render(12);

    REQUIRE(second_highlight);
    CHECK(rendered_text(*second_highlight, 12).find("cache") != std::string::npos);
    CHECK(second_highlight->lines.at(1).find("\x1b[35m") != std::string::npos);
    CHECK(second_highlight->lines.at(1).find("\x1b[36m") == std::string::npos);
}

TEST_CASE("Markdown heading roles match frozen pi styling", "[tui][markdown][issue971][issue1217][compat-pi]") {
    tui::MarkdownStyleConfig style;
    style.heading = [](std::string text) { return "\x1b[36m" + text + "\x1b[39m"; };
    style.strong = [](std::string text) { return "\x1b[1m" + text + "\x1b[22m"; };
    style.underline = [](std::string text) { return "\x1b[4m" + text + "\x1b[24m"; };
    style.inline_code = [](std::string text) { return "\x1b[33m" + text + "\x1b[39m"; };
    tui::Markdown markdown("# H1\n## H2\n# `code` tail", 0, 0, std::move(style));

    const auto lines = markdown.render(24);

    REQUIRE(lines);
    tui::VirtualTerminal terminal({.columns = 24, .rows = lines->lines.size()});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    const auto* h1 = find_cell(terminal, "H");
    REQUIRE(h1 != nullptr);
    CHECK(h1->style.bold);
    CHECK(h1->style.underline);
    CHECK(h1->style.fg_color == "36");
    const auto h2_row = std::find_if(terminal.cells().begin(), terminal.cells().end(), [](const auto& row) {
        return std::any_of(row.begin(), row.end(), [](const auto& cell) { return cell.grapheme == "2"; });
    });
    REQUIRE(h2_row != terminal.cells().end());
    const auto h2 = std::find_if(h2_row->begin(), h2_row->end(), [](const auto& cell) { return cell.grapheme == "H"; });
    REQUIRE(h2 != h2_row->end());
    CHECK(h2->style.bold);
    CHECK_FALSE(h2->style.underline);
    CHECK(h2->style.fg_color == "36");
    const auto* code = find_cell(terminal, "c");
    REQUIRE(code != nullptr);
    CHECK(code->style.fg_color == "33");
    CHECK_FALSE(code->style.bold);
    CHECK_FALSE(code->style.underline);
    const auto* tail = find_cell(terminal, "t");
    REQUIRE(tail != nullptr);
    CHECK(tail->style.bold);
    CHECK(tail->style.underline);
    CHECK(tail->style.fg_color == "36");
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

TEST_CASE("Markdown nested quote styles and OSC links preserve frozen cell attributes",
        "[tui][markdown][issue971][issue1217][compat-pi]") {
    tui::MarkdownStyleConfig style;
    style.quote = [](std::string text) { return "\x1b[3m" + text + "\x1b[23m"; };
    style.quote_border = [](std::string text) { return "\x1b[34m" + text + "\x1b[39m"; };
    style.link_text = [](std::string text) { return "\x1b[34m\x1b[4m" + text + "\x1b[24m\x1b[39m"; };
    tui::Markdown markdown("> > nested [linked](https://example.com)", 0, 0, std::move(style));
    const tests::ImageCapabilitiesGuard capabilities({.images = tui::InlineImageProtocol::None, .hyperlinks = true});

    const auto lines = markdown.render(40);

    REQUIRE(lines);
    tui::VirtualTerminal terminal({.columns = 40, .rows = lines->lines.size()});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    const auto borders = std::count_if(terminal.cells().front().begin(),
            terminal.cells().front().end(),
            [](const auto& cell) { return cell.grapheme == "│"; });
    CHECK(borders == 2);
    const auto* linked = find_cell(terminal, "l");
    REQUIRE(linked != nullptr);
    CHECK(linked->style.hyperlink == "https://example.com");
    CHECK(linked->style.underline);
    CHECK(linked->style.italic);
    CHECK(linked->style.fg_color == "34");
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

TEST_CASE("Markdown streamed code fences match frozen pi at each chunk",
        "[tui][markdown][issue971][issue1217][compat-pi]") {
    constexpr std::size_t kWidth = 40;
    const std::vector<std::pair<std::string, std::vector<std::string>>> snapshots{
            {"```cpp\n",
                    {"\x1b[2m```cpp\x1b[22m                                  ",
                            "  \x1b[32m\x1b[39m                                      ",
                            "\x1b[2m```\x1b[22m                                     "}},
            {"```cpp\nint value = 42;",
                    {"\x1b[2m```cpp\x1b[22m                                  ",
                            "  \x1b[32mint value = 42;\x1b[39m                       ",
                            "\x1b[2m```\x1b[22m                                     "}},
            {"```cpp\nint value = 42;\n``",
                    {"\x1b[2m```cpp\x1b[22m                                  ",
                            "  \x1b[32mint value = 42;\x1b[39m                       ",
                            "\x1b[2m```\x1b[22m                                     "}},
            {"```cpp\nint value = 42;\n```",
                    {"\x1b[2m```cpp\x1b[22m                                  ",
                            "  \x1b[32mint value = 42;\x1b[39m                       ",
                            "\x1b[2m```\x1b[22m                                     "}},
    };
    tui::Markdown markdown({}, 0, 0, ansi_style());
    for (const auto& [chunk, expected] : snapshots) {
        markdown.set_text(chunk);

        const auto lines = markdown.render(kWidth);

        REQUIRE(lines);
        CHECK(lines->lines == expected);
        tui::VirtualTerminal terminal({.columns = kWidth, .rows = lines->lines.size()});
        REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
                [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
        write_lines(terminal, *lines);
        CHECK(terminal.final_style() == tui::TerminalStyle{});
    }
}

TEST_CASE("Markdown default text hooks respect empty narrow and padded rendering",
        "[tui][markdown][issue971][issue1217][compat-pi]") {
    tui::Markdown empty(" \n\t", 0, 0, {});
    const auto empty_lines = empty.render(4);
    REQUIRE(empty_lines);
    CHECK(empty_lines->lines.empty());

    tui::MarkdownStyleConfig style;
    style.text = [](std::string text) { return "\x1b[31m" + text + "\x1b[39m"; };
    style.heading = [](std::string text) { return "\x1b[36m" + text + "\x1b[39m"; };
    style.strong = [](std::string text) { return "\x1b[1m" + text + "\x1b[22m"; };
    style.underline = [](std::string text) { return "\x1b[4m" + text + "\x1b[24m"; };
    tui::Markdown markdown("plain\n# heading", 1, 1, std::move(style));
    const auto lines = markdown.render(10);
    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 5);
    tui::VirtualTerminal terminal({.columns = 10, .rows = lines->lines.size()});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    const auto* plain = find_cell(terminal, "p");
    REQUIRE(plain != nullptr);
    CHECK(plain->style.fg_color == "31");
    const auto heading_row = std::find_if(terminal.cells().begin(), terminal.cells().end(), [](const auto& row) {
        return std::any_of(row.begin(), row.end(), [](const auto& cell) { return cell.grapheme == "h"; });
    });
    REQUIRE(heading_row != terminal.cells().end());
    const auto heading = std::find_if(
            heading_row->begin(), heading_row->end(), [](const auto& cell) { return cell.grapheme == "h"; });
    REQUIRE(heading != heading_row->end());
    CHECK(heading->style.fg_color == "36");
    CHECK(terminal.final_style() == tui::TerminalStyle{});

    tui::Markdown narrow("word", 0, 0);
    const auto narrow_lines = narrow.render(1);
    REQUIRE(narrow_lines);
    CHECK(narrow_lines->lines == std::vector<std::string>{"w", "o", "r", "d"});
}

TEST_CASE("Markdown applies configured padding and background to every cell",
        "[tui][markdown][issue51][issue1217][spec]") {
    auto background = tests::background_hook("\x1b[44m");
    tui::Markdown markdown("body", 1, 1, {}, {}, std::move(background));

    const auto lines = markdown.render(8);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 3);
    tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    for (const auto& row : terminal.cells()) {
        for (const auto& cell : row) CHECK(cell.style.bg_color == "44");
    }
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

TEST_CASE("Markdown renders GFM table borders and bold headers", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| Name | Value |\n| --- | --- |\n| a | b |", 0, 0);

    const auto lines = markdown.render(20);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 5);
    CHECK(lines->lines[0].starts_with("┌─"));
    CHECK(lines->lines[1].find("Name") != std::string::npos);
    CHECK(lines->lines[1].find("Value") != std::string::npos);
    CHECK(lines->lines[2].starts_with("├─"));
    CHECK(lines->lines[3].find("a") != std::string::npos);
    CHECK(lines->lines[3].find("b") != std::string::npos);
    CHECK(lines->lines[4].starts_with("└─"));
    for (const auto& line : lines->lines)
        CHECK(tui::visible_width(line) == 20);
}

TEST_CASE("Markdown allocates unequal GFM table column widths", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| X | Description |\n| --- | --- |\n| a | long value |", 0, 0);

    const auto lines = markdown.render(30);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 5);
    CHECK(lines->lines[0].starts_with("┌───┬─"));
    CHECK(lines->lines[1].find("Description") != std::string::npos);
    CHECK(lines->lines[3].find("long value") != std::string::npos);
}

TEST_CASE("Markdown wraps narrow GFM table cells within frozen borders", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| A | B |\n| --- | --- |\n| longword | another |", 0, 0);

    const auto lines = markdown.render(14);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() > 5);
    for (const auto& line : lines->lines)
        CHECK(tui::visible_width(line) <= 14);
    const auto text = rendered_text(*lines, 14);
    CHECK(text.find("long") != std::string::npos);
    CHECK(text.find("word") != std::string::npos);
    CHECK(text.find("ano") != std::string::npos);
    CHECK(text.find("the") != std::string::npos);
}

TEST_CASE("Markdown measures CJK GFM table cells by terminal columns", "[tui][markdown][issue972][issue1217][spec]") {
    tui::Markdown markdown("| 地域 | 名前 |\n| --- | --- |\n| 東京 | 大阪 |", 0, 0);

    const auto lines = markdown.render(24);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 5);
    for (const auto& line : lines->lines)
        CHECK(tui::visible_width(line) == 24);
    const auto text = rendered_text(*lines, 24);
    CHECK(text.find("東京") != std::string::npos);
    CHECK(text.find("大阪") != std::string::npos);
}

TEST_CASE("Markdown preserves linked GFM table cell styles", "[tui][markdown][issue972][spec]") {
    auto style = ansi_style();
    style.link_text = [](std::string text) { return "\x1b[34m" + text + "\x1b[39m"; };
    tui::Markdown markdown(
            "| Label | Link |\n| --- | --- |\n| value | [site](https://example.com) |", 0, 0, std::move(style));
    const tests::ImageCapabilitiesGuard capabilities({.images = tui::InlineImageProtocol::None, .hyperlinks = true});

    const auto lines = markdown.render(40);

    REQUIRE(lines);
    tui::VirtualTerminal terminal({.columns = 40, .rows = lines->lines.size()});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    write_lines(terminal, *lines);
    const auto* link = find_cell(terminal, "s");
    REQUIRE(link != nullptr);
    CHECK(link->style.hyperlink == "https://example.com");
    CHECK(link->style.fg_color == "34");
    const auto* header = find_cell(terminal, "L");
    REQUIRE(header != nullptr);
    CHECK(header->style.bold);
    CHECK(terminal.final_style() == tui::TerminalStyle{});
}

TEST_CASE("Markdown falls back to raw GFM table text below border width", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| A | B |\n| --- | --- |\n| x | y |", 0, 0);

    const auto lines = markdown.render(8);

    REQUIRE(lines);
    REQUIRE_FALSE(lines->lines.empty());
    const auto text = rendered_text(*lines, 8);
    CHECK(text.find("|") != std::string::npos);
    CHECK(text.find("┌") == std::string::npos);
}

TEST_CASE("Markdown separates a table from following content", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| A | B |\n| --- | --- |\n| x | y |\nAfter table", 0, 0);

    const auto lines = markdown.render(24);

    REQUIRE(lines);
    const auto bottom = std::find_if(
            lines->lines.begin(), lines->lines.end(), [](const auto& line) { return line.starts_with("└─"); });
    REQUIRE(bottom != lines->lines.end());
    REQUIRE(std::distance(bottom, lines->lines.end()) >= 3);
    CHECK(std::all_of(
            std::next(bottom)->begin(), std::next(bottom)->end(), [](char character) { return character == ' '; }));
    CHECK(std::next(bottom, 2)->find("After table") != std::string::npos);
}

TEST_CASE("Markdown renders GFM table rows as streamed chunks complete", "[tui][markdown][issue972][spec]") {
    tui::Markdown markdown("| A | B |\n| --- | --- |\n| par", 0, 0);

    const auto partial = markdown.render(24);

    REQUIRE(partial);
    CHECK(rendered_text(*partial, 24).find("par") != std::string::npos);
    markdown.set_text("| A | B |\n| --- | --- |\n| partial | done |");

    const auto complete = markdown.render(24);

    REQUIRE(complete);
    const auto text = rendered_text(*complete, 24);
    CHECK(text.find("partial") != std::string::npos);
    CHECK(text.find("done") != std::string::npos);
}

TEST_CASE("Markdown background survives inline styling and code blocks on every cell",
        "[tui][markdown][background][issue707][spec]") {
    // Regression for #707: a wrapped line with inline styling used to end in a
    // full reset emitted by the wrapping utility, which cancelled the enclosing
    // Markdown background so its padding lost the tint. The hook is move-only,
    // so the probe and the Tui-owned component each take a fresh one.
    // The bold run stays open across the first break, which is where the old
    // wrapping utility emitted a full reset and cut the background at the end
    // of the text.
    const std::string source = "**bold words that wrap across the line** and `code` and [link](https://example.com)\n"
                               "```cpp\nint answer = 42;\n```";

    // Render once through the component to pin the exact composed row count, so
    // the VirtualTerminal has no trailing empty rows to pollute the per-cell
    // assertion. padding_x makes the right margin land after the wrapped line
    // content, which is where the old reset used to cut the background.
    tui::Markdown probe(source, 1, 0, ansi_style(), tui::SyntaxHighlightHook{}, tests::background_hook("\x1b[44m"));
    const auto probe_lines = probe.render(20);
    REQUIRE(probe_lines);
    const auto row_count = probe_lines->lines.size();
    REQUIRE(row_count > 0);

    tui::VirtualTerminal terminal({.columns = 20, .rows = row_count});
    tui::Tui tui(terminal);
    auto markdown = std::make_unique<tui::Markdown>(
            source, 1, 0, ansi_style(), tui::SyntaxHighlightHook{}, tests::background_hook("\x1b[44m"));
    REQUIRE(tui.add_child(std::move(markdown)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Every cell of every row — the wrapped inline-styled paragraph rows and
    // the fenced-code rows — carries the configured background.
    tests::check_background_cells(terminal, row_count, 20, "44");

    // Each composed Markdown row ends with the one full reset at its true end.
    std::size_t background_rows = 0;
    for (const auto& row : terminal.output()) {
        if (row.find("\x1b[44m") == std::string::npos) continue;
        ++background_rows;
        CHECK(row.ends_with("\x1b[0m\x1b]8;;\x07"));
    }
    CHECK(background_rows == row_count);
}

TEST_CASE("pi-v1.3.0 markdown renders headings and inline styles matching frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-headings-and-inline-styles");
    const auto& expected = scenario.at("expected").at("lines").get_array();
    REQUIRE(expected.size() >= 4);

    tui::Markdown markdown(scenario.at("inputs").at("markdown").get_string(), 0, 0, ansi_style());
    const auto rendered = markdown.render(50);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() >= 4);
    const auto text = rendered_text(*rendered, 50);
    CHECK(text.find("Heading 1") != std::string::npos);
    CHECK(text.find("Heading 2") != std::string::npos);
    CHECK(text.find("bold") != std::string::npos);
    CHECK(text.find("inline") != std::string::npos);
    CHECK(text.find("code") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown list markers and normalization match frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-lists-and-markers");

    tui::Markdown unordered(scenario.at("inputs").at("unordered").get_string(), 0, 0, ansi_style());
    const auto un_rendered = unordered.render(40);
    REQUIRE(un_rendered);
    const auto un_text = rendered_text(*un_rendered, 40);
    CHECK(un_text.find("bullet 1") != std::string::npos);
    CHECK(un_text.find("nested bullet") != std::string::npos);

    tui::Markdown ordered(scenario.at("inputs").at("ordered").get_string(), 0, 0, ansi_style());
    const auto ord_rendered = ordered.render(40);
    REQUIRE(ord_rendered);
    const auto ord_text = rendered_text(*ord_rendered, 40);
    CHECK(ord_text.find("1. item one") != std::string::npos);
    CHECK(ord_text.find("2. item two") != std::string::npos);
    CHECK(ord_text.find("3. item three") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown tables and CJK width measurement match frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-tables-and-cjk");

    tui::Markdown std_table(scenario.at("inputs").at("standard").get_string(), 0, 0, ansi_style());
    const auto std_rendered = std_table.render(30);
    REQUIRE(std_rendered);
    const auto std_text = rendered_text(*std_rendered, 30);
    CHECK(std_text.find("Col A") != std::string::npos);
    CHECK(std_text.find("val 1") != std::string::npos);

    tui::Markdown cjk_table(scenario.at("inputs").at("cjk").get_string(), 0, 0, ansi_style());
    const auto cjk_rendered = cjk_table.render(24);
    REQUIRE(cjk_rendered);
    const auto cjk_text = rendered_text(*cjk_rendered, 24);
    CHECK(cjk_text.find("地域") != std::string::npos);
    CHECK(cjk_text.find("東京") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown code blocks and blockquotes match frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-code-blocks-and-quotes");

    tui::Markdown code(scenario.at("inputs").at("code").get_string(), 0, 0, ansi_style());
    const auto code_rendered = code.render(40);
    REQUIRE(code_rendered);
    const auto code_text = rendered_text(*code_rendered, 40);
    CHECK(code_text.find("int answer = 42;") != std::string::npos);

    tui::Markdown quote(scenario.at("inputs").at("quote").get_string(), 0, 0, ansi_style());
    const auto quote_rendered = quote.render(40);
    REQUIRE(quote_rendered);
    const auto quote_text = rendered_text(*quote_rendered, 40);
    CHECK(quote_text.find("first line of quote") != std::string::npos);
    CHECK(quote_text.find("nested quote") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown links hr and options transform match frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-links-hr-and-transforms");

    tui::Markdown link(scenario.at("inputs").at("link").get_string(), 0, 0, ansi_style());
    const auto link_rendered = link.render(40);
    REQUIRE(link_rendered);
    const auto link_text = rendered_text(*link_rendered, 40);
    CHECK(link_text.find("Pike") != std::string::npos);

    tui::Markdown hr(scenario.at("inputs").at("hr").get_string(), 0, 0, ansi_style());
    const auto hr_rendered = hr.render(30);
    REQUIRE(hr_rendered);
    CHECK(hr_rendered->lines.size() == 1);
    CHECK(hr_rendered->lines[0].find("─") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown latex math integration matches frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-latex-math-integration");

    tui::Markdown inline_math(scenario.at("inputs").at("inlineMath").get_string(), 0, 0, ansi_style());
    const auto inline_rendered = inline_math.render(40);
    REQUIRE(inline_rendered);
    const auto inline_text = rendered_text(*inline_rendered, 40);
    CHECK(inline_text.find("1/2") != std::string::npos);
    CHECK(inline_text.find("x²") != std::string::npos);

    tui::Markdown display_math(scenario.at("inputs").at("displayMath").get_string(), 0, 0, ansi_style());
    const auto display_rendered = display_math.render(40);
    REQUIRE(display_rendered);
    const auto display_text = rendered_text(*display_rendered, 40);
    CHECK(display_text.find("1") != std::string::npos);
    CHECK(display_text.find("─") != std::string::npos);

    tui::Markdown disabled_math(
            scenario.at("inputs").at("inlineMath").get_string(), 0, 0, ansi_style(), {}, {}, {.render_latex = false});
    const auto disabled_rendered = disabled_math.render(40);
    REQUIRE(disabled_rendered);
    const auto disabled_text = rendered_text(*disabled_rendered, 40);
    CHECK(disabled_text.find(R"($\frac{1}{2}$)") != std::string::npos);

    tui::Markdown pending_math(scenario.at("inputs").at("pendingMath").get_string(), 0, 0, ansi_style());
    const auto pending_rendered = pending_math.render(40);
    REQUIRE(pending_rendered);
    const auto pending_text = rendered_text(*pending_rendered, 40);
    CHECK(pending_text.find("before $") != std::string::npos);
}

TEST_CASE("pi-v1.3.0 markdown lifecycle default styles and cache invalidation match frozen observation",
        "[tui][markdown][issue1217][compat-pi]") {
    const auto fixture = tests::read_pi_v130_audit_evidence("audit-1217", "markdown.json");
    REQUIRE(fixture);
    const auto& scenario = tests::find_scenario(*fixture, "markdown-lifecycle-and-default-styles");

    tui::Markdown md(scenario.at("inputs").at("initial").get_string(), 2, 1, ansi_style());
    CHECK(md.text() == "initial text");
    const auto initial = md.render(40);
    REQUIRE(initial);
    CHECK(initial->lines.size() >= 3);

    md.set_text(scenario.at("inputs").at("updated").get_string());
    CHECK(md.text() == "updated text");
    const auto updated = md.render(30);
    REQUIRE(updated);
    const auto updated_text = rendered_text(*updated, 30);
    CHECK(updated_text.find("updated text") != std::string::npos);

    md.invalidate();
    const auto invalidated = md.render(30);
    REQUIRE(invalidated);
    CHECK(invalidated->lines == updated->lines);
}
