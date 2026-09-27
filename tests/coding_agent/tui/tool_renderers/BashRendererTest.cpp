#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "coding_agent/tui/tool_renderers/BashRenderer.hpp"
#include "coding_agent/tui/tool_renderers/RenderUtils.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"
#include "support/Json.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

/// The screen width every case renders at. The block's box takes one column
/// each side, so a renderer measures 78.
constexpr std::size_t kScreenWidth = 80;
/// The width the renderer therefore measures.
constexpr std::size_t kContentWidth = kScreenWidth - 2;

[[nodiscard]] std::vector<std::string> numbered_rows(std::size_t first, std::size_t last) {
    std::vector<std::string> rows;
    for (auto line = first; line <= last; ++line)
        rows.push_back(std::format("line {}", line));
    return rows;
}

[[nodiscard]] std::string numbered_output(std::size_t first, std::size_t last) {
    std::string output;
    for (auto line = first; line <= last; ++line) {
        if (!output.empty()) output.push_back('\n');
        output += std::format("line {}", line);
    }
    return output;
}

[[nodiscard]] support::JsonValue bash_details(
        std::string_view truncation_object, std::string_view full_output_path = {}) {
    auto parsed = support::read_json(truncation_object);
    REQUIRE(parsed);
    support::JsonValue::object_t object{{"truncation", std::move(*parsed)}};
    if (!full_output_path.empty()) {
        object.emplace("fullOutputPath", support::JsonValue(std::string{full_output_path}));
    }
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] ai::ToolResultMessage bash_result(
        std::string output, std::optional<support::JsonValue> result_details = std::nullopt, bool is_error = false) {
    return ai::ToolResultMessage{
            .tool_call_id = "call_1",
            .tool_name = "bash",
            .content = {ai::text_content(std::move(output))},
            .details = std::move(result_details),
            .is_error = is_error,
            .timestamp = 0,
    };
}

/// A component with a started execution and a settled result, the state every
/// result case renders from.
[[nodiscard]] std::unique_ptr<coding_agent::tui::ToolExecutionComponent> settled(
        const coding_agent::tui::LiveTheme& theme,
        const std::shared_ptr<const coding_agent::tui::SharedKeybindings>& keybindings,
        std::string arguments,
        ai::ToolResultMessage result) {
    auto component = std::make_unique<coding_agent::tui::ToolExecutionComponent>(
            theme, keybindings, "bash", "call_1", std::move(arguments), "/workspace");
    component->mark_execution_started();
    component->update_result(std::move(result));
    return component;
}

/// The block without its two fixed trailing rows: the box's bottom padding and
/// pi's `Took` line. The duration row is asserted on its own so a block's
/// shape can be compared whole without freezing the measured run width.
[[nodiscard]] std::vector<std::string> body_of(const tests::ToolRenderScreen& screen) {
    auto rows = screen.visible;
    if (rows.size() >= 2 && rows.back().empty()) rows.pop_back();
    if (!rows.empty() && rows.back().starts_with("Took ")) rows.pop_back();
    return rows;
}

/// A render context the test drives directly, for the states a live component
/// cannot reach: a chosen argument object, a chosen execution clock, and a
/// chosen frame width.
///
/// The context's `args` and `expand_hint` are references, so this owns both
/// for the lifetime of the context it hands out; a bare function returning a
/// context would leave every view it copied pointing at a destroyed temporary.
struct Direct {
    explicit Direct(const coding_agent::tui::LiveTheme& theme)
        : theme_(theme), args(support::JsonValue::object_t{}),
          expand_hint(theme.foreground(coding_agent::tui::ThemeToken::Dim, "ctrl+o") +
                      theme.foreground(coding_agent::tui::ThemeToken::Muted, " to expand")) {}

