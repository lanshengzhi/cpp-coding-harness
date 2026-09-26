#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "coding_agent/tui/tool_renderers/PathPresentation.hpp"
#include "coding_agent/tui/tool_renderers/RenderUtils.hpp"
#include "coding_agent/tui/tool_renderers/ToolRendererRegistry.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/ImageCapabilitiesGuard.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::tui::ToolRenderContext render_context(const coding_agent::tui::LiveTheme& theme,
        const support::JsonValue& args,
        std::string_view tool_name = "custom_tool") {
    const std::string expand_key = "ctrl+o";
    const std::string expand_hint = theme.foreground(coding_agent::tui::ThemeToken::Dim, expand_key) +
                                    theme.foreground(coding_agent::tui::ThemeToken::Muted, " to expand");
    return coding_agent::tui::ToolRenderContext{
            .args = args,
            .tool_name = tool_name,
            .tool_call_id = "call_1",
            .cwd = "/workspace",
            .theme = theme,
            .expand_key = expand_key,
            .expand_hint = expand_hint,
            .args_complete = false,
            .execution_started = false,
            .is_partial = true,
            .is_error = false,
            .expanded = false,
            .started_at_ms = std::nullopt,
            .ended_at_ms = std::nullopt,
    };
}

[[nodiscard]] support::JsonValue parse(std::string_view json) {
    auto parsed = support::read_json(json);
    REQUIRE(parsed);
    return std::move(*parsed);
}

} // namespace

TEST_CASE("a registered tool is resolved by name and an unregistered one takes the fallback",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    auto theme = tests::tool_render_theme();
    const auto args = support::JsonValue::object_t{};

    // The registered pair draws its own (still empty) call text, while the
    // fallback draws the bold tool name: comparing the two is what shows the
    // lookup is by name rather than a single pair for every tool.
    auto& registered = registry.lookup("read");
    REQUIRE(static_cast<bool>(registered.render_call));
    CHECK(registered.render_call(render_context(theme, args)).title.empty());
    auto& unregistered = registry.lookup("grep");
    REQUIRE(static_cast<bool>(unregistered.render_call));
    CHECK(unregistered.render_call(render_context(theme, args, "grep")).title ==
            coding_agent::tui::bold_foreground(theme, coding_agent::tui::ThemeToken::ToolTitle, "grep"));
}

TEST_CASE("a registered tool with an empty call half still draws pi's bare bold tool name",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    registry.register_renderer("probe",
            coding_agent::tui::ToolRenderer{
                    .render_result =
                            [](const coding_agent::tui::ToolRenderedResult&,
                                    const coding_agent::tui::ToolRenderContext&) {
                                return coding_agent::tui::ToolRenderedText{};
                            },
            });
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "probe", "call_1", R"({"alpha":"beta"})", "/workspace", std::move(registry));

    const auto screen = tests::render_tool_screen(component, 80);

    // The host's call fallback, not the registry's: the resolved pair reports
    // an empty call half and the composed block is the bare bold tool name
    // with none of the fallback's argument JSON.
    CHECK(screen.visible == std::vector<std::string>{"", "probe", ""});
}

TEST_CASE("the four built-in tool names render through the component without a fallback body",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    for (const auto* name : {"read", "bash", "write", "edit"}) {
        coding_agent::tui::ToolExecutionComponent component(
                theme, keybindings, name, "call_1", R"({"file_path":"notes.txt"})", "/workspace");
        component.update_result(ai::ToolResultMessage{
                .tool_call_id = "call_1",
                .tool_name = name,
                .content = {ai::text_content("tool output")},
                .details = std::nullopt,
                .is_error = false,
                .timestamp = 0,
        });
        const auto screen = tests::render_tool_screen(component, 80);
        // No tool name takes the fallback: none of the four blocks shows the
        // fallback's bold bare name or its pretty-printed argument JSON. The
        // three whose renderer is still a stub render nothing at all, which is
        // pi's `hideComponent`; `bash` draws its own `$ ...` title since #826.
        std::string joined;
        for (const auto& row : screen.visible)
            joined += row + "\n";
        CHECK(joined.find("{\n") == std::string::npos);
        CHECK(joined.find("\"file_path\"") == std::string::npos);
        if (std::string_view{name} == "bash") {
            CHECK(!screen.visible.empty());
            CHECK(screen.visible.at(1) == "$ ...");
        } else {
            CHECK(screen.visible.empty());
        }
    }
}

