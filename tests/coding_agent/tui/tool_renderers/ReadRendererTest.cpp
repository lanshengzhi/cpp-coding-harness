#include "coding_agent/tui/tool_renderers/RenderUtils.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ImageCapabilitiesGuard.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// pi's `getCapabilities().hyperlinks === false` presentation, so the composed
/// rows carry no OSC 8 pair and a case reads them as plain text. A case
/// constructs its own: the guard installs process-wide state, so it must not
/// live at namespace scope.
struct PlainCapabilities {
    PlainCapabilities()
        : guard({
                  .images = cch::tui::InlineImageProtocol::None,
                  .hyperlinks = false,
          }) {}

    tests::ImageCapabilitiesGuard guard;
};

/// Every case renders through the host, so the assertions read the composed
/// rows a user sees rather than a renderer's return value in isolation.
struct ReadBlock {
    // The component keeps a reference to the theme, so it is a member declared
    // before the component rather than a temporary in the initializer list.
    coding_agent::tui::LiveTheme theme{tests::tool_render_theme()};
    coding_agent::tui::ToolExecutionComponent component;

    ReadBlock(const std::shared_ptr<const coding_agent::tui::SharedKeybindings>& keybindings,
            std::string arguments_json,
            std::string cwd = "/workspace")
        : component(theme, keybindings, "read", "call_1", std::move(arguments_json), std::move(cwd)) {}

    void succeed(std::string output, std::optional<support::JsonValue> details = std::nullopt) {
        component.update_result(ai::ToolResultMessage{
                .tool_call_id = "call_1",
                .tool_name = "read",
                .content = {ai::text_content(std::move(output))},
                .details = std::move(details),
                .is_error = false,
                .timestamp = 0,
        });
    }

    void fail(std::string output, std::optional<support::JsonValue> details = std::nullopt) {
        component.update_result(ai::ToolResultMessage{
                .tool_call_id = "call_1",
                .tool_name = "read",
                .content = {ai::text_content(std::move(output))},
                .details = std::move(details),
                .is_error = true,
                .timestamp = 0,
        });
    }

    [[nodiscard]] tests::ToolRenderScreen screen(std::size_t width = 80) {
        return tests::render_tool_screen(component, width);
    }

    [[nodiscard]] std::vector<std::string> rows(std::size_t width = 80) { return screen(width).visible; }
};

[[nodiscard]] std::string numbered_output(std::size_t count) {
    std::string output;
    for (std::size_t line = 1; line <= count; ++line) {
        if (!output.empty()) output.push_back('\n');
        output += "line " + std::to_string(line);
    }
    return output;
}

/// pi's `details.truncation` exactly as `AsyncToolFactories.cpp`
/// `truncation_details` builds it, so the renderer consumes the wire shape the
/// tool layer emits rather than a projection of it.
[[nodiscard]] support::JsonValue truncation_details(bool truncated,
        std::string_view truncated_by,
        double output_lines,
        double total_lines,
        bool first_line_exceeds_limit,
        double max_lines,
        double max_bytes) {
    const auto number = [](double value) { return support::JsonValue{value}; };
    return support::JsonValue{support::JsonValue::object_t{
            {"truncation",
                    support::JsonValue{support::JsonValue::object_t{
                            {"truncated", support::JsonValue{truncated}},
                            {"truncatedBy",
                                    truncated_by.empty() ? support::JsonValue{nullptr}
                                                         : support::JsonValue{std::string{truncated_by}}},
                            {"totalLines", number(total_lines)},
                            {"totalBytes", number(0.0)},
                            {"outputLines", number(output_lines)},
                            {"outputBytes", number(0.0)},
                            {"lastLinePartial", support::JsonValue{false}},
                            {"firstLineExceedsLimit", support::JsonValue{first_line_exceeds_limit}},
                            {"maxLines", number(max_lines)},
                            {"maxBytes", number(max_bytes)},
                    }}},
    }};
}

[[nodiscard]] bool has_row(const std::vector<std::string>& rows, std::string_view text) {
    return std::find(rows.begin(), rows.end(), text) != rows.end();
}