    [[nodiscard]] coding_agent::tui::ToolRenderContext context(std::optional<std::int64_t> started_at_ms = std::nullopt,
            std::optional<std::int64_t> ended_at_ms = std::nullopt,
            std::size_t width = kContentWidth) const {
        return coding_agent::tui::ToolRenderContext{
                .args = args,
                .tool_name = "bash",
                .tool_call_id = "call_1",
                .cwd = "/workspace",
                .width = width,
                .theme = theme_,
                .expand_key = "ctrl+o",
                .expand_hint = expand_hint,
                .args_complete = true,
                .execution_started = started_at_ms.has_value(),
                .is_partial = false,
                .is_error = false,
                .expanded = false,
                .started_at_ms = started_at_ms,
                .ended_at_ms = ended_at_ms,
        };
    }

    const coding_agent::tui::LiveTheme& theme_;
    support::JsonValue args;
    std::string expand_hint;
};

[[nodiscard]] std::string rendered_result(
        const coding_agent::tui::ToolRenderedResult& result, const coding_agent::tui::ToolRenderContext& context) {
    auto renderer = coding_agent::tui::make_bash_renderer();
    const auto rendered = renderer.render_result(result, context);
    std::string text;
    for (const auto& block : rendered.blocks)
        text += block;
    return tui::strip_terminal_sequences(text);
}

} // namespace

TEST_CASE("the bash call title is the command, and the timeout suffix appears only when a timeout is set",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();

    SECTION("no timeout draws the command alone") {
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, "bash", "call_1", R"({"command":"ls -la"})", "/workspace");
        const auto screen = tests::render_tool_screen(component, kScreenWidth);
        // The whole row, not a `find("ls -la")`: a title that kept a stale
        // suffix, or drew a second row, passes the substring check.
        CHECK(screen.visible == std::vector<std::string>{"", "$ ls -la", ""});
    }

    SECTION("a timeout adds pi's muted suffix outside the title styling") {
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, "bash", "call_1", R"({"command":"ls -la","timeout":30})", "/workspace");
        const auto screen = tests::render_tool_screen(component, kScreenWidth);
        CHECK(screen.visible == std::vector<std::string>{"", "$ ls -la (timeout 30s)", ""});
        // The suffix is `muted` and not part of the bold `toolTitle` span: the
        // title's bold close precedes it.
        CHECK(screen.raw.at(1).find("\x1b[1m$ ls -la\x1b[22m") != std::string::npos);
        CHECK(screen.raw.at(1).find("\x1b[1m$ ls -la (timeout 30s)") == std::string::npos);
    }

    SECTION("a zero timeout prints no suffix, because pi tests it for truthiness") {
        auto parsed = support::read_json(R"({"command":"ls -la","timeout":0})");
        REQUIRE(parsed);
        Direct direct{theme};
        direct.args = std::move(*parsed);
        auto renderer = coding_agent::tui::make_bash_renderer();
        CHECK(tui::strip_terminal_sequences(renderer.render_call(direct.context()).title) == "$ ls -la");
    }
}

TEST_CASE("the bash command slot shows pi's placeholder and its invalid-argument text",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();

    SECTION("arguments still streaming render the command slot as ...") {
        // A half-streamed argument object is not valid JSON, so the host hands
        // the renderer an empty argument object.
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, "bash", "call_1", R"({"command":"ls -la","time)", "/workspace");
        const auto screen = tests::render_tool_screen(component, kScreenWidth);
        CHECK(screen.visible == std::vector<std::string>{"", "$ ...", ""});
    }

    SECTION("a present but non-string command renders [invalid arg] inside the title") {
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, "bash", "call_1", R"({"command":42})", "/workspace");
        const auto screen = tests::render_tool_screen(component, kScreenWidth);
        CHECK(screen.visible == std::vector<std::string>{"", "$ [invalid arg]", ""});
    }

    SECTION("an empty command string is the placeholder, not a blank title") {
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, "bash", "call_1", R"({"command":""})", "/workspace");
        const auto screen = tests::render_tool_screen(component, kScreenWidth);
        CHECK(screen.visible == std::vector<std::string>{"", "$ ...", ""});
    }
}

