// The scoped-models selector (pi `scoped-models-selector.ts`): enables,
// disables, and reorders the models Ctrl+P cycles through, with the six
// `app.models.*` actions bound inside it and session-only changes persisted
// only through `app.models.save`. Component-level coverage of the toggle /
// enable-all / clear-all / toggle-provider / reorder / save actions, the
// footer counts, and cancellation.

#include "coding_agent/tui/ScopedModelsSelector.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    const auto app_action = [&request](std::string_view id, std::vector<std::string> keys) {
        request.definitions.push_back({
            .id = std::string{id},
            .default_keys = std::move(keys),
            .description = {},
            .category = {},
            .available = true,
            .unavailable_reason = std::nullopt,
        });
    };
    app_action("app.models.save", {"ctrl+s"});
    app_action("app.models.enableAll", {"ctrl+a"});
    app_action("app.models.clearAll", {"ctrl+x"});
    app_action("app.models.toggleProvider", {"ctrl+p"});
    app_action("app.models.reorderUp", {"alt+up"});
    app_action("app.models.reorderDown", {"alt+down"});
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] ai::Model model(std::string id, std::string provider) {
    ai::Model result;
    result.id = std::move(id);
    result.provider = std::move(provider);
    return result;
}

/// alpha-1, alpha-2 (provider alpha) and beta-1 (provider beta).
[[nodiscard]] std::vector<ai::Model> catalog() {
    return {
        model("alpha-1", "alpha"),
        model("alpha-2", "alpha"),
        model("beta-1", "beta"),
    };
}

struct Recorder {
    std::optional<std::vector<std::string>> last_change{std::nullopt};
    std::optional<std::vector<std::string>> last_persist{std::nullopt};
    std::size_t cancellations{0};
    bool changed{false};
    bool persisted{false};
};

[[nodiscard]] coding_agent::tui::ScopedModelsSelectorComponent make_selector(
    const coding_agent::tui::LiveTheme& theme,
    Recorder& recorder,
    std::optional<std::vector<std::string>> enabled_ids = std::nullopt) {
    return coding_agent::tui::ScopedModelsSelectorComponent(
        theme,
        test_keybindings(),
        catalog(),
        std::move(enabled_ids),
        [&recorder](std::optional<std::vector<std::string>> ids) {
            recorder.changed = true;
            recorder.last_change = std::move(ids);
        },
        [&recorder](std::optional<std::vector<std::string>> ids) {
            recorder.persisted = true;
            recorder.last_persist = std::move(ids);
        },
        [&recorder] { ++recorder.cancellations; });
}

} // namespace

TEST_CASE(
    "ScopedModelsSelector renders the configuration header, list, and all-enabled footer",
    "[coding_agent][tui][scoped-models][issue407]") {
    auto theme = test_theme();
    Recorder recorder;
    auto selector = make_selector(theme, recorder);

    const auto rendered = selector.render(90);
    REQUIRE(rendered);
    const auto screen = join_lines(rendered->lines);
    CHECK(screen.find("Model Configuration") != std::string::npos);
    CHECK(screen.find("Session-only.") != std::string::npos);
    // All enabled: no ✓/✗ markers and the "all enabled" count.
    CHECK(screen.find("alpha-1 [alpha]") != std::string::npos);
    CHECK(screen.find("all enabled") != std::string::npos);
    CHECK(screen.find("toggle") != std::string::npos);
    CHECK(screen.find("save") != std::string::npos);
    CHECK_FALSE(recorder.changed);
    CHECK_FALSE(recorder.persisted);
}

TEST_CASE(
    "ScopedModelsSelector toggles on Enter starting an explicit list, with counts and the unsaved marker",
    "[coding_agent][tui][scoped-models][issue407]") {
    auto theme = test_theme();
    Recorder recorder;
    auto selector = make_selector(theme, recorder);

    // First toggle on the all-enabled state starts an explicit list with
    // only the selected id (pi `toggle`).
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(recorder.changed);
    REQUIRE(recorder.last_change.has_value());
    REQUIRE(recorder.last_change->size() == 1);
    CHECK((*recorder.last_change)[0] == "alpha/alpha-1");

    {
        const auto rendered = selector.render(90);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("1/3 enabled") != std::string::npos);
        CHECK(screen.find("(unsaved)") != std::string::npos);
        CHECK(screen.find("alpha-1 [alpha] ✓") != std::string::npos);
        CHECK(screen.find("alpha-2 [alpha] ✗") != std::string::npos);
    }

    // Toggle again removes it from the explicit list.
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(recorder.last_change.has_value());
    CHECK(recorder.last_change->empty());

    // Saving fires onPersist with the explicit list and clears the dirty
    // marker (pi `app.models.save`).
    selector.handle_input(tui::KeyEvent{.key = "s", .ctrl = true});
    REQUIRE(recorder.persisted);
    REQUIRE(recorder.last_persist.has_value());
    CHECK(recorder.last_persist->empty());
    {
        const auto rendered = selector.render(90);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("(unsaved)") == std::string::npos);
    }
}

