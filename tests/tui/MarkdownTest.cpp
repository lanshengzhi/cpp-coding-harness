#include <cch/tui/Markdown.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

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
            [](std::string) {},
            [](tui::TerminalDimensions) {});
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
        [](std::string) {},
        [](tui::TerminalDimensions) {}));
    write_lines(terminal, lines);
    CHECK(terminal.final_style() == tui::VirtualTerminalStyle{});
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

TEST_CASE("Markdown wraps plain text within terminal width", "[tui][markdown][issue51]") {
    tui::Markdown markdown("alpha beta gamma", 0, 0);

    const auto lines = markdown.render(10);

    REQUIRE(lines);
    const std::vector<std::string> expected{"alpha beta", "gamma     "};
    CHECK(lines->lines == expected);
}

TEST_CASE("Markdown renders common response constructs in semantic order", "[tui][markdown][issue51]") {
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

TEST_CASE(
    "Markdown preserves nested styles Unicode and link termination at narrow widths",
    "[tui][markdown][issue51]") {
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
        [](std::string) {},
        [](tui::TerminalDimensions) {}));
    write_lines(terminal, *lines);
    const auto* linked_cell = find_cell(terminal, "l");
    REQUIRE(linked_cell != nullptr);
    CHECK(linked_cell->style.bold);
    CHECK(linked_cell->style.italic);
    CHECK(linked_cell->style.hyperlink == "https://example.com/long");
    CHECK(terminal.final_style() == tui::VirtualTerminalStyle{});
}

TEST_CASE("Markdown injects syntax highlighting and falls back for unknown languages", "[tui][markdown][issue51]") {
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
        [](std::string) {},
        [](tui::TerminalDimensions) {}));
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

TEST_CASE("Markdown keeps partial and invalid input readable", "[tui][markdown][issue51]") {
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

TEST_CASE("Markdown degrades semantic prefixes at extremely narrow widths", "[tui][markdown][issue51]") {
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

TEST_CASE("Markdown follows strict strikethrough delimiter semantics", "[tui][markdown][issue51]") {
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

TEST_CASE("Markdown preserves loose list paragraph structure", "[tui][markdown][issue51]") {
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

TEST_CASE("Markdown stabilizes streamed partial closing fences", "[tui][markdown][issue51]") {
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

TEST_CASE("Markdown converts injected callback exceptions to render errors", "[tui][markdown][issue51]") {
    auto style = ansi_style();
    style.heading = [](std::string) -> std::string { throw std::runtime_error("style"); };
    tui::Markdown markdown("# heading", 0, 0, std::move(style));

    const auto result = markdown.render(20);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == util::ErrorCode::Unknown);
    CHECK(result.error().message == "TUI Markdown callback failed");
}

TEST_CASE("Markdown invalidates content style and highlighter caches", "[tui][markdown][issue51]") {
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
        [](std::string) {},
        [](tui::TerminalDimensions) {}));
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

TEST_CASE("Markdown applies configured padding and background to every cell", "[tui][markdown][issue51]") {
    auto background = [](std::string text) { return "\x1b[44m" + text + "\x1b[49m"; };
    tui::Markdown markdown("body", 1, 1, {}, {}, std::move(background));

    const auto lines = markdown.render(8);

    REQUIRE(lines);
    REQUIRE(lines->lines.size() == 3);
    tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    REQUIRE(terminal.start(
        [](std::string) {},
        [](tui::TerminalDimensions) {}));
    write_lines(terminal, *lines);
    for (const auto& row : terminal.cells()) {
        for (const auto& cell : row) CHECK(cell.style.bg_color == "44");
    }
    CHECK(terminal.final_style() == tui::VirtualTerminalStyle{});
}