/// The tool block's box paints its own background token across a row, adds one
/// margin column ahead of the content, pads the row to the render width, and
/// closes with the background reset. This strips exactly those four box-owned
/// pieces and returns the run the renderer itself emitted, so a case can
/// compare it for equality: a wrong token, a reordered pair, or a sequence the
/// renderer did not write all fail, while none of the box's own framing does.
[[nodiscard]] std::string row_body(const std::string& row) {
    const auto margin = row.find("m ");
    REQUIRE(margin != std::string::npos);
    auto body = row.substr(margin + 2);
    const auto background_reset = body.rfind("\x1b[49m");
    if (background_reset != std::string::npos) body.erase(background_reset);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\n' || body.back() == '\r'))
        body.pop_back();
    return body;
}

[[nodiscard]] std::string joined(const std::vector<std::string>& rows) {
    std::string all;
    for (const auto& row : rows)
        all += row + "\n";
    return all;
}

} // namespace

TEST_CASE("a collapsed successful read renders the title line and nothing else",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"notes.txt"})");
    block.succeed("alpha\nbeta\ngamma");

    const auto screen = block.screen();

    // The whole composed block, not a `find("read")`: a body line leaking
    // through, a stray hint, or a missing title all satisfy a substring check
    // while violating "the title line is the whole collapsed block".
    CHECK(screen.visible == std::vector<std::string>{"", "read notes.txt", ""});

    // The negative twin: the file body is absent from the screen entirely, not
    // merely off the visible rows a reader happens to look at.
    const auto all = joined(screen.visible);
    CHECK(all.find("alpha") == std::string::npos);
    CHECK(all.find("beta") == std::string::npos);
    CHECK(all.find("more lines") == std::string::npos);
}

TEST_CASE("an expanded successful read renders the whole file body",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"notes.txt"})");
    block.succeed("alpha\nbeta\ngamma");
    block.component.set_expanded(true);

    CHECK(block.rows() == std::vector<std::string>{"", "read notes.txt", "", "alpha", "beta", "gamma", ""});
}