TEST_CASE("mid-stream arguments never reach a tool title as a partial JSON fragment",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "custom_tool", "call_1", R"({"file_path":"/tmp/notes.txt","off)", "/workspace");

    // The arguments are still streaming: no fragment of the half-written
    // object reaches the title, and no character-by-character JSON grows in
    // its place.
    const auto incomplete = tests::render_tool_screen(component, 80);
    CHECK(incomplete.visible == std::vector<std::string>{"", "custom_tool", "", "{}", ""});

    component.update_args(R"({"file_path":"/tmp/notes.txt","offset":12})");

    // The complete arguments replace it wholesale.
    const auto complete = tests::render_tool_screen(component, 80);
    CHECK(complete.visible ==
            std::vector<std::string>{
                    "", "custom_tool", "", "{", "  \"file_path\": \"/tmp/notes.txt\",", "  \"offset\": 12", "}", ""});
}

TEST_CASE(
        "an argument that is not a string renders pi's invalid-arg text and an absent one renders the progressive dots",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    const auto not_a_string = parse(R"({"file_path":7})");
    const auto absent = parse(R"({})");
    const auto null_value = parse(R"({"file_path":null})");

    // Three distinct pi outcomes, so a single "shows something in the path
    // slot" check cannot stand in for any of them.
    CHECK(coding_agent::tui::string_argument(not_a_string, "file_path", "path") == std::nullopt);
    CHECK(coding_agent::tui::render_tool_path(
                  theme, "/workspace", coding_agent::tui::string_argument(not_a_string, "file_path", "path")) ==
            theme.foreground(coding_agent::tui::ThemeToken::Error, "[invalid arg]"));

    CHECK(coding_agent::tui::string_argument(absent, "file_path", "path") == std::string_view{});
    CHECK(coding_agent::tui::render_tool_path(
                  theme, "/workspace", coding_agent::tui::string_argument(absent, "file_path", "path")) ==
            theme.foreground(coding_agent::tui::ThemeToken::ToolOutput, "..."));

    // pi's `??` chain skips a null value and takes the next key.
    CHECK(coding_agent::tui::string_argument(null_value, "file_path", "path") == std::string_view{});
    const auto preferred = parse(R"({"file_path":null,"path":"notes.txt"})");
    CHECK(coding_agent::tui::string_argument(preferred, "file_path", "path") == "notes.txt");
}

TEST_CASE("a rendered path is accent-coloured, home-shortened, and linked only when the terminal supports it",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    const tests::EnvVarGuard home("HOME", "/home/agent");
    const tests::ImageCapabilitiesGuard linked({
            .images = cch::tui::InlineImageProtocol::None,
            .hyperlinks = true,
    });

    const auto displayed = theme.foreground(coding_agent::tui::ThemeToken::Accent, "~/notes.txt");
    // The display is shortened, the link target is not: the URL carries the
    // path resolved against the tool execution's cwd.
    CHECK(coding_agent::tui::render_tool_path(theme, "/workspace", "/home/agent/notes.txt") ==
            cch::tui::hyperlink(displayed, "file:///home/agent/notes.txt"));
    CHECK(coding_agent::tui::render_tool_path(theme, "/workspace", "notes.txt") ==
            cch::tui::hyperlink(theme.foreground(coding_agent::tui::ThemeToken::Accent, "notes.txt"),
                    "file:///workspace/notes.txt"));
}