TEST_CASE("a collapsed bash result keeps the last five visual lines and drops the first five",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(numbered_output(1, 12)));

    const auto screen = tests::render_tool_screen(*component, kScreenWidth);

    // The whole block: the blank row, the hint above the tail, the five kept
    // rows. A head fold draws the same number of rows in the same places, so
    // the row-by-row equality is what separates the two.
    CHECK(body_of(screen) == std::vector<std::string>{"",
                                     "$ ls",
                                     "",
                                     "... (7 earlier lines, ctrl+o to expand)",
                                     "line 8",
                                     "line 9",
                                     "line 10",
                                     "line 11",
                                     "line 12"});

    // The twin of a length check: the first line of the output is absent and
    // the last two are present, so "folded to the tail" cannot pass as
    // "rendered something".
    std::string joined;
    for (const auto& row : screen.visible)
        joined += row + "\n";
    CHECK(joined.find("line 1\n") == std::string::npos);
    CHECK(joined.find("line 8") != std::string::npos);
    CHECK(joined.find("line 12") != std::string::npos);
}

TEST_CASE("the earlier-lines hint counts visual lines and names the resolved expand key",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();

    SECTION("a bound key renders pi's key hint") {
        auto keybindings = tests::tool_render_keybindings();
        auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(numbered_output(1, 12)));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        CHECK(screen.visible.at(3) == "... (7 earlier lines, ctrl+o to expand)");
    }

    SECTION("nothing bound renders Unbound in the same slot") {
        auto keybindings = tests::tool_render_keybindings(false);
        auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(numbered_output(1, 12)));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        CHECK(screen.visible.at(3) == "... (7 earlier lines, Unbound to expand)");
    }

    SECTION("a hint wider than the frame is truncated with an ellipsis, so it cannot wrap") {
        // A wrapped hint would add a row and change the block's height, which
        // a check that only reads the count misses.
        const coding_agent::tui::ToolRenderedResult result{
                .output = numbered_output(1, 200),
                .details = std::nullopt,
        };
        Direct direct{theme};
        const auto rendered = rendered_result(result, direct.context(std::nullopt, std::nullopt, 30));
        CHECK(rendered == "\n... (195 earlier lines, ctr...\nline 196\nline 197\nline 198\nline 199\nline 200");
    }
}

TEST_CASE("a wrapped output line is one visual line, so the hint counts it as one",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // Six short lines and one 100-column line, which wraps to two visual rows
    // at the block's 78-column content width: eight visual lines, of which the
    // last five are `line 4`, `line 5`, `line 6` and the two wrapped rows.
    auto output = numbered_output(1, 6);
    output += "\n" + std::string(100, 'x');
    auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(std::move(output)));

    const auto screen = tests::render_tool_screen(*component, kScreenWidth);

    // A logical fold drops one line and says so; the visual fold drops three
    // rows, so the count itself is the assertion that separates them.
    CHECK(screen.visible.at(3) == "... (3 earlier lines, ctrl+o to expand)");
    CHECK(screen.visible.at(4) == "line 4");
    CHECK(screen.visible.at(6) == "line 6");
    CHECK(screen.visible.at(7) == std::string(kContentWidth, 'x'));
    CHECK(screen.visible.at(8) == std::string(100 - kContentWidth, 'x'));
    CHECK(body_of(screen).size() == 9);
    // The head of the output, wrapped and whole, is gone.
    std::string joined;
    for (const auto& row : screen.visible)
        joined += row + "\n";
    CHECK(joined.find("line 1\n") == std::string::npos);
    CHECK(joined.find("line 3\n") == std::string::npos);
}

TEST_CASE("a folded result of five visual lines or fewer draws the tail with no hint at all",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(numbered_output(1, 5)));

    const auto screen = tests::render_tool_screen(*component, kScreenWidth);

    // pi's `["", ...lines]`: the leading blank row survives, the hint does not.
    // A hint that is always drawn, or never drawn, passes a check that only
    // counts output rows.
    CHECK(body_of(screen) ==
            std::vector<std::string>{"", "$ ls", "", "line 1", "line 2", "line 3", "line 4", "line 5"});
}