TEST_CASE(
    "ScopedModelsSelector enable-all and clear-all act on the filtered set when searching",
    "[coding_agent][tui][scoped-models][issue407]") {
    auto theme = test_theme();
    Recorder recorder;
    // Start with only alpha-1 enabled.
    auto selector = make_selector(theme, recorder, std::vector<std::string>{"alpha/alpha-1"});

    // Search narrows the list to alpha-2.
    selector.handle_input(tui::KeyEvent{.key = "2"});
    {
        const auto rendered = selector.render(90);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("alpha-2 [alpha]") != std::string::npos);
        CHECK(screen.find("alpha-1 [alpha]") == std::string::npos);
    }
    // Ctrl+A enables the filtered item only.
    selector.handle_input(tui::KeyEvent{.key = "a", .ctrl = true});
    REQUIRE(recorder.last_change.has_value());
    REQUIRE(recorder.last_change->size() == 2);
    CHECK((*recorder.last_change)[0] == "alpha/alpha-1");
    CHECK((*recorder.last_change)[1] == "alpha/alpha-2");

    // Ctrl+X clears the filtered item only.
    selector.handle_input(tui::KeyEvent{.key = "x", .ctrl = true});
    REQUIRE(recorder.last_change.has_value());
    REQUIRE(recorder.last_change->size() == 1);
    CHECK((*recorder.last_change)[0] == "alpha/alpha-1");

    // Ctrl+A with no search enables everything and normalizes back to the
    // null (all-enabled) state (pi `enableAll`).
    selector.handle_input(tui::KeyEvent{.key = "c", .ctrl = true});  // clear search
    selector.handle_input(tui::KeyEvent{.key = "a", .ctrl = true});
    CHECK(recorder.last_change == std::nullopt);

    // Ctrl+X with no search clears everything to an explicit empty list.
    selector.handle_input(tui::KeyEvent{.key = "x", .ctrl = true});
    REQUIRE(recorder.last_change.has_value());
    CHECK(recorder.last_change->empty());
}

TEST_CASE(
    "ScopedModelsSelector toggles whole providers and reorders enabled models",
    "[coding_agent][tui][scoped-models][issue407]") {
    auto theme = test_theme();
    Recorder recorder;
    auto selector = make_selector(theme, recorder, std::vector<std::string>{"alpha/alpha-1"});

    // Select alpha-2 (index 1) and toggle its provider: alpha is not fully
    // enabled, so Ctrl+P enables both alpha models.
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "p", .ctrl = true});
    REQUIRE(recorder.last_change.has_value());
    REQUIRE(recorder.last_change->size() == 2);
    CHECK((*recorder.last_change)[0] == "alpha/alpha-1");
    CHECK((*recorder.last_change)[1] == "alpha/alpha-2");

    // Move to beta-1 and toggle its provider: beta is fully disabled, so
    // Ctrl+P enables it too — and normalizes back to the null (all-enabled)
    // state because every model is now enabled (pi `enableAll`).
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "p", .ctrl = true});
    CHECK(recorder.last_change == std::nullopt);

    // Ctrl+P again on beta (still selected; all beta models enabled) clears
    // the whole provider back to the explicit alpha list.
    selector.handle_input(tui::KeyEvent{.key = "p", .ctrl = true});
    REQUIRE(recorder.last_change.has_value());
    REQUIRE(recorder.last_change->size() == 2);
    CHECK((*recorder.last_change)[0] == "alpha/alpha-1");
    CHECK((*recorder.last_change)[1] == "alpha/alpha-2");

    // The selection stays on the now-disabled beta-1 (pi: reordering a
    // disabled item is a no-op); move up to alpha-2, then Alt+Up reorders it
    // ahead of alpha-1.
    selector.handle_input(tui::KeyEvent{.key = "up"});
    selector.handle_input(tui::KeyEvent{.key = "up", .alt = true});
    REQUIRE(recorder.last_change.has_value());
    CHECK((*recorder.last_change)[0] == "alpha/alpha-2");
    CHECK((*recorder.last_change)[1] == "alpha/alpha-1");

    // Reordering at the top is a no-op.
    selector.handle_input(tui::KeyEvent{.key = "up", .alt = true});
    REQUIRE(recorder.last_change.has_value());
    CHECK((*recorder.last_change)[0] == "alpha/alpha-2");
}

TEST_CASE(
    "ScopedModelsSelector cancels on Escape and Ctrl+C clears the search first",
    "[coding_agent][tui][scoped-models][issue407]") {
    auto theme = test_theme();
    Recorder recorder;
    auto selector = make_selector(theme, recorder);

    selector.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(recorder.cancellations == 1);

    // Ctrl+C with a search clears the search instead of cancelling.
    selector.handle_input(tui::KeyEvent{.key = "b"});
    selector.handle_input(tui::KeyEvent{.key = "c", .ctrl = true});
    CHECK(recorder.cancellations == 1);
    {
        const auto rendered = selector.render(90);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("beta-1 [beta]") != std::string::npos);
    }
    // Ctrl+C with an empty search cancels.
    selector.handle_input(tui::KeyEvent{.key = "c", .ctrl = true});
    CHECK(recorder.cancellations == 2);
}

/// Every emitted line must fit the render width bound exactly: the TUI render
/// path asserts each line's visible width <= the bound and aborts the whole
/// app on a single over-wide line (issue #426). The scoped-models selector
/// used to emit raw, untruncated lines — this pins the width-boundary
/// behavior.
static void check_all_lines_bounded(const tui::RenderResult& rendered, std::size_t width) {
    REQUIRE_FALSE(rendered.lines.empty());
    for (const auto& line : rendered.lines) {
        const auto visible = cch::tui::visible_width(strip_ansi(line));
        CHECK(visible <= width);
    }
}

TEST_CASE(
    "ScopedModelsSelector never emits a line wider than the render width",
    "[coding_agent][tui][scoped-models][issue426]") {
    auto theme = test_theme();
    Recorder recorder;
    auto selector = make_selector(theme, recorder);

    // Narrow widths any raw (untruncated) model line would exceed; the
    // reported defect reproduced at width 10 with 41-char lines.
    for (const std::size_t width : {8ul, 10ul, 16ul}) {
        const auto rendered = selector.render(width);
        REQUIRE(rendered);
        check_all_lines_bounded(*rendered, width);
    }
}