TEST_CASE("a rendered path stays plain accent text when the terminal has no hyperlink support",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    const tests::EnvVarGuard home("HOME", "/home/agent");
    const tests::ImageCapabilitiesGuard plain({
            .images = cch::tui::InlineImageProtocol::None,
            .hyperlinks = false,
    });

    const auto rendered = coding_agent::tui::render_tool_path(theme, "/workspace", "/home/agent/notes.txt");
    CHECK(rendered == theme.foreground(coding_agent::tui::ThemeToken::Accent, "~/notes.txt"));
    CHECK(rendered.find("\x1b]8;;") == std::string::npos);
}

TEST_CASE("a path outside the home directory is not shortened and a bare home path is a lone tilde",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    auto theme = tests::tool_render_theme();
    const tests::EnvVarGuard home("HOME", "/home/agent");
    const tests::ImageCapabilitiesGuard plain({
            .images = cch::tui::InlineImageProtocol::None,
            .hyperlinks = false,
    });

    CHECK(coding_agent::tui::shorten_path("/srv/data/notes.txt") == "/srv/data/notes.txt");
    CHECK(coding_agent::tui::shorten_path("/home/agent") == "~");
    // pi tests `startsWith(home)` without a separator boundary, so a sibling
    // directory whose name merely begins with the home path shortens too. The
    // seam mirrors that rather than improving on it.
    CHECK(coding_agent::tui::shorten_path("/home/agentic/notes.txt") == "~ic/notes.txt");
}

TEST_CASE("the execution-start and args-complete transitions reach a renderer, and the clock is stamped once",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    struct Observation {
        bool execution_started{false};
        bool args_complete{false};
        bool is_partial{true};
        std::optional<std::int64_t> started_at_ms{std::nullopt};
        std::optional<std::int64_t> ended_at_ms{std::nullopt};
    };
    std::vector<Observation> observed;
    const auto observe = [&observed](const coding_agent::tui::ToolRenderContext& context) {
        observed.push_back(Observation{
                .execution_started = context.execution_started,
                .args_complete = context.args_complete,
                .is_partial = context.is_partial,
                .started_at_ms = context.started_at_ms,
                .ended_at_ms = context.ended_at_ms,
        });
        return coding_agent::tui::ToolRenderedText{};
    };
    const auto probe_call = observe;
    const auto probe_result = [](const coding_agent::tui::ToolRenderedResult&,
                                      const coding_agent::tui::ToolRenderContext&) {
        return coding_agent::tui::ToolRenderedText{};
    };
    auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
    registry.register_renderer("bash",
            coding_agent::tui::ToolRenderer{
                    .render_call = probe_call,
                    .render_result = probe_result,
            });
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "bash", "call_1", R"({"command":"ls -la"})", "/workspace", std::move(registry));

    // The whole transition sequence, not a single flag: a check that only
    // asked "did anything set execution_started" would pass while the stamp
    // was re-taken per render, the end was stamped on a partial result, or
    // the args-complete signal never arrived.
    REQUIRE(observed.size() == 1);
    CHECK(observed[0].execution_started == false);
    CHECK(observed[0].args_complete == false);
    CHECK(observed[0].started_at_ms == std::nullopt);
    CHECK(observed[0].ended_at_ms == std::nullopt);

    component.mark_execution_started();
    REQUIRE(observed.size() == 2);
    CHECK(observed[1].execution_started);
    REQUIRE(observed[1].started_at_ms.has_value());

    // The stamp is taken once: a second call neither re-renders nor moves the
    // instant the duration is measured from.
    component.mark_execution_started();
    CHECK(observed.size() == 2);

    component.set_args_complete();
    REQUIRE(observed.size() == 3);
    CHECK(observed[2].args_complete);

    const auto partial = ai::ToolResultMessage{
            .tool_call_id = "call_1",
            .tool_name = "bash",
            .content = {ai::text_content("running")},
            .details = std::nullopt,
            .is_error = false,
            .timestamp = 0,
    };
    component.update_result(partial, /*is_partial=*/true);
    REQUIRE(observed.size() == 4);
    CHECK(observed[3].is_partial);
    CHECK(observed[3].ended_at_ms == std::nullopt);

    component.update_result(partial, /*is_partial=*/false);
    REQUIRE(observed.size() == 5);
    CHECK(observed[4].is_partial == false);
    REQUIRE(observed[4].ended_at_ms.has_value());
    REQUIRE(observed[1].started_at_ms.has_value());
    CHECK(*observed[4].ended_at_ms >= *observed[1].started_at_ms);
}