TEST_CASE("an expanded bash result draws the whole output and no fold hint",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto component = std::make_unique<coding_agent::tui::ToolExecutionComponent>(
            theme, keybindings, "bash", "call_1", R"({"command":"ls"})", "/workspace");
    component->mark_execution_started();
    component->update_result(bash_result(numbered_output(1, 12)));
    component->set_expanded(true);

    const auto screen = tests::render_tool_screen(*component, kScreenWidth);

    CHECK(body_of(screen) == std::vector<std::string>{"",
                                     "$ ls",
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
                                     "line 12"});
    for (const auto& row : screen.visible)
        CHECK(row.find("earlier lines") == std::string::npos);
}

TEST_CASE("a settled bash result renders pi's Took line from the execution clock",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();

    SECTION("a live execution renders a duration after the output") {
        auto keybindings = tests::tool_render_keybindings();
        auto component = settled(theme, keybindings, R"({"command":"sleep 1"})", bash_result("done"));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        REQUIRE(screen.visible.size() == 6);
        CHECK(screen.visible.at(3) == "done");
        CHECK(screen.visible.at(4).starts_with("Took "));
        CHECK(screen.visible.at(4).size() > std::string_view{"Took "}.size());
    }

    SECTION("each of pi's duration shapes is exact, including the rollovers") {
        const coding_agent::tui::ToolRenderedResult result{.output = "done", .details = std::nullopt};
        Direct direct{theme};
        // A cheap check is "the row starts with Took", which a hundred wrong
        // formats pass; each line is asserted whole.
        CHECK(rendered_result(result, direct.context(1'000'000, 1'000'400)) == "\ndone\nTook 0.4s");
        CHECK(rendered_result(result, direct.context(1'000'000, 1'012'345)) == "\ndone\nTook 12.3s");
        CHECK(rendered_result(result, direct.context(1'000'000, 1'125'000)) == "\ndone\nTook 2m 5s");
        CHECK(rendered_result(result, direct.context(1'000'000, 4'723'000)) == "\ndone\nTook 1h 2m 3s");
    }

    SECTION("no start stamp means no duration line, because a snapshot-rebuilt tool never saw the start") {
        // Fabricating a start would print a plausible but invented number, so
        // the block carries no duration row at all.
        const coding_agent::tui::ToolRenderedResult result{.output = "done", .details = std::nullopt};
        Direct direct{theme};
        CHECK(rendered_result(result, direct.context()) == "\ndone");
        CHECK(rendered_result(result, direct.context(1'000'000)) == "\ndone");
        CHECK(rendered_result(result, direct.context(std::nullopt, 2'000'000)) == "\ndone");
    }
}