TEST_CASE("the read title carries pi's warning-coloured line range only when offset or limit is present",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();
    auto theme = tests::tool_render_theme();

    SECTION("offset alone") {
        // pi renders `:5`, never `:5-` and never a defaulted `:1-5`.
        ReadBlock block(keybindings, R"({"path":"notes.txt","offset":5})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "read notes.txt:5", ""});
    }

    SECTION("offset and limit") {
        // The end is `start + limit - 1`.
        ReadBlock block(keybindings, R"({"path":"notes.txt","offset":5,"limit":100})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "read notes.txt:5-104", ""});
    }

    SECTION("limit alone starts at 1") {
        ReadBlock block(keybindings, R"({"path":"notes.txt","limit":3})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "read notes.txt:1-3", ""});
    }

    SECTION("neither emits no suffix at all, not `:1`") {
        // A `:1` here satisfies "a range is shown" while contradicting
        // `formatReadLineRange`, which returns the empty string.
        ReadBlock block(keybindings, R"({"path":"notes.txt"})");
        block.succeed("alpha");
        block.component.set_expanded(true);
        CHECK(block.rows() == std::vector<std::string>{"", "read notes.txt", "", "alpha", ""});
    }

    SECTION("the range is the warning token, outside the path's hyperlink") {
        // The visible text alone passes for an uncoloured suffix.
        const tests::EnvVarGuard home("HOME", "/home/agent");
        const tests::ImageCapabilitiesGuard linked({
                .images = cch::tui::InlineImageProtocol::None,
                .hyperlinks = true,
        });
        ReadBlock block(keybindings, R"({"path":"/home/agent/notes.txt","offset":5})");
        block.succeed("alpha");
        block.component.set_expanded(true);
        const auto screen = block.screen();
        REQUIRE(screen.visible.size() == 5);
        CHECK(screen.visible[1] == "read ~/notes.txt:5");
        // The display is `~`-shortened, the link target is the resolved
        // absolute path, and the `:5` range is a separate `warning` run
        // outside the link: a link that swallowed the suffix, or one carrying
        // `~`, satisfies a "has a hyperlink and a range" check.
        const auto title = coding_agent::tui::bold_foreground(theme, coding_agent::tui::ThemeToken::ToolTitle, "read") +
                           " " +
                           cch::tui::hyperlink(theme.foreground(coding_agent::tui::ThemeToken::Accent, "~/notes.txt"),
                                   "file:///home/agent/notes.txt") +
                           theme.foreground(coding_agent::tui::ThemeToken::Warning, ":5");
        CHECK(row_body(screen.raw[1]) == title);
    }
}

TEST_CASE("an expanded read shows the plain read title even when the path classifies",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();

    // pi `read.ts:154` attempts the classification only while collapsed, so an
    // expanded read of a `SKILL.md` is the plain form. A "compact titles exist"
    // check passes while this behaviour is missing entirely.
    ReadBlock collapsed(keybindings, R"({"path":"skills/pdf/SKILL.md"})");
    collapsed.succeed("alpha");
    CHECK(collapsed.rows() == std::vector<std::string>{"", "[skill] pdf (ctrl+o to expand)", ""});

    ReadBlock expanded(keybindings, R"({"path":"skills/pdf/SKILL.md"})");
    expanded.succeed("alpha");
    expanded.component.set_expanded(true);
    CHECK(expanded.rows() == std::vector<std::string>{"", "read skills/pdf/SKILL.md", "", "alpha", ""});
}

TEST_CASE("a SKILL.md read titles as the skill label and a resource read as read resource, and a plain markdown file "
          "as neither",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();

    SECTION("the skill label is the containing directory, then the range, then the hint") {
        ReadBlock block(keybindings, R"({"path":"skills/pdf/SKILL.md","offset":2,"limit":4})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "[skill] pdf:2-5 (ctrl+o to expand)", ""});
    }

    SECTION("a root-level SKILL.md falls back to the file name") {
        // pi's `basename(dirname(path)) || fileName`: the containing directory
        // of `/SKILL.md` is `/`, whose basename is empty.
        ReadBlock block(keybindings, R"({"path":"/SKILL.md"})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "[skill] SKILL.md (ctrl+o to expand)", ""});
    }

    SECTION("a resource label is cwd-relative when the file is inside the cwd") {
        ReadBlock block(keybindings, R"({"path":"docs/AGENTS.md"})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "read resource docs/AGENTS.md (ctrl+o to expand)", ""});
    }

    SECTION("a resource label outside the cwd is the absolute path, not a `..` escape") {
        ReadBlock block(keybindings, R"({"path":"/etc/CLAUDE.md"})");
        block.succeed("alpha");
        CHECK(block.rows() == std::vector<std::string>{"", "read resource /etc/CLAUDE.md (ctrl+o to expand)", ""});
    }

    SECTION("the classification resolves through pi's full resolveToCwd, not `~` alone") {
        // ADR 0057's three preprocessing steps reach the label as well as the
        // gate. A `~`-only resolution leaves the marker in the cwd-relative
        // path, so the two rows below are the discriminating assertions: an
        // assertion on the *classification* alone passes either way, because
        // `basename` reads the same file name off both resolutions.
        ReadBlock at_marked(keybindings, R"({"path":"@docs/AGENTS.md"})");
        at_marked.succeed("alpha");
        CHECK(at_marked.rows() == std::vector<std::string>{"", "read resource docs/AGENTS.md (ctrl+o to expand)", ""});

        // pi `utils/paths.ts:7` `UNICODE_SPACES`: a U+00A0 no-break space
        // between two directory name segments is one ASCII space.
        const std::string unicode_space = std::string{R"({"path":"do"} + "\xC2\xA0" + R"(cs/AGENTS.md"})";
        ReadBlock spaced(keybindings, unicode_space);
        spaced.succeed("alpha");
        CHECK(spaced.rows() == std::vector<std::string>{"", "read resource docs/AGENTS.md (ctrl+o to expand)", ""});

        // The `~` step the same helper performs: the classification follows
        // the expanded home, not a literal `~` directory under the cwd.
        const tests::EnvVarGuard home("HOME", "/home/agent");
        ReadBlock tilde(keybindings, R"({"path":"~/skills/pdf/SKILL.md"})");
        tilde.succeed("alpha");
        CHECK(tilde.rows() == std::vector<std::string>{"", "[skill] pdf (ctrl+o to expand)", ""});
    }

    SECTION("the resource set is exactly pi's five names, matched case-sensitively") {
        for (const auto* name : {"AGENTS.md", "AGENTS.MD", "AGENTS.override.md", "CLAUDE.md", "CLAUDE.MD"}) {
            ReadBlock block(keybindings, std::string{R"({"path":")"} + name + R"("})");
            block.succeed("alpha");
            const auto rows = block.rows();
            REQUIRE(rows.size() == 3);
            CHECK(rows[1].starts_with("read resource "));
        }

        // `agents.md` in lower case is NOT one of them, and neither is a plain
        // markdown file. A case-insensitive match, or a "some reads are
        // compact" check, passes while both are wrong.
        ReadBlock wrong_case(keybindings, R"({"path":"agents.md"})");
        wrong_case.succeed("alpha");
        CHECK(wrong_case.rows() == std::vector<std::string>{"", "read agents.md", ""});

        ReadBlock markdown(keybindings, R"({"path":"notes/foo.md"})");
        markdown.succeed("alpha");
        CHECK(markdown.rows() == std::vector<std::string>{"", "read notes/foo.md", ""});
    }

    SECTION("both path spellings classify, because pi reads `file_path ?? path`") {
        // pi `read.ts:70` reads `str(args?.file_path ?? args?.path)`, the same
        // two-key precedence the title uses. Asserting only the `path`
        // spelling passes whether or not `file_path` works at all, so both are
        // driven here and both must reach the skill label.
        for (const auto* arguments : {R"({"path":"skills/pdf/SKILL.md"})", R"({"file_path":"skills/pdf/SKILL.md"})"}) {
            ReadBlock block(keybindings, arguments);
            block.succeed("alpha");
            CHECK(block.rows() == std::vector<std::string>{"", "[skill] pdf (ctrl+o to expand)", ""});
        }
    }
}

TEST_CASE("the compact title and the compact hint carry pi's own tokens",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();
    auto theme = tests::tool_render_theme();

    SECTION("the skill label is a literal bold pair inside customMessageLabel") {
        ReadBlock block(keybindings, R"({"path":"skills/pdf/SKILL.md"})");
        block.succeed("alpha");
        const auto screen = block.screen();
        REQUIRE(screen.raw.size() == 3);
        // pi does not use `theme.bold` here; the label text is literally
        // `\x1b[1m[skill]\x1b[22m ` in the `customMessageLabel` token, and the
        // label itself is `customMessageText`. A `bold_foreground` spelling
        // yields the same visible rows and fails here.
        CHECK(row_body(screen.raw[1]) ==
                theme.foreground(coding_agent::tui::ThemeToken::CustomMessageLabel, "\x1b[1m[skill]\x1b[22m ") +
                        theme.foreground(coding_agent::tui::ThemeToken::CustomMessageText, "pdf") +
                        theme.foreground(coding_agent::tui::ThemeToken::Dim, " (ctrl+o to expand)"));
    }

    SECTION("the resource title is a bold toolTitle label, an accent label, and a dim hint") {
        ReadBlock block(keybindings, R"({"path":"docs/AGENTS.md"})");
        block.succeed("alpha");
        const auto screen = block.screen();
        REQUIRE(screen.raw.size() == 3);
        // The compact hint is not the fold hint: it carries no line count, has
        // a leading space inside the parens, and is entirely `dim`.
        CHECK(row_body(screen.raw[1]) ==
                coding_agent::tui::bold_foreground(theme, coding_agent::tui::ThemeToken::ToolTitle, "read resource") +
                        " " + theme.foreground(coding_agent::tui::ThemeToken::Accent, "docs/AGENTS.md") +
                        theme.foreground(coding_agent::tui::ThemeToken::Dim, " (ctrl+o to expand)"));
    }
}

TEST_CASE("the compact expand hint reads Unbound when the expand key is unbound",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(/*bound=*/false), R"({"path":"skills/pdf/SKILL.md"})");
    block.succeed("alpha");

    // The whole hint, not "Unbound": pi's punctuation is a leading space, then
    // the key, then " to expand", inside the parens.
    CHECK(block.rows() == std::vector<std::string>{"", "[skill] pdf (Unbound to expand)", ""});
}

TEST_CASE("mid-stream read arguments put pi's progressive dots in the path slot",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();

    // A half-streamed argument object is not valid JSON, so the host hands the
    // renderer an empty argument object and the path slot is `...`. Asserting
    // "no fragment of the argument text" alone passes for any placeholder.
    ReadBlock streaming(keybindings, R"({"path":"/tmp/notes.txt","off)");
    CHECK(streaming.rows() == std::vector<std::string>{"", "read ...", ""});

    streaming.component.update_args(R"({"path":"/tmp/notes.txt"})");
    CHECK(streaming.rows() == std::vector<std::string>{"", "read /tmp/notes.txt", ""});

    // An argument present but not a string is pi's `[invalid arg]`: a third
    // outcome distinct from both the dots and the path.
    ReadBlock invalid(keybindings, R"({"path":7})");
    CHECK(invalid.rows() == std::vector<std::string>{"", "read [invalid arg]", ""});
}

TEST_CASE("a read error shows ten lines and pi's remaining-lines hint",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"missing.txt"})");
    block.fail(numbered_output(13));

    // The exact rows: a tail fold satisfies "3 more lines" while showing the
    // wrong ten, and ten lines without the hint satisfies "ten lines".
    const std::vector<std::string> expected{
            "",
            "read missing.txt",
            "",
            "line 1",
            "line 2",
            "line 3",
            "line 4",
            "line 5",
            "line 6",
            "line 7",
            "line 8",
            "line 9",
            "line 10",
            "... (3 more lines, ctrl+o to expand)",
            "",
    };
    const auto rows = block.rows();
    CHECK(rows == expected);
    CHECK(!has_row(rows, "line 11"));
    CHECK(!has_row(rows, "line 13"));
}

TEST_CASE("a read error's fold hint counts real lines, not the trailing empty one",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"missing.txt"})");
    // Eleven real lines plus a trailing newline: pi's
    // `trimTrailingEmptyLines` runs before the count, so the hint says one more
    // line. A count taken on the raw split is off by one and still prints a
    // plausible "N more lines" hint.
    block.fail(numbered_output(11) + "\n");

    const auto rows = block.rows();
    REQUIRE(rows.size() == 15);
    CHECK(rows[13] == "... (1 more lines, ctrl+o to expand)");
}

TEST_CASE("an expanded read error shows every line and no fold hint",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"missing.txt"})");
    block.fail(numbered_output(13));
    block.component.set_expanded(true);

    const auto rows = block.rows();
    CHECK(rows == std::vector<std::string>{
                          "",
                          "read missing.txt",
                          "",
                          "line 1",
                          "line 2",
                          "line 3",
                          "line 4",
                          "line 5",
                          "line 6",
                          "line 7",
                          "line 8",
                          "line 9",
                          "line 10",
                          "line 11",
                          "line 12",
                          "line 13",
                          "",
                  });
}

TEST_CASE("each of the three truncation warnings renders its own text",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto keybindings = tests::tool_render_keybindings();

    // Every section asserts the whole composed block, so a renderer that
    // collapsed the three warnings into one message, emitted the right warning
    // on the wrong row, or emitted two of them all fail here. A
    // `find("[Truncated")` check passes for all three.
    const std::vector<std::string> no_warning{"", "read notes.txt", "", "x", "y", ""};

    SECTION("a first line over the byte limit") {
        ReadBlock block(keybindings, R"({"path":"wide.txt"})");
        block.succeed("x\ny", truncation_details(true, "", 0.0, 2.0, true, 2000.0, 50.0 * 1024.0));
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows ==
                std::vector<std::string>{"", "read wide.txt", "", "x", "y", "[First line exceeds 50.0KB limit]", ""});
    }

    SECTION("a line-count truncation") {
        ReadBlock block(keybindings, R"({"path":"long.txt"})");
        block.succeed("x\ny", truncation_details(true, "lines", 2000.0, 5231.0, false, 2000.0, 50.0 * 1024.0));
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows == std::vector<std::string>{"",
                              "read long.txt",
                              "",
                              "x",
                              "y",
                              "[Truncated: showing 2000 of 5231 lines (2000 line limit)]",
                              ""});
    }

    SECTION("a byte truncation") {
        ReadBlock block(keybindings, R"({"path":"long.txt"})");
        block.succeed("x\ny", truncation_details(true, "bytes", 812.0, 900.0, false, 2000.0, 50.0 * 1024.0));
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows == std::vector<std::string>{
                              "", "read long.txt", "", "x", "y", "[Truncated: 812 lines shown (50.0KB limit)]", ""});
    }

    SECTION("a first-line overflow wins over the line-count branch") {
        // pi's `if / else if`: a truncation object carrying both flags takes
        // the first branch only. Two warnings here would satisfy "a warning is
        // drawn" for each of the three.
        ReadBlock block(keybindings, R"({"path":"long.txt"})");
        block.succeed("x\ny", truncation_details(true, "lines", 12.0, 40.0, true, 2000.0, 50.0 * 1024.0));
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows ==
                std::vector<std::string>{"", "read long.txt", "", "x", "y", "[First line exceeds 50.0KB limit]", ""});
    }

    SECTION("a truncation object that omits the limits falls back to pi's defaults") {
        // pi's `maxBytes ?? DEFAULT_MAX_BYTES` / `maxLines ?? DEFAULT_MAX_LINES`
        // exist because the object may omit them; dropping the fallback prints
        // `0B` / `0 line limit`, which still looks like a warning.
        ReadBlock block(keybindings, R"({"path":"long.txt"})");
        const support::JsonValue::object_t fields{
                {"truncated", support::JsonValue{true}},
                {"truncatedBy", support::JsonValue{std::string{"bytes"}}},
                {"outputLines", support::JsonValue{7.0}},
                {"totalLines", support::JsonValue{9.0}},
        };
        block.succeed(
                "x\ny", support::JsonValue{support::JsonValue::object_t{{"truncation", support::JsonValue{fields}}}});
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows == std::vector<std::string>{
                              "", "read long.txt", "", "x", "y", "[Truncated: 7 lines shown (50.0KB limit)]", ""});
    }

    SECTION("a line-count truncation without maxLines still names the default limit") {
        ReadBlock block(keybindings, R"({"path":"long.txt"})");
        const support::JsonValue::object_t fields{
                {"truncated", support::JsonValue{true}},
                {"truncatedBy", support::JsonValue{std::string{"lines"}}},
                {"outputLines", support::JsonValue{2000.0}},
                {"totalLines", support::JsonValue{5231.0}},
        };
        block.succeed(
                "x\ny", support::JsonValue{support::JsonValue::object_t{{"truncation", support::JsonValue{fields}}}});
        block.component.set_expanded(true);
        const auto rows = block.rows();
        CHECK(rows == std::vector<std::string>{"",
                              "read long.txt",
                              "",
                              "x",
                              "y",
                              "[Truncated: showing 2000 of 5231 lines (2000 line limit)]",
                              ""});
    }

    SECTION("no truncation object draws no warning") {
        ReadBlock block(keybindings, R"({"path":"notes.txt"})");
        block.succeed("x\ny");
        block.component.set_expanded(true);
        // Nothing beyond the body: a renderer that always drew a warning passes
        // "a warning is reachable" while breaking every ordinary read.
        CHECK(block.rows() == no_warning);
    }

    SECTION("a truncation object that is not truncated draws no warning") {
        ReadBlock block(keybindings, R"({"path":"notes.txt"})");
        block.succeed("x\ny", truncation_details(false, "lines", 2.0, 2.0, false, 2000.0, 50.0 * 1024.0));
        block.component.set_expanded(true);
        CHECK(block.rows() == no_warning);
    }

    SECTION("details without a truncation key draws no warning") {
        ReadBlock block(keybindings, R"({"path":"notes.txt"})");
        block.succeed("x\ny", support::JsonValue{support::JsonValue::object_t{{"diff", support::JsonValue{true}}}});
        block.component.set_expanded(true);
        CHECK(block.rows() == no_warning);
    }
}

