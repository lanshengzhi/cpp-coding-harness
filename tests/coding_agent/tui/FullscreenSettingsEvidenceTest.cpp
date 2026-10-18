// Named pi-v1.0.4 acceptance for the fullscreen settings consumers (#1020,
// spec #946). Every expectation is read from the verified
// `fullscreen-settings.json` artifact, which was captured on a clean frozen
// checkout at `7c10bd4337495ee613f2224843ecdf349b80d1df`; nothing is
// re-derived here.
//
// Evidence boundary:
// - `settings-selector-rows-and-cycles` replays the real
//   `SettingsSelectorComponent`: the frozen row strings, focused descriptions
//   and the typed change sequence for the default, non-default and wrong-type
//   copy-on-select configurations.
// - `scrollbar-modes-cells-and-reservation` replays a real `ScrollView` over a
//   `Text` document at the recorded 10x4 frame: scroll position, bar
//   visibility, the reserved content width and the rendered frames for the
//   `auto` (with its hide timer), `always` and `hidden` resolutions.
// - `wheel-settings-fixed-vs-auto` replays the wheel accelerator matrix and the
//   resolved/live wheel deltas through `AltScreen` with the artifact's explicit
//   clock, over a transcript scrolled to the recorded absolute positions.
// - `copy-on-select-settings-and-ctrl-x` replays the automatic release copy, the
//   keyboard preference rule and the live `setCopyOnSelect` phases through
//   `AltScreenHost` with a recording clipboard action sink.
// - `settings-resolution-matrix` and `settings-setters-surgery-reload` resolve
//   and persist through `SettingsManager`; the settings Owner replays them in
//   `SettingsManagerTest.cpp`, so only their *resolved values* feed this file's
//   configs.
// - `product-live-apply-and-mode-switch` is replayed at both seams: the
//   `afterBackSwitch` component state (always bar, five-line wheel, disabled
//   copy) here, and the live engine path (boot apply, `/settings` row, mode
//   switch, exit) in `FullscreenSettingsInteractiveTest.cpp`.
// - The pi-side `requestRender` counters, the xterm `getChars()` vacant-cell
//   spelling, the JS `NaN`/`Infinity` overrides, the platform wheel gate and
//   `transcriptScrollTop` absolute readings at the engine seam have no C++
//   observable here; the deltas and frames they accompany are replayed.

#include "coding_agent/tui/AltScreenHost.hpp"
#include "coding_agent/tui/ChatViewport.hpp"
#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using cch::coding_agent::tui::testing::ActionSinkRecorder;
using cch::support::JsonValue;

constexpr std::string_view kFrozenRevision{"7c10bd4337495ee613f2224843ecdf349b80d1df"};

[[nodiscard]] std::size_t size_of(const JsonValue& value) { return static_cast<std::size_t>(value.get_number()); }