TEST_CASE("the truncation warning draws pi's bracketed forms with the path verbatim",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();

    SECTION("a spill path with no truncation still warns, in one bracket pair") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done", bash_details(R"({"truncated":false})", "/tmp/cch-bash-ab12.log")));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        CHECK(body_of(screen) ==
                std::vector<std::string>{"", "$ ls", "", "done", "[Full output: /tmp/cch-bash-ab12.log]"});
    }

    SECTION("line truncation reports the shown and total line counts") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done",
                        bash_details(R"({"truncated":true,"truncatedBy":"lines","outputLines":2000,"totalLines":5231})",
                                "/tmp/cch-bash-ab12.log")));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        // The whole block, not `.at(4)`: a renderer that also emitted a second
        // warning row satisfies "the warning is present" and every `.at(n)`
        // check, and the extra row is exactly what a duplicate-warning defect
        // looks like on screen. One warning row, and nothing after it.
        CHECK(body_of(screen) == std::vector<std::string>{"",
                                         "$ ls",
                                         "",
                                         "done",
                                         "[Full output: /tmp/cch-bash-ab12.log. Truncated: showing 2000 of 5231 "
                                         "lines]"});
        CHECK(screen.visible.at(4) == "[Full output: /tmp/cch-bash-ab12.log. Truncated: showing 2000 of 5231 lines]");
    }

    SECTION("byte truncation reports the line count and the size limit") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done",
                        bash_details(R"({"truncated":true,"truncatedBy":"bytes","outputLines":812,"maxBytes":51200})",
                                "/tmp/b.log")));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        // A short path so pi's whole warning fits the 78-column content width;
        // the long form is the same text, wrapped, which the next case shows.
        // The whole block again, so a second warning row fails here too.
        CHECK(body_of(screen) == std::vector<std::string>{"",
                                         "$ ls",
                                         "",
                                         "done",
                                         "[Full output: /tmp/b.log. Truncated: 812 lines shown (50.0KB limit)]"});
        CHECK(screen.visible.at(4) == "[Full output: /tmp/b.log. Truncated: 812 lines shown (50.0KB limit)]");
    }

    SECTION("a byte-truncated result with no limit of its own falls back to pi's 50KB default") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done",
                        bash_details(R"({"truncated":true,"truncatedBy":"bytes","outputLines":812})", "/tmp/b.log")));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        CHECK(screen.visible.at(4) == "[Full output: /tmp/b.log. Truncated: 812 lines shown (50.0KB limit)]");
    }

    SECTION("a warning longer than the frame wraps like any other text, without losing a character") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done",
                        bash_details(R"({"truncated":true,"truncatedBy":"bytes","outputLines":812,"maxBytes":51200})",
                                "/tmp/cch-bash-ab12.log")));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        REQUIRE(screen.visible.size() == 8);
        CHECK(screen.visible.at(4) == "[Full output: /tmp/cch-bash-ab12.log. Truncated: 812 lines shown (50.0KB");
        CHECK(screen.visible.at(5) == "limit)]");
    }

    SECTION("the path is verbatim: not shortened, not hyperlinked, not accented") {
        const auto* home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        const auto spill = std::string{home} + "/cch-bash-ab12.log";
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done", bash_details(R"({"truncated":false})", spill)));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        // A check that only asks whether the path appears somewhere passes
        // with a `~` for the home prefix or inside an OSC 8 sequence; the
        // bracketed row is asserted here instead. This spill path is longer
        // than the frame, so pi's `Text` wraps it after the label and the two
        // rows are asserted whole.
        REQUIRE(screen.visible.size() == 8);
        CHECK(screen.visible.at(4) == "[Full output:");
        CHECK(screen.visible.at(5) == spill + "]");
        CHECK(screen.raw.at(4).find("\x1b]8;;") == std::string::npos);
        CHECK(screen.raw.at(5).find("\x1b]8;;") == std::string::npos);
    }

    SECTION("no details draws no warning, which is what keeps an old session's baked-in marker plain text") {
        auto component = settled(theme,
                keybindings,
                R"({"command":"ls"})",
                bash_result("done\n\n[Showing lines 1-5 of 9. Full output: /tmp/old.log]"));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        // Both halves, or the case is worthless: the whole block shows the
        // baked-in marker still on screen as ordinary output, and shows no row
        // added for it. "No `Truncated:` substring" alone passes trivially for
        // a renderer that dropped the output.
        CHECK(body_of(screen) ==
                std::vector<std::string>{
                        "", "$ ls", "", "done", "", "[Showing lines 1-5 of 9. Full output: /tmp/old.log]"});
        CHECK(screen.visible.at(5) == "[Showing lines 1-5 of 9. Full output: /tmp/old.log]");
        for (const auto& row : screen.visible)
            CHECK(row.find("Truncated:") == std::string::npos);
    }

    SECTION("a truncation with no spill path warns about the truncation alone") {
        const coding_agent::tui::ToolRenderedResult result{
                .output = "done",
                .details = bash_details(R"({"truncated":true,"truncatedBy":"lines","outputLines":2,"totalLines":9})"),
        };
        Direct direct{theme};
        CHECK(rendered_result(result, direct.context()) == "\ndone\n[Truncated: showing 2 of 9 lines]");
    }
}