TEST_CASE("the tool block a path-bearing renderer draws is hyperlink-gated on the terminal capability",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    // The shape #825 and #827 write: bold tool name, a space, then the shared
    // path presentation. Driving it through the component proves the host
    // hands the renderer its cwd, theme, and the live capability gate.
    const auto path_renderer = [](const coding_agent::tui::ToolRenderContext& context) {
        const auto path = coding_agent::tui::string_argument(context.args, "file_path", "path");
        return coding_agent::tui::ToolRenderedText{
                .title = coding_agent::tui::bold_foreground(
                                 context.theme, coding_agent::tui::ThemeToken::ToolTitle, context.tool_name) +
                         " " + coding_agent::tui::render_tool_path(context.theme, context.cwd, path),
        };
    };
    const auto render = [&path_renderer](bool hyperlinks) {
        auto theme = tests::tool_render_theme();
        auto keybindings = tests::tool_render_keybindings();
        const tests::EnvVarGuard home("HOME", "/home/agent");
        const tests::ImageCapabilitiesGuard capabilities({
                .images = cch::tui::InlineImageProtocol::None,
                .hyperlinks = hyperlinks,
        });
        auto registry = coding_agent::tui::ToolRendererRegistry::make_default();
        registry.register_renderer("read",
                coding_agent::tui::ToolRenderer{
                        .render_call = path_renderer,
                });
        coding_agent::tui::ToolExecutionComponent component(theme,
                keybindings,
                "read",
                "call_1",
                R"({"file_path":"/home/agent/notes.txt"})",
                "/workspace",
                std::move(registry));
        return tests::render_tool_screen(component, 80);
    };

    const auto linked = render(true);
    REQUIRE(linked.visible.size() == 3);
    CHECK(linked.visible[1] == "read ~/notes.txt");
    // The OSC 8 pair around the accent-coloured, home-shortened display, with
    // the resolved absolute path as the target: a `~/notes.txt` target would
    // satisfy a "has a hyperlink" check while pointing nowhere.
    const std::string_view open = "\x1b]8;;file:///home/agent/notes.txt\x1b\\";
    const std::string_view close = "\x1b]8;;\x1b\\";
    const auto open_at = linked.raw[1].find(open);
    REQUIRE(open_at != std::string::npos);
    const auto close_at = linked.raw[1].find(close, open_at + open.size());
    REQUIRE(close_at != std::string::npos);
    CHECK(cch::tui::strip_terminal_sequences(
                  linked.raw[1].substr(open_at + open.size(), close_at - (open_at + open.size()))) == "~/notes.txt");

    const auto plain = render(false);
    REQUIRE(plain.visible.size() == 3);
    CHECK(plain.visible[1] == "read ~/notes.txt");
    CHECK(plain.raw[1].find("\x1b]8;;") == std::string::npos);
}

TEST_CASE("the fold helper reports the same remaining count pi's trim-and-slice fold does",
        "[coding_agent][tui][tool-renderers][issue824][spec]") {
    // Trailing empty lines are dropped before the count, so a body ending in a
    // blank line does not inflate the "more lines" number.
    const auto trailing = coding_agent::tui::fold_head_lines("a\nb\n\n\n", 10);
    CHECK(trailing.lines == std::vector<std::string>{"a", "b"});
    CHECK(trailing.remaining == 0);

    const auto folded = coding_agent::tui::fold_head_lines("a\nb\nc\nd\ne\n", 2);
    CHECK(folded.lines == std::vector<std::string>{"a", "b"});
    CHECK(folded.remaining == 3);

    const auto empty = coding_agent::tui::fold_head_lines("", 10);
    CHECK(empty.lines.empty());
    CHECK(empty.remaining == 0);
}