TEST_CASE("the truncation warning follows the fold hint on the last body row",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    // pi appends the warning after the fold hint, so a collapsed error with a
    // truncation object shows ten lines, the hint, and then the warning. A
    // "the warning row exists" check passes with the two in either order.
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"missing.txt"})");
    block.fail(numbered_output(13), truncation_details(true, "bytes", 13.0, 13.0, false, 2000.0, 50.0 * 1024.0));

    const auto rows = block.rows();
    const std::vector<std::string> expected{
            "",
            "read missing.txt",
            "",
            "line 1",
            "line 2",
            "line 3",
            "line 4",
            "line 5",
            "line 6",
            "line 7",
            "line 8",
            "line 9",
            "line 10",
            "... (3 more lines, ctrl+o to expand)",
            "[Truncated: 13 lines shown (50.0KB limit)]",
            "",
    };
    CHECK(rows == expected);
}

TEST_CASE("an old session read whose marker is baked into the text adds no second warning line",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"notes.txt"})");
    // A session file written before `details.truncation` existed carries the
    // notice in the content and emits no details. The renderer must leave it
    // alone: it is ordinary text, and it is why no migration is needed.
    block.succeed("alpha\nbeta\n[output truncated]", /*details=*/std::nullopt);
    block.component.set_expanded(true);

    const auto rows = block.rows();
    // The baked-in marker is the last row and nothing follows it but the box's
    // own margin: a renderer that also inferred a truncation from the text
    // would pass "the marker is still visible" while adding a row.
    CHECK(rows == std::vector<std::string>{"", "read notes.txt", "", "alpha", "beta", "[output truncated]", ""});
    CHECK(!has_row(rows, "[Truncated: 3 lines shown (50.0KB limit)]"));
    CHECK(!has_row(rows, "[First line exceeds 50.0KB limit]"));
}