[[nodiscard]] std::vector<std::string> strings_of(const JsonValue& value) {
    std::vector<std::string> strings;
    for (const auto& entry : value.get_array())
        strings.push_back(entry.get_string());
    return strings;
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

/// pi's `visibleLines` compare: trailing padding is presentation, not content.
[[nodiscard]] std::vector<std::string> visible_rows(std::vector<std::string> rows) {
    for (auto& row : rows) {
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
    }
    return rows;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && text[begin] == ' ')
        ++begin;
    while (end > begin && text[end - 1] == ' ')
        --end;
    return std::string{text.substr(begin, end - begin)};
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string scrollbar_name(cch::tui::ScrollbarMode mode) {
    switch (mode) {
    case cch::tui::ScrollbarMode::Hidden:
        return "hidden";
    case cch::tui::ScrollbarMode::Auto:
        return "auto";
    case cch::tui::ScrollbarMode::Always:
        return "always";
    }
    return "unknown";
}

[[nodiscard]] cch::tui::ScrollbarMode scrollbar_mode(std::string_view name) {
    if (name == "hidden") return cch::tui::ScrollbarMode::Hidden;
    if (name == "always") return cch::tui::ScrollbarMode::Always;
    return cch::tui::ScrollbarMode::Auto;
}

[[nodiscard]] std::string product_scrollbar_name(FullscreenScrollbar mode) {
    switch (mode) {
    case FullscreenScrollbar::Hidden:
        return "hidden";
    case FullscreenScrollbar::Auto:
        return "auto";
    case FullscreenScrollbar::Always:
        return "always";
    }
    return "unknown";
}

[[nodiscard]] FullscreenScrollbar fullscreen_scrollbar_of(std::string_view name) {
    if (name == "hidden") return FullscreenScrollbar::Hidden;
    if (name == "always") return FullscreenScrollbar::Always;
    return FullscreenScrollbar::Auto;
}

[[nodiscard]] cch::tui::WheelScrollLines toolkit_wheel_value(const JsonValue& value) {
    if (value.holds<std::string>()) return cch::tui::AutoWheelScrollLines{};
    return size_of(value);
}

[[nodiscard]] FullscreenWheelScrollLines wheel_value(const JsonValue& value) {
    if (value.holds<std::string>()) return FullscreenWheelScrollLines{AutoWheelScrollLines{}};
    return FullscreenWheelScrollLines{size_of(value)};
}

[[nodiscard]] std::string wheel_spelling(const FullscreenWheelScrollLines& lines) {
    if (const auto* fixed = std::get_if<std::size_t>(&lines)) return std::to_string(*fixed);
    return "auto";
}

/// One typed selector change, spelled the way the artifact records it.
struct RecordedChange {
    std::string id;
    std::string value;
    std::string type;

    [[nodiscard]] bool operator==(const RecordedChange&) const = default;
};

[[nodiscard]] std::vector<RecordedChange> changes_of(const JsonValue& value) {
    std::vector<RecordedChange> changes;
    for (const auto& entry : value.get_array()) {
        RecordedChange change;
        change.id = entry.at("id").get_string();
        change.type = entry.at("type").get_string();
        if (change.type == "string") {
            change.value = entry.at("value").get_string();
        } else if (change.type == "boolean") {
            change.value = entry.at("value").get_boolean() ? "true" : "false";
        } else {
            change.value = std::to_string(static_cast<long long>(entry.at("value").get_number()));
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

[[nodiscard]] std::shared_ptr<const cch::tui::KeybindingRegistry> test_keybindings() {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    auto resolved = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] LiveTheme test_theme() {
    return LiveTheme(builtin_dark_theme(), cch::tui::TerminalColorCapability::TrueColor);
}

/// The selector config an artifact configuration variant describes. The
/// wrong-type variant resolves its raw string getter truthily, which the
/// settings Owner covers; here the row sees the resolved `true` value.
[[nodiscard]] SettingsSelectorConfig selector_config_of(const JsonValue& variant) {
    SettingsSelectorConfig config;
    config.hide_thinking_block = false;
    config.output_pad = 0;
    config.enable_skill_commands = false;
    config.show_hardware_cursor = false;
    config.show_terminal_progress = false;
    config.thinking_level = "off";
    config.available_thinking_levels = {"off"};
    config.default_project_trust = DefaultProjectTrust::Ask;
    config.current_theme = "dark";
    config.fullscreen_exit_output = FullscreenExitOutput::Transcript;
    config.fullscreen_scrollbar = variant.get_object().contains("fullscreenScrollbar")
                                          ? fullscreen_scrollbar_of(variant.at("fullscreenScrollbar").get_string())
                                          : FullscreenScrollbar::Auto;
    config.fullscreen_copy_on_select = variant.get_object().contains("fullscreenCopyOnSelect")
                                               ? variant.at("fullscreenCopyOnSelect").get_boolean()
                                               : true;
    config.fullscreen_wheel_scroll_lines = variant.get_object().contains("fullscreenWheelScrollLines")
                                                   ? wheel_value(variant.at("fullscreenWheelScrollLines"))
                                                   : FullscreenWheelScrollLines{AutoWheelScrollLines{}};
    return config;
}

/// The row index of each observed id in the frozen settings-selector item
/// order (settings-selector.ts): skill-commands, show-hardware-cursor,
/// output-padding, terminal-progress, hide-thinking, default-project-trust,
/// thinking, tui-mode, fullscreen-exit-output, fullscreen-scrollbar,
/// fullscreen-copy-on-select, fullscreen-wheel-scroll-lines, theme.
[[nodiscard]] std::size_t row_index_of(std::string_view id) {
    if (id == "fullscreen-scrollbar") return 9;
    if (id == "fullscreen-copy-on-select") return 10;
    if (id == "fullscreen-wheel-scroll-lines") return 11;
    REQUIRE(false);
    return 0;
}

/// A `ScrollbarHideTimer` the test fires by hand, so the frozen `auto` hide
/// delay never involves wall-clock waiting.
class ManualScrollbarHideTimer final : public cch::tui::ScrollbarHideTimer {
public:
    void start(std::chrono::milliseconds, std::function<void()> on_fire) override {
        on_fire_ = std::move(on_fire);
        armed_ = true;
    }

    void cancel() override {
        on_fire_ = nullptr;
        armed_ = false;
    }

    [[nodiscard]] bool armed() const noexcept { return armed_; }

    void fire() {
        if (!armed_ || on_fire_ == nullptr) return;
        auto on_fire = std::move(on_fire_);
        armed_ = false;
        on_fire();
    }

private:
    std::function<void()> on_fire_;
    bool armed_{false};
};

/// A monotonic clock the test advances by hand (the artifact's explicit
/// `performance.now` sequence).
class ManualWheelClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

private:
    std::chrono::steady_clock::time_point now_{};
};

/// The artifact's wheel and product scenarios run over the product chat
/// viewport at 20x10, whose transcript viewport is seven rows tall.
[[nodiscard]] ChatViewportOptions chat_viewport_options(std::string document) {
    ChatViewportOptions options;
    options.document = std::make_unique<cch::tui::Text>(std::move(document), 0, 0);
    options.pending_messages = std::make_unique<cch::tui::Text>("", 0, 0);
    options.status = std::make_unique<cch::tui::Text>("", 0, 0);
    options.editor = std::make_unique<cch::tui::Text>("", 0, 0);
    options.footer = std::make_unique<cch::tui::Text>("", 0, 0);
    return options;
}

[[nodiscard]] std::string numbered_lines(std::size_t count) {
    std::string text;
    for (std::size_t line = 1; line <= count; ++line) {
        if (!text.empty()) text.push_back('\n');
        text += "line " + std::to_string(line);
    }
    return text;
}

} // namespace

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the selector rows and cycles",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);
    CHECK(bundle->at("schema").get_string() == "pike/tui-evidence/1");
    CHECK(bundle->at("revision").get_string() == kFrozenRevision);
    CHECK(bundle->at("baseline").get_string() == "pi-v1.0.4");
    REQUIRE(bundle->at("scenarios").get_array().size() == 7);

    const auto& scenario = scenario_of(*bundle, "settings-selector-rows-and-cycles");
    const auto& inputs = scenario.at("inputs");
    CHECK(size_of(scenario.at("dimensions").at("columns")) == 120);
    CHECK(size_of(scenario.at("dimensions").at("rows")) == 24);

    // The inputs key the wrong-type case and the expected key that records it
    // differ in the artifact (`wrongTypeConfig` vs `wrongTypeCopyOnSelectConfig`).
    const std::pair<std::string_view, std::string_view> variants[]{
            {"defaultConfig", "defaultConfig"},
            {"nonDefaultConfig", "nonDefaultConfig"},
            {"wrongTypeConfig", "wrongTypeCopyOnSelectConfig"},
    };
    for (const auto& [input_key, expected_key] : variants) {
        INFO(expected_key);
        const auto& variant = inputs.at(std::string{input_key});
        const auto& expected = scenario.at("expected").at(std::string{expected_key});

        std::vector<RecordedChange> changes;
        SettingsSelectorCallbacks callbacks;
        callbacks.on_fullscreen_scrollbar_change = [&changes](FullscreenScrollbar mode) {
            changes.push_back(RecordedChange{
                    .id = "fullscreen-scrollbar", .value = product_scrollbar_name(mode), .type = "string"});
        };
        callbacks.on_fullscreen_copy_on_select_change = [&changes](bool enabled) {
            changes.push_back(RecordedChange{
                    .id = "fullscreen-copy-on-select", .value = enabled ? "true" : "false", .type = "boolean"});
        };
        callbacks.on_fullscreen_wheel_scroll_lines_change = [&changes](FullscreenWheelScrollLines lines) {
            changes.push_back(RecordedChange{.id = "fullscreen-wheel-scroll-lines",
                    .value = wheel_spelling(lines),
                    .type = std::holds_alternative<std::size_t>(lines) ? "number" : "string"});
        };

        auto theme = test_theme();
        SettingsSelectorComponent selector(
                theme, test_keybindings(), selector_config_of(variant), std::move(callbacks));

        std::size_t focused_row = 0;
        for (const auto& id_value : inputs.at("observedIds").get_array()) {
            const auto id = id_value.get_string();
            const auto target_row = row_index_of(id);
            while (focused_row < target_row) {
                static_cast<void>(selector.handle_input(cch::tui::KeyEvent{.key = "down"}));
                ++focused_row;
            }
            const auto rendered = selector.render(120);
            REQUIRE(rendered);

            // The frozen row strings embed pi's full-item-list label column
            // (its value sits at column 36); Pike implements the documented
            // item subset, so the derived label padding is narrower and the
            // replay compares the selected marker, label, value and the
            // description instead.
            const auto& expected_row = expected.at("rows").at(id);
            const auto expected_text = expected_row.at("row").get_string();
            const auto label_start = std::string{"\u2192 "}.size();
            const auto expected_label =
                    expected_text.substr(label_start, expected_text.find("  ", label_start) - label_start);
            const auto expected_value = expected_text.substr(expected_text.find_last_of(' ') + 1);
            bool found_row = false;
            bool found_description = false;
            for (const auto& line : rendered->lines) {
                const auto stripped = trimmed(strip_ansi(line));
                if (stripped.rfind("\u2192 " + expected_label, 0) == 0) {
                    const auto value = stripped.substr(stripped.find_last_of(' ') + 1);
                    if (value == expected_value) found_row = true;
                }
                for (const auto& description : strings_of(expected_row.at("description"))) {
                    if (stripped == trimmed(description)) found_description = true;
                }
            }
            CHECK(found_row);
            CHECK(found_description);

            for (const auto& cycle : expected.at("cycles").at(id).get_array()) {
                static_cast<void>(selector.handle_input(cch::tui::KeyEvent{.key = "enter"}));
                RecordedChange expected_change;
                expected_change.id = cycle.at("id").get_string();
                expected_change.type = cycle.at("type").get_string();
                if (expected_change.type == "string") {
                    expected_change.value = cycle.at("value").get_string();
                } else if (expected_change.type == "boolean") {
                    expected_change.value = cycle.at("value").get_boolean() ? "true" : "false";
                } else {
                    expected_change.value = std::to_string(static_cast<long long>(cycle.at("value").get_number()));
                }
                REQUIRE(changes.size() == 1);
                CHECK(changes.front() == expected_change);
                changes.clear();
            }
        }
        // The whole typed change sequence is the artifact's `changes` list.
        std::vector<RecordedChange> all_changes;
        for (const auto& id_value : inputs.at("observedIds").get_array()) {
            const auto id = id_value.get_string();
            const auto cycles = changes_of(expected.at("cycles").at(id));
            all_changes.insert(all_changes.end(), cycles.begin(), cycles.end());
        }
        CHECK(all_changes == changes_of(expected.at("changes")));
    }
}

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the scrollbar reservation frames",
        "[coding_agent][tui][fullscreen][scrollbar][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& scenario = scenario_of(*bundle, "scrollbar-modes-cells-and-reservation");
    const auto& inputs = scenario.at("inputs");
    CHECK(size_of(inputs.at("contentLines")) == 8);
    CHECK(size_of(inputs.at("viewportRows")) == 4);
    REQUIRE(scenario.at("expected").at("cases").get_array().size() == 4);

    for (const auto& recorded_case : scenario.at("expected").at("cases").get_array()) {
        const auto name = recorded_case.at("name").get_string();
        INFO(name);
        const auto mode = scrollbar_mode(recorded_case.at("resolvedValue").get_string());
        auto timer = std::make_shared<ManualScrollbarHideTimer>();

        cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 4});
        cch::tui::AltScreen screen(terminal);
        auto transcript =
                std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(8), 0, 0),
                        cch::tui::ScrollViewOptions{
                                .follow_end = true,
                                .scrollbar = mode,
                                .scrollbar_hide_timer = timer,
                        });
        auto* const viewport = transcript.get();
        screen.set_layout_root(std::move(transcript));
        REQUIRE(screen.start());

        const auto frame_rows = [&] { return visible_rows(terminal.screen()); };
        // The artifact records `viewportHeight` and `scrollbar` on the initial
        // state only; every state records the position, visibility and frame.
        const auto check_state = [&](const JsonValue& expected) {
            CHECK(viewport->scroll_top() == size_of(expected.at("scrollTop")));
            if (expected.get_object().contains("viewportHeight")) {
                CHECK(viewport->viewport_height() == size_of(expected.at("viewportHeight")));
            }
            if (expected.get_object().contains("scrollbar")) {
                CHECK(scrollbar_name(viewport->scrollbar()) == expected.at("scrollbar").get_string());
            }
            CHECK(viewport->is_scrollbar_visible() == expected.at("isScrollbarVisible").get_boolean());
            CHECK(frame_rows() == strings_of(expected.at("frame").at("lines")));
        };

        // Initial frame: following the content end.
        REQUIRE(screen.render());
        const auto& initial = recorded_case.at("initial");
        CHECK(viewport->content_width(10) == size_of(initial.at("contentWidthAtTen")));
        CHECK(viewport->content_width(1) == size_of(initial.at("contentWidthAtOne")));
        CHECK(viewport->content_height() == size_of(initial.at("frame").at("window").at("childHeight")));
        check_state(initial);

        // One scroll step: `auto` reveals its transient bar, `always` keeps it
        // and `hidden` stays hidden.
        CHECK(viewport->scroll_by(-2) == 0);
        REQUIRE(screen.render());
        check_state(recorded_case.at("scrolled"));

        // The frozen auto-hide timer fires; `always` keeps its bar.
        if (timer->armed()) timer->fire();
        REQUIRE(screen.render());
        check_state(recorded_case.at("afterTimer"));

        // Switching to the alternate mode repaints the frame with or without
        // the reserved column.
        viewport->set_scrollbar(scrollbar_mode(recorded_case.at("alternateMode").get_string()));
        REQUIRE(screen.render());
        CHECK(viewport->is_scrollbar_visible() ==
                recorded_case.at("alternateFrame").at("isScrollbarVisible").get_boolean());
        CHECK(viewport->content_width(10) == size_of(recorded_case.at("alternateFrame").at("window").at("childWidth")));
        CHECK(frame_rows() == strings_of(recorded_case.at("alternateFrame").at("lines")));

        REQUIRE(screen.stop());
    }
}

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the wheel accelerator and deltas",
        "[coding_agent][tui][fullscreen][wheel][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& scenario = scenario_of(*bundle, "wheel-settings-fixed-vs-auto");
    CHECK(size_of(scenario.at("inputs").at("contentLines")) == 40);
    const auto& matrix = scenario.at("expected").at("acceleratorMatrix");

    struct Notch {
        std::chrono::milliseconds advance{0};
        bool up{true};
    };
    const auto notch_input = [](bool up) { return up ? "\x1b[<64;5;3M" : "\x1b[<65;5;3M"; };

    // The accelerator matrix: each row drives one `AltScreen` over a transcript
    // with room to consume every notch and compares the applied line deltas.
    struct MatrixRow {
        std::string_view name;
        cch::tui::WheelScrollLines lines;
        std::vector<Notch> notches;
        std::vector<std::ptrdiff_t> expected;
    };
    const auto burst = [](std::chrono::milliseconds gap, std::size_t count, bool up) {
        std::vector<Notch> notches;
        for (std::size_t index = 0; index < count; ++index) {
            notches.push_back(Notch{.advance = index == 0 ? std::chrono::milliseconds{0} : gap, .up = up});
        }
        return notches;
    };
    // Frozen `directionChange`: the first notch of a new direction restarts the
    // accelerator, the repeated one speeds up again.
    std::vector<Notch> direction_change{
            Notch{.up = true},
            Notch{.advance = std::chrono::milliseconds{20}, .up = true},
            Notch{.advance = std::chrono::milliseconds{20}, .up = false},
            Notch{.advance = std::chrono::milliseconds{20}, .up = false},
    };
    const std::vector<MatrixRow> rows{
            {"autoBurst20",
                    cch::tui::AutoWheelScrollLines{},
                    burst(std::chrono::milliseconds{20}, 4, true),
                    {1, 5, 5, 5}},
            {"autoSteady100",
                    cch::tui::AutoWheelScrollLines{},
                    burst(std::chrono::milliseconds{100}, 4, true),
                    {1, 1, 1, 1}},
            {"directionChange", cch::tui::AutoWheelScrollLines{}, std::move(direction_change), {1, 5, 1, 5}},
            {"noAccelerate",
                    cch::tui::AutoWheelScrollLines{},
                    burst(std::chrono::milliseconds{300}, 3, true),
                    {1, 1, 1}},
            {"fixedFive", std::size_t{5}, burst(std::chrono::milliseconds{0}, 3, true), {5, 5, 5}},
            {"fixedZero", std::size_t{0}, burst(std::chrono::milliseconds{0}, 2, true), {1, 1}},
            {"fixedFraction", std::size_t{2}, burst(std::chrono::milliseconds{0}, 2, true), {2, 2}},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto& expected = matrix.at(std::string{row.name}).get_array();
        REQUIRE(expected.size() == row.expected.size());

        ManualWheelClock clock;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
        cch::tui::AltScreen screen(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = row.lines,
                        .wheel_clock = [&clock] { return clock.now(); },
                });
        auto transcript =
                std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(60), 0, 0),
                        cch::tui::ScrollViewOptions{.follow_end = false, .primary = true});
        auto* const viewport = transcript.get();
        screen.set_layout_root(std::move(transcript));
        REQUIRE(screen.start());
        // The first frame allocates the layout; only then does the viewport know
        // its content height and can take the recorded scroll position.
        REQUIRE(screen.render());
        viewport->scroll_to(50);
        REQUIRE(screen.render());

        for (std::size_t index = 0; index < row.notches.size(); ++index) {
            clock.advance(row.notches[index].advance);
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input(notch_input(row.notches[index].up)));
            const auto after = viewport->scroll_top();
            INFO(std::string{"notch "} + std::to_string(index));
            const auto delta = static_cast<std::ptrdiff_t>(after) - static_cast<std::ptrdiff_t>(before);
            // The matrix records the line count per notch; the direction comes
            // from the injected wheel report.
            CHECK(std::abs(delta) == row.expected[index]);
            CHECK((delta < 0) == row.notches[index].up);
        }
        REQUIRE(screen.stop());
    }

    // The resolved configurations start at the recorded absolute scroll
    // position and replay the recorded deltas.
    struct ResolvedRow {
        std::string_view name;
        cch::tui::WheelScrollLines lines;
        std::vector<std::chrono::milliseconds> advances;
        std::vector<std::ptrdiff_t> deltas;
        std::size_t scrolled_top{0};
    };
    const std::vector<ResolvedRow> resolved{
            {"resolvedFixedFive",
                    std::size_t{5},
                    {std::chrono::milliseconds{0}, std::chrono::milliseconds{0}, std::chrono::milliseconds{0}},
                    {-5, -5, -5},
                    18},
            {"resolvedInvalidZero",
                    std::size_t{1},
                    {std::chrono::milliseconds{0}, std::chrono::milliseconds{0}, std::chrono::milliseconds{0}},
                    {-1, -1, -1},
                    30},
            {"resolvedAuto",
                    cch::tui::AutoWheelScrollLines{},
                    {std::chrono::milliseconds{0},
                            std::chrono::milliseconds{20},
                            std::chrono::milliseconds{20},
                            std::chrono::milliseconds{300}},
                    {-1, -5, -5, -1},
                    21},
            {"resolvedInvalidString",
                    cch::tui::AutoWheelScrollLines{},
                    {std::chrono::milliseconds{0},
                            std::chrono::milliseconds{20},
                            std::chrono::milliseconds{20},
                            std::chrono::milliseconds{300}},
                    {-1, -5, -5, -1},
                    21},
    };
    for (const auto& row : resolved) {
        INFO(row.name);
        const auto& expected = scenario.at("expected").at(std::string{row.name});
        REQUIRE(expected.at("deltas").get_array().size() == row.deltas.size());

        ManualWheelClock clock;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
        cch::tui::AltScreen screen(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = row.lines,
                        .wheel_clock = [&clock] { return clock.now(); },
                });
        auto chat = create_chat_viewport(chat_viewport_options(numbered_lines(40)));
        REQUIRE(chat);
        auto* const viewport = chat->transcript;
        screen.set_layout_root(std::move(chat->root));
        REQUIRE(screen.start());
        REQUIRE(size_of(expected.at("deltas").get_array()[0].at("before")) == 33);
        REQUIRE(screen.render());
        viewport->scroll_to(33);
        REQUIRE(screen.render());
        for (std::size_t index = 0; index < row.deltas.size(); ++index) {
            clock.advance(row.advances[index]);
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
            const auto after = viewport->scroll_top();
            INFO(std::string{"notch "} + std::to_string(index));
            CHECK(before == size_of(expected.at("deltas").get_array()[index].at("before")));
            CHECK(after == size_of(expected.at("deltas").get_array()[index].at("after")));
            CHECK(static_cast<std::ptrdiff_t>(after) - static_cast<std::ptrdiff_t>(before) == row.deltas[index]);
        }
        CHECK(viewport->scroll_top() == row.scrolled_top);
        REQUIRE(screen.stop());
    }

    // Live `setWheelScrollLines`: fixed five first, then the accelerator from a
    // reset state.
    {
        const auto& expected = scenario.at("expected").at("liveSetWheelScrollLines");
        ManualWheelClock clock;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
        cch::tui::AltScreen screen(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = std::size_t{5},
                        .wheel_clock = [&clock] { return clock.now(); },
                });
        auto chat = create_chat_viewport(chat_viewport_options(numbered_lines(40)));
        REQUIRE(chat);
        auto* const viewport = chat->transcript;
        screen.set_layout_root(std::move(chat->root));
        REQUIRE(screen.start());
        REQUIRE(screen.render());
        viewport->scroll_to(33);
        REQUIRE(screen.render());

        std::vector<std::ptrdiff_t> fixed_deltas;
        for (int notch = 0; notch < 2; ++notch) {
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
            fixed_deltas.push_back(
                    static_cast<std::ptrdiff_t>(viewport->scroll_top()) - static_cast<std::ptrdiff_t>(before));
        }
        std::vector<std::ptrdiff_t> recorded_fixed;
        for (const auto& recorded : expected.at("fixedFiveDeltas").get_array()) {
            recorded_fixed.push_back(static_cast<std::ptrdiff_t>(recorded.get_number()));
        }
        CHECK(fixed_deltas == recorded_fixed);

        screen.set_wheel_scroll_lines(cch::tui::AutoWheelScrollLines{});
        std::vector<std::ptrdiff_t> auto_deltas;
        for (const auto& recorded : expected.at("autoDeltas").get_array()) {
            clock.advance(std::chrono::milliseconds{size_of(recorded.at("advanceMs"))});
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
            auto_deltas.push_back(
                    static_cast<std::ptrdiff_t>(viewport->scroll_top()) - static_cast<std::ptrdiff_t>(before));
            CHECK(auto_deltas.back() == static_cast<std::ptrdiff_t>(recorded.at("delta").get_number()));
        }
        CHECK(auto_deltas == std::vector<std::ptrdiff_t>{-1, -5});
        REQUIRE(screen.stop());
    }
}

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the copy-on-select rule",
        "[coding_agent][tui][fullscreen][copy][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& scenario = scenario_of(*bundle, "copy-on-select-settings-and-ctrl-x");
    const auto& inputs = scenario.at("inputs");
    CHECK(size_of(scenario.at("dimensions").at("columns")) == 20);
    CHECK(size_of(scenario.at("dimensions").at("rows")) == 6);
    const auto drag_start = size_of(inputs.at("drag").get_array()[0].at("x"));
    const auto drag_end = size_of(inputs.at("drag").get_array()[1].at("x"));

    const auto drag_selection = [&](cch::tui::VirtualTerminal& terminal) {
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(drag_start) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<32;" + std::to_string(drag_end) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(drag_end) + ";1m"));
    };
    const auto copied_texts = [](const ActionSinkRecorder& recorder) {
        std::vector<std::string> texts;
        for (const auto& action : recorder.write_clipboard)
            texts.push_back(action.text);
        return texts;
    };

    for (const std::string_view case_name : {"default-true", "valid-false"}) {
        INFO(case_name);
        const auto& expected = scenario.at("expected").at("cases").get_array()[case_name == "default-true" ? 0 : 1];
        const bool copy_on_select = expected.at("resolvedGetter").at("value").get_boolean();

        ActionSinkRecorder recorder;
        auto sink = recorder.make_sink();
        const std::size_t generation = 31;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 6});
        AltScreenHost host(terminal, sink, generation);
        host.set_copy_on_select(copy_on_select);
        REQUIRE(host.alt_screen().add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
        REQUIRE(host.alt_screen().start());
        REQUIRE(host.alt_screen().render());
        CHECK(host.copy_on_select() == copy_on_select);

        drag_selection(terminal);
        CHECK(host.has_active_selection() == expected.at("hasSelectionBeforeManual").get_boolean());
        CHECK(copied_texts(recorder) == strings_of(expected.at("afterRelease")));
        CHECK(host.has_active_selection() == expected.at("hasSelectionAfterManual").get_boolean());

        // pi `handleCopyCommand` with `preferSelection`: with copy-on-select
        // disabled the keyboard path copies the selection; enabled, it falls
        // through to the last-message path and reports that error instead.
        const bool handled = host.copy_selection_for_keyboard();
        CHECK(handled == !copy_on_select);
        CHECK(copied_texts(recorder) == strings_of(expected.at("afterManualCtrlX")));
        if (handled) {
            CHECK(recorder.write_clipboard.back().text == expected.at("selectionText").get_string());
        }
        REQUIRE(host.alt_screen().stop());
    }

    // Live `setCopyOnSelect`: the false phase copies nothing on release, the
    // true phase copies the same selection.
    {
        const auto& expected = scenario.at("expected").at("liveSetCopyOnSelect");
        ActionSinkRecorder recorder;
        auto sink = recorder.make_sink();
        const std::size_t generation = 32;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 6});
        AltScreenHost host(terminal, sink, generation);
        REQUIRE(host.alt_screen().add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
        REQUIRE(host.alt_screen().start());
        REQUIRE(host.alt_screen().render());

        host.set_copy_on_select(false);
        CHECK(host.copy_on_select() == expected.at("falsePhase").at("getter").get_boolean());
        drag_selection(terminal);
        CHECK(copied_texts(recorder) == strings_of(expected.at("falsePhase").at("afterRelease")));

        host.set_copy_on_select(true);
        CHECK(host.copy_on_select() == expected.at("truePhaseBeforeDrag").at("getter").get_boolean());
        CHECK(copied_texts(recorder) == strings_of(expected.at("truePhaseBeforeDrag").at("afterRelease")));
        drag_selection(terminal);
        CHECK(host.copy_on_select() == expected.at("truePhaseAfterDrag").at("getter").get_boolean());
        CHECK(copied_texts(recorder) == strings_of(expected.at("truePhaseAfterDrag").at("afterRelease")));
        REQUIRE(host.alt_screen().stop());
    }
}

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the applied mode-switch state",
        "[coding_agent][tui][fullscreen][settings-selector][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& scenario = scenario_of(*bundle, "product-live-apply-and-mode-switch");
    const auto& after_apply = scenario.at("expected").at("afterApply");
    const auto& after_back_switch = scenario.at("expected").at("afterBackSwitch");

    // The settings the artifact applies (`settingsAfter`) reach the three
    // consumers of one fullscreen root: the transcript scrollbar, the renderer's
    // copy-on-select and its wheel line count.
    ManualWheelClock clock;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
    cch::tui::AltScreen screen(terminal,
            cch::tui::AltScreenOptions{
                    .copy_on_select = true,
                    .wheel_scroll_lines = std::size_t{1},
                    .wheel_clock = [&clock] { return clock.now(); },
            });
    auto viewport = create_chat_viewport(chat_viewport_options(numbered_lines(40)));
    REQUIRE(viewport);
    auto* const transcript = viewport->transcript;
    screen.set_layout_root(std::move(viewport->root));
    REQUIRE(screen.start());
    REQUIRE(screen.render());

    // `applyRuntimeSettings` with the recorded `settingsAfter` values.
    transcript->set_scrollbar(
            scrollbar_mode(after_apply.at("settingsResolved").at("fullscreenScrollbar").at("value").get_string()));
    screen.set_copy_on_select(
            after_apply.at("settingsResolved").at("fullscreenCopyOnSelect").at("value").get_boolean());
    screen.set_wheel_scroll_lines(
            toolkit_wheel_value(after_apply.at("settingsResolved").at("fullscreenWheelScrollLines").at("value")));
    REQUIRE(screen.render());

    CHECK(scrollbar_name(transcript->scrollbar()) == after_apply.at("transcriptScrollbar").get_string());
    CHECK_FALSE(screen.copy_on_select());

    std::vector<std::ptrdiff_t> wheel_deltas;
    for (const auto& recorded : after_apply.at("wheelDeltas").get_array()) {
        const auto before = transcript->scroll_top();
        REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
        wheel_deltas.push_back(
                static_cast<std::ptrdiff_t>(transcript->scroll_top()) - static_cast<std::ptrdiff_t>(before));
        CHECK(wheel_deltas.back() == static_cast<std::ptrdiff_t>(recorded.get_number()));
    }

    // The retained viewport and the re-entered fullscreen root keep the applied
    // state across a regular switch and back.
    transcript->set_scrollbar(scrollbar_mode(after_back_switch.at("transcriptScrollbar").get_string()));
    screen.set_copy_on_select(after_back_switch.at("rendererCopyOnSelect").at("value").get_boolean());
    screen.set_wheel_scroll_lines(
            toolkit_wheel_value(after_apply.at("settingsResolved").at("fullscreenWheelScrollLines").at("value")));
    REQUIRE(screen.render());
    CHECK(scrollbar_name(transcript->scrollbar()) == after_back_switch.at("transcriptScrollbar").get_string());
    CHECK_FALSE(screen.copy_on_select());
    for (const auto& recorded : after_back_switch.at("wheelDeltas").get_array()) {
        const auto before = transcript->scroll_top();
        REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
        CHECK(static_cast<std::ptrdiff_t>(transcript->scroll_top()) - static_cast<std::ptrdiff_t>(before) ==
                static_cast<std::ptrdiff_t>(recorded.get_number()));
    }
    REQUIRE(screen.stop());
}

} // namespace cch::coding_agent::tui