TEST_CASE("a settled result's own spill-path summary is stripped once the warning line owns it",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // Exactly the text `AsyncToolFactories` bakes into the model-facing
    // output: the tail, then a `\n\n`-separated summary naming the spill file.
    const auto baked = std::string{"line 8\nline 9\n\n[Showing lines 8-9 of 9. Full output: /tmp/cch-bash-ab12.log]"};
    const auto result_details = bash_details(
            R"({"truncated":true,"truncatedBy":"lines","outputLines":2,"totalLines":9})", "/tmp/cch-bash-ab12.log");

    SECTION("collapsed and settled, the baked-in footer is gone and the warning replaces it") {
        auto component = settled(theme, keybindings, R"({"command":"ls"})", bash_result(baked, result_details));
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);
        CHECK(body_of(screen) == std::vector<std::string>{"",
                                         "$ ls",
                                         "",
                                         "line 8",
                                         "line 9",
                                         "[Full output: /tmp/cch-bash-ab12.log. Truncated: showing 2 "
                                         "of 9 lines]"});
    }

    SECTION("expanded, the footer is still stripped, because pi's condition has no expanded clause") {
        // `bash.ts:64` gates the strip on `isPartial`, `truncated`, a path, and
        // an output ending in `]` — `expanded` is not among them, so the
        // expanded view shows the output without the baked-in footer too. A
        // check that only looks for the footer being gone in the folded view
        // passes an implementation that strips per branch.
        const auto long_baked =
                numbered_output(1, 12) + "\n\n[Showing lines 8-9 of 9. Full output: /tmp/cch-bash-ab12.log]";
        auto component = std::make_unique<coding_agent::tui::ToolExecutionComponent>(
                theme, keybindings, "bash", "call_1", R"({"command":"ls"})", "/workspace");
        component->mark_execution_started();
        component->update_result(bash_result(long_baked, result_details));
        component->set_expanded(true);
        const auto screen = tests::render_tool_screen(*component, kScreenWidth);

        std::vector<std::string> expected{"", "$ ls", ""};
        for (const auto& row : numbered_rows(1, 12))
            expected.push_back(row);
        expected.push_back("[Full output: /tmp/cch-bash-ab12.log. Truncated: showing 2 of 9 lines]");
        CHECK(body_of(screen) == expected);
    }

    SECTION("an unrelated bracketed line is never eaten") {
        // The last `\n\n[` does not name the spill path, so pi leaves the
        // output alone; stripping it would hide real tool output.
        const coding_agent::tui::ToolRenderedResult result{
                .output = "line 8\nline 9\n\n[note: the file was listed]",
                .details = result_details,
        };
        Direct direct{theme};
        CHECK(rendered_result(result, direct.context()) ==
                "\nline 8\nline 9\n\n[note: the file was listed]\n[Full output: /tmp/cch-bash-ab12.log. Truncated: "
                "showing 2 of 9 lines]");
    }

    SECTION("a partial result keeps its footer, because the settle condition has not happened") {
        Direct direct{theme};
        auto context = direct.context(1'000'000);
        context.is_partial = true;
        const coding_agent::tui::ToolRenderedResult result{.output = baked, .details = result_details};
        const auto rendered = rendered_result(result, context);
        CHECK(rendered.find("[Showing lines 8-9 of 9. Full output: /tmp/cch-bash-ab12.log]") != std::string::npos);
        CHECK(rendered.find("earlier lines") == std::string::npos);
    }
}

TEST_CASE("a non-zero-exit result renders its text on an error block",
        "[coding_agent][tui][tool-renderers][issue826][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto failed = settled(theme, keybindings, R"({"command":"ls"})", bash_result("no such file", std::nullopt, true));
    auto succeeded = settled(theme, keybindings, R"({"command":"ls"})", bash_result("no such file"));

    const auto error_screen = tests::render_tool_screen(*failed, kScreenWidth);
    const auto success_screen = tests::render_tool_screen(*succeeded, kScreenWidth);

    // pi's bash result half has no error-colour branch (bash.ts:74-106 styles
    // every line `toolOutput`); the failure shows as the block's `toolErrorBg`
    // background the host already applies. Both halves of that are asserted:
    // the text is there, and the composed block differs from the success one.
    CHECK(error_screen.visible.at(3) == "no such file");
    CHECK(error_screen.visible == success_screen.visible);
    CHECK(error_screen.raw != success_screen.raw);
}