TEST_CASE("a read body's tabs render as three spaces in the tool-output colour",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto theme = tests::tool_render_theme();
    ReadBlock block(tests::tool_render_keybindings(), R"({"path":"notes.txt"})");
    block.succeed("a\tb");
    block.component.set_expanded(true);

    const auto screen = block.screen();
    REQUIRE(screen.visible.size() == 5);
    CHECK(screen.visible[3] == "a   b");
    // pi's `replaceTabs` maps a tab to three spaces and the whole line takes
    // the `toolOutput` token; a plain uncoloured row satisfies the visible
    // check above.
    CHECK(row_body(screen.raw[3]) == theme.foreground(coding_agent::tui::ThemeToken::ToolOutput, "a   b"));
}

TEST_CASE("read is resolved by name and draws the read title, where an unregistered tool falls back",
        "[coding_agent][tui][tool-renderers][read renderer][issue825][spec]") {
    const PlainCapabilities plain;
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    auto& read = registry.lookup("read");
    REQUIRE(static_cast<bool>(read.render_call));
    REQUIRE(static_cast<bool>(read.render_result));

    // The component keeps a reference to the theme, so it outlives it here.
    const coding_agent::tui::LiveTheme theme{tests::tool_render_theme()};
    coding_agent::tui::ToolExecutionComponent registered(theme,
            tests::tool_render_keybindings(),
            "read",
            "call_1",
            R"({"path":"notes.txt"})",
            "/workspace",
            std::move(registry));
    const auto registered_rows = tests::render_tool_screen(registered, 80).visible;
    // The registered pair draws the path; the fallback would draw the bold
    // name plus the argument JSON. The contrast is what shows the lookup is by
    // name rather than one pair for every tool.
    CHECK(registered_rows == std::vector<std::string>{"", "read notes.txt", ""});
    CHECK(!has_row(registered_rows, "\"path\": \"notes.txt\""));

    coding_agent::tui::ToolExecutionComponent unregistered(
            theme, tests::tool_render_keybindings(), "grep", "call_1", R"({"pattern":"alpha"})", "/workspace");
    const auto unregistered_rows = tests::render_tool_screen(unregistered, 80).visible;
    CHECK(has_row(unregistered_rows, "grep"));
    CHECK(has_row(unregistered_rows, "  \"pattern\": \"alpha\""));
}
