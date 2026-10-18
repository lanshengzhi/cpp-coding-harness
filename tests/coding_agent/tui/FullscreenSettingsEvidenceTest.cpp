// Named pi-v1.0.4 acceptance for the fullscreen settings consumers (#1020,
// spec #946). Every expectation is read from the verified
// `fullscreen-settings.json` artifact, which was captured on a clean frozen
// checkout at `7c10bd4337495ee613f2224843ecdf349b80d1df`; nothing is
// re-derived here.
//
// Evidence boundary:
// - `settings-resolution-matrix` and `settings-setters-surgery-reload` are
//   resolved and persisted through the real `SettingsManager` from the
//   artifact's own raw inputs (`global`/`project` objects and the seed files);
//   the expected resolutions are the artifact's. The four `apply-overrides-*`
//   cases record pi's in-memory `applyOverrides` payloads (`NaN`, `Infinity`,
//   `undefined`, a hidden/zero merge) and have no Pike seam.
// - `settings-selector-rows-and-cycles` replays the real
//   `SettingsSelectorComponent` over a `SettingsManager` seeded with each
//   variant's raw settings: the frozen row strings, focused descriptions and
//   the whole observed typed change sequence for the default, non-default and
//   wrong-type copy-on-select configurations.
// - `scrollbar-modes-cells-and-reservation` resolves each recorded provenance
//   (`default`/`persisted always`/`persisted hidden`/`persisted bogus`) through
//   `SettingsManager`, then replays a real `ScrollView` over a `Text` document
//   at the recorded 10x4 frame: scroll position, bar visibility/activity, the
//   reserved content width, the styled scrollbar cells of the recorded
//   `rawLines` and every frame window field for the `auto` (with its hide
//   timer), `always` and `hidden` resolutions.
// - `wheel-settings-fixed-vs-auto` replays the wheel accelerator matrix and the
//   resolved/live wheel deltas through `AltScreen` with the artifact's explicit
//   clock, over a transcript scrolled to the recorded absolute positions; the
//   matrix and resolved deltas/scroll-tops/advances are read from the artifact,
//   and each resolved row's raw wheel value is resolved through
//   `SettingsManager` first.
// - `copy-on-select-settings-and-ctrl-x` resolves each case through
//   `SettingsManager` (raw inputs from the resolution matrix case it names) and
//   then replays the automatic release copy, the keyboard preference rule and
//   the live `setCopyOnSelect` phases through `AltScreenHost` with a recording
//   clipboard action sink; the typed getter is the documented consumer-truthy
//   projection of the artifact's raw JS getter.
// - `product-live-apply-and-mode-switch` is replayed at two seams. Here a real
//   `SettingsManager` seeded with `inputs.settingsBefore` initializes the
//   product chat viewport (the `createChatViewport({scrollbar})` pattern) over
//   the frozen harness's literal 40 numbered lines; `inputs.settingsAfter`
//   reaches the live consumers through the manager's resolved values, and the
//   wheel deltas/scroll positions are read from the artifact. The actual
//   `InteractiveEngine::switch_tui_mode` continuity, the runtime apply and the
//   mode-switch option fields (`result`, `rendererMode`, `optionsTuiMode`) are
//   exercised in `FullscreenSettingsInteractiveTest.cpp`.
// - The pi-side `requestRender` counters are replayed at the real
//   `ScrollView` request-render seam; the JS `NaN`/`Infinity`/`undefined`
//   override payloads and the raw JS getter types have no C++ spelling and stay
//   on the settings lane. The scrollbar `rawLines` are compared semantically
//   (glyph plus foreground SGR per row); their OSC-8 envelope and reset
//   spelling are the xterm byte representation, and scrollbar/selection styles
//   outside artifact 36 stay under their prior authorities (#1001/#1004/#1006/
//   #1036).

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
#include "support/JsonCompare.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
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

[[nodiscard]] std::vector<std::size_t> sizes_of(const JsonValue& value) {
    std::vector<std::size_t> sizes;
    for (const auto& entry : value.get_array())
        sizes.push_back(static_cast<std::size_t>(entry.get_number()));
    return sizes;
}

[[nodiscard]] std::vector<std::ptrdiff_t> numbers_of(const JsonValue& value) {
    std::vector<std::ptrdiff_t> numbers;
    for (const auto& entry : value.get_array())
        numbers.push_back(static_cast<std::ptrdiff_t>(entry.get_number()));
    return numbers;
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

[[nodiscard]] std::string serialize_json(const JsonValue& value) {
    auto text = cch::support::write_json(value);
    REQUIRE(text);
    return *text;
}

/// One on-disk settings workspace: the global scope is
/// `<agent>/settings.json`, the project scope `<cwd>/.pi/settings.json`.
struct EvidenceSettingsWorkspace {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path agent_dir{workspace.path() / "agent"};

    void write_global(const JsonValue& settings) const { workspace.write("agent/settings.json", serialize_json(settings)); }
    void write_global_text(std::string text) const { workspace.write("agent/settings.json", std::move(text)); }
    void write_project(const JsonValue& settings) const { workspace.write(".pi/settings.json", serialize_json(settings)); }

    [[nodiscard]] coding_agent::SettingsManager manager() const {
        return coding_agent::SettingsManager::create(workspace.path(), agent_dir, /* project_trusted */ true);
    }

    [[nodiscard]] std::string read_global() const { return workspace.read("agent/settings.json"); }
    [[nodiscard]] std::string read_project() const { return workspace.read(".pi/settings.json"); }
};

/// Structural comparison of two JSON documents (key order is not evidence).
[[nodiscard]] std::optional<std::string> json_text_mismatch(
        std::string_view expected_text, std::string_view actual_text) {
    const auto expected = cch::support::read_json(expected_text);
    const auto actual = cch::support::read_json(actual_text);
    REQUIRE(expected);
    REQUIRE(actual);
    return cch::tests::json_mismatch(*expected, *actual);
}

[[nodiscard]] std::string value_spelling(const JsonValue& value) {
    if (value.holds<std::string>()) return value.get_string();
    if (value.holds<bool>()) return value.get_boolean() ? "true" : "false";
    return std::to_string(static_cast<long long>(value.get_number()));
}

/// JavaScript truthiness of an artifact raw value: the wrong-type string and
/// number cases project onto the typed product values through it.
[[nodiscard]] bool js_truthy(const JsonValue& value) {
    if (value.holds<bool>()) return value.get_boolean();
    if (value.holds<std::string>()) return !value.get_string().empty();
    if (value.holds<double>()) return value.get_number() != 0.0;
    return false;
}

/// The artifact's resolved-value triple, compared through the typed product
/// getters. The copy-on-select getter carries the consumer bool (`?? true`
/// plus JavaScript truthiness); the artifact also records the raw JS
/// value/type, which has no C++ spelling, so the documented truthy projection
/// is compared.
void check_resolved_getters(const coding_agent::SettingsManager& manager, const JsonValue& expected) {
    CHECK(product_scrollbar_name(manager.get_fullscreen_scrollbar()) ==
            expected.at("fullscreenScrollbar").at("value").get_string());
    CHECK(manager.get_fullscreen_copy_on_select() ==
            expected.at("fullscreenCopyOnSelect").at("truthy").get_boolean());
    CHECK(wheel_spelling(manager.get_fullscreen_wheel_scroll_lines()) ==
            value_spelling(expected.at("fullscreenWheelScrollLines").at("value")));
}

[[nodiscard]] cch::tui::WheelScrollLines toolkit_wheel(const FullscreenWheelScrollLines& lines) {
    if (const auto* fixed = std::get_if<std::size_t>(&lines)) return *fixed;
    return cch::tui::AutoWheelScrollLines{};
}

/// The resolution-matrix case a downstream scenario names for its raw inputs.
[[nodiscard]] const JsonValue& resolution_case_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& recorded : scenario_of(bundle, "settings-resolution-matrix").at("inputs").at("cases").get_array()) {
        if (recorded.at("name").get_string() == name) return recorded;
    }
    REQUIRE(false);
    return bundle;
}

/// One named case of a scenario's `expected.cases` array.
[[nodiscard]] const JsonValue& named_case_of(const JsonValue& cases, std::string_view name) {
    for (const auto& entry : cases.get_array()) {
        if (entry.at("name").get_string() == name) return entry;
    }
    REQUIRE(false);
    return cases;
}

/// The settings-selector config a `SettingsManager` resolves for one artifact
/// configuration variant: the variant's own fields are the raw global settings
/// (`wrongTypeConfig` stores its raw string under the getter metadata key).
[[nodiscard]] JsonValue raw_settings_of_variant(const JsonValue& variant) {
    JsonValue::object_t settings;
    for (const std::string_view key : {"fullscreenScrollbar", "fullscreenCopyOnSelect", "fullscreenWheelScrollLines"}) {
        if (variant.get_object().contains(std::string{key})) {
            settings.emplace(std::string{key}, variant.at(std::string{key}));
        }
    }
    if (variant.get_object().contains("fullscreenCopyOnSelectGetter")) {
        settings.emplace("fullscreenCopyOnSelect", variant.at("fullscreenCopyOnSelectGetter"));
    }
    return JsonValue{std::move(settings)};
}

[[nodiscard]] SettingsSelectorConfig selector_config_of(const coding_agent::SettingsManager& manager) {
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
    config.fullscreen_exit_output = manager.get_fullscreen_exit_output();
    config.fullscreen_scrollbar = manager.get_fullscreen_scrollbar();
    config.fullscreen_copy_on_select = manager.get_fullscreen_copy_on_select();
    config.fullscreen_wheel_scroll_lines = manager.get_fullscreen_wheel_scroll_lines();
    return config;
}

/// The raw scrollbar value each recorded case's provenance names
/// (`initial.resolvedFrom` metadata), or nothing for the default resolution.
[[nodiscard]] std::optional<std::string> persisted_scrollbar_of(const JsonValue& recorded_case) {
    const auto& from = recorded_case.at("initial").at("resolvedFrom").get_string();
    const auto open = from.rfind('(');
    const auto close = from.rfind(')');
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    const std::string provenance = from.substr(open + 1, close - open - 1);
    constexpr std::string_view kPersisted{"persisted "};
    if (!provenance.starts_with(kPersisted)) return std::nullopt;
    return provenance.substr(kPersisted.size());
}

[[nodiscard]] bool is_scrollbar_glyph(std::string_view grapheme) {
    return grapheme == "\xe2\x94\x82" || grapheme == "\xe2\x94\x83" || grapheme == "\xe2\x96\x88";
}

[[nodiscard]] std::size_t utf8_char_length(unsigned char lead) {
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    return 4;
}

/// The styled scrollbar cell of one artifact `rawLines` entry: the exact SGR
/// foreground immediately before the track/thumb glyph. The OSC-8 envelope,
/// the leading reset and the trailing `39` reset are the xterm byte spelling
/// of the same cell, not Pike cell state; only the glyph and foreground code
/// are compared, and the prior scrollbar style authority (#1001/#1004/#1036)
/// stays outside artifact 36.
struct ScrollbarRawCell {
    std::string glyph;
    std::string fg_color;
};

[[nodiscard]] std::optional<ScrollbarRawCell> styled_scrollbar_cell(std::string_view raw_line) {
    for (const std::string_view code : {"\x1b[90m", "\x1b[37m"}) {
        const auto position = raw_line.find(code);
        if (position == std::string_view::npos) continue;
        const auto glyph_start = position + code.size();
        REQUIRE(glyph_start < raw_line.size());
        const auto glyph_end = glyph_start + utf8_char_length(static_cast<unsigned char>(raw_line[glyph_start]));
        return ScrollbarRawCell{.glyph = std::string{raw_line.substr(glyph_start, glyph_end - glyph_start)},
                .fg_color = std::string{code.substr(2, code.size() - 3)}};
    }
    return std::nullopt;
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

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence resolves the settings matrix through SettingsManager",
        "[coding_agent][tui][fullscreen][settings][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& cases = scenario_of(*bundle, "settings-resolution-matrix").at("inputs").at("cases");
    std::size_t replayed = 0;
    std::size_t override_cases = 0;
    for (const auto& recorded : cases.get_array()) {
        const auto& case_inputs = recorded.at("inputs");
        // The four `apply-overrides-*` cases record pi's in-memory
        // `applyOverrides` payloads (`NaN`, `Infinity`, `undefined`, a
        // hidden/zero merge) and their `beforeOverride`/`afterOverride`
        // resolutions; the C++ settings manager has no override application
        // path, so they stay explicitly unrereplayed here.
        if (case_inputs.get_object().contains("override")) {
            // The excluded case's `beforeOverride`/`afterOverride`
            // resolutions have no C++ seam; assert the record is one of those
            // cases so the count above cannot silently absorb another shape.
            CHECK(recorded.at("expected").get_object().contains("beforeOverride"));
            CHECK(recorded.at("expected").get_object().contains("afterOverride"));
            ++override_cases;
            continue;
        }
        INFO(recorded.at("name").get_string());
        EvidenceSettingsWorkspace settings;
        settings.write_global(case_inputs.at("global"));
        if (case_inputs.get_object().contains("project")) settings.write_project(case_inputs.at("project"));
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());
        check_resolved_getters(manager, recorded.at("expected"));
        ++replayed;
    }
    CHECK(replayed == 19);
    CHECK(override_cases == 4);
}

TEST_CASE("Named pi-v1.0.4 fullscreen-settings evidence replays the setter surgery and reload",
        "[coding_agent][tui][fullscreen][settings][issue1020][compat-pi]") {
    const auto bundle = cch::tests::read_pi_tui_evidence("fullscreen-settings.json");
    REQUIRE(bundle);

    const auto& scenario = scenario_of(*bundle, "settings-setters-surgery-reload");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto check_texts = [](const EvidenceSettingsWorkspace& settings, const JsonValue& stage) {
        const auto global_mismatch = json_text_mismatch(stage.at("globalText").get_string(), settings.read_global());
        INFO(global_mismatch.value_or(""));
        CHECK_FALSE(global_mismatch.has_value());
        const auto project_mismatch = json_text_mismatch(stage.at("projectText").get_string(), settings.read_project());
        INFO(project_mismatch.value_or(""));
        CHECK_FALSE(project_mismatch.has_value());
    };

    EvidenceSettingsWorkspace settings;
    settings.write_global(inputs.at("globalSeed"));
    settings.write_project(inputs.at("projectSeed"));
    auto manager = settings.manager();
    REQUIRE(manager.errors().empty());

    // `before`: the seeds on disk and the resolved merged view (the project
    // wheel shadows the global value).
    check_texts(settings, expected.at("before"));
    check_resolved_getters(manager, expected.at("before").at("resolved"));

    // The typed setters pi's JS expresses as `setFullscreenScrollbar('hidden')`,
    // `setFullscreenCopyOnSelect(false)` and `setFullscreenWheelScrollLines(7)`.
    REQUIRE(manager.set_fullscreen_scrollbar(FullscreenScrollbar::Hidden));
    REQUIRE(manager.set_fullscreen_copy_on_select(false));
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(FullscreenWheelScrollLines{std::size_t{7}}));
    check_texts(settings, expected.at("afterSetters"));
    check_resolved_getters(manager, expected.at("afterSetters").at("resolved"));

    // `reload()` reads both scopes back.
    REQUIRE(manager.reload());
    CHECK(manager.errors().empty());
    check_resolved_getters(manager, expected.at("afterReload"));

    // The invalid wire spellings (`'bogus' as any`, `'false' as any`, `-3 as
    // any`) cannot be produced by the typed setters; the artifact's own global
    // text for that stage is loaded and its resolution replayed.
    settings.write_global_text(expected.at("afterInvalidSetters").at("globalText").get_string());
    REQUIRE(manager.reload());
    CHECK(manager.errors().empty());
    check_texts(settings, expected.at("afterInvalidSetters"));
    check_resolved_getters(manager, expected.at("afterInvalidSetters").at("resolved"));

    // The wheel floor/clamp stages are representable typed: pi's `2.7`, `100000`
    // and `'auto'` reach the C++ setter as their resolved typed values
    // (`2`, `100`, `auto`), and the recorded disk text is the following stage.
    const std::pair<std::size_t, std::string_view> wheel_stages[]{
            {2, "afterFractionSetter"},
            {100, "afterOverSetter"},
    };
    for (const auto& [lines, stage_name] : wheel_stages) {
        INFO(stage_name);
        REQUIRE(manager.set_fullscreen_wheel_scroll_lines(FullscreenWheelScrollLines{lines}));
        const auto& stage = expected.at(std::string{stage_name});
        check_texts(settings, stage);
        const auto resolved = manager.get_fullscreen_wheel_scroll_lines();
        REQUIRE(std::holds_alternative<std::size_t>(resolved));
        CHECK(std::get<std::size_t>(resolved) ==
                size_of(stage.at("resolved").at("fullscreenWheelScrollLines").at("value")));
    }
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(FullscreenWheelScrollLines{AutoWheelScrollLines{}}));
    check_texts(settings, expected.at("afterAutoSetter"));
    CHECK(wheel_spelling(manager.get_fullscreen_wheel_scroll_lines()) ==
            value_spelling(expected.at("afterAutoSetter").at("resolved").at("fullscreenWheelScrollLines").at("value")));
}

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

        // The variant's raw settings resolve through the real SettingsManager;
        // the wrong-type variant records its raw string getter as the wire
        // value, which resolves truthily.
        EvidenceSettingsWorkspace settings;
        settings.write_global(raw_settings_of_variant(variant));
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());

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
                theme, test_keybindings(), selector_config_of(manager), std::move(callbacks));

        // Every observed callback is appended before the per-cycle buffer is
        // cleared, so the final comparison is the observed order, never a
        // re-derivation from the fixture's own cycle lists.
        std::vector<RecordedChange> observed_changes;
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
                observed_changes.push_back(changes.front());
                changes.clear();
            }
        }
        CHECK(observed_changes == changes_of(expected.at("changes")));
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
        // The recorded provenance names the persisted raw value; the real
        // SettingsManager resolves it before the component is built.
        const std::optional<std::string> persisted = persisted_scrollbar_of(recorded_case);
        EvidenceSettingsWorkspace settings;
        settings.write_global(persisted.has_value()
                                      ? JsonValue{JsonValue::object_t{{"fullscreenScrollbar", JsonValue{*persisted}}}}
                                      : JsonValue{JsonValue::object_t{}});
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());
        const auto resolved = manager.get_fullscreen_scrollbar();
        CHECK(product_scrollbar_name(resolved) == recorded_case.at("resolvedValue").get_string());
        const auto mode = scrollbar_mode(product_scrollbar_name(resolved));
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
        // `rawLines` style boundary: only the scrollbar cell is compared
        // (glyph plus SGR foreground); the OSC-8 envelope and reset bytes are
        // the xterm representation, not Pike cell state.
        const auto check_raw_styles = [&](const JsonValue& frame) {
            const auto raw_lines = strings_of(frame.at("rawLines"));
            REQUIRE(raw_lines.size() == terminal.cells().size());
            for (std::size_t row = 0; row < raw_lines.size(); ++row) {
                const auto& last_cell = terminal.cells()[row].back();
                const auto styled = styled_scrollbar_cell(raw_lines[row]);
                if (styled.has_value()) {
                    CHECK(last_cell.grapheme == styled->glyph);
                    CHECK(last_cell.style.fg_color == styled->fg_color);
                } else {
                    CHECK_FALSE(is_scrollbar_glyph(last_cell.grapheme));
                }
            }
        };
        // Every recorded phase carries the position, visibility, resolved
        // content width, the full layout window and the styled frame; the
        // initial state also carries `viewportHeight` and `scrollbar`.
        const auto check_state = [&](const JsonValue& expected) {
            CHECK(viewport->scroll_top() == size_of(expected.at("scrollTop")));
            if (expected.get_object().contains("viewportHeight")) {
                CHECK(viewport->viewport_height() == size_of(expected.at("viewportHeight")));
            }
            if (expected.get_object().contains("scrollbar")) {
                CHECK(scrollbar_name(viewport->scrollbar()) == expected.at("scrollbar").get_string());
            }
            if (expected.get_object().contains("isScrollbarActive")) {
                CHECK(viewport->is_scrollbar_active() == expected.at("isScrollbarActive").get_boolean());
            }
            CHECK(viewport->is_scrollbar_visible() == expected.at("isScrollbarVisible").get_boolean());
            CHECK(frame_rows() == strings_of(expected.at("frame").at("lines")));
            if (expected.get_object().contains("contentWidthAtTen")) {
                CHECK(viewport->content_width(10) == size_of(expected.at("contentWidthAtTen")));
            }
            const auto& window = expected.at("frame").at("window");
            CHECK(terminal.dimensions().columns == size_of(window.at("width")));
            CHECK(terminal.dimensions().rows == size_of(window.at("height")));
            CHECK(viewport->content_width(10) == size_of(window.at("childWidth")));
            CHECK(viewport->content_height() == size_of(window.at("childHeight")));
            check_raw_styles(expected.at("frame"));
        };

        // Initial frame: following the content end.
        REQUIRE(screen.render());
        const auto& initial = recorded_case.at("initial");
        CHECK(viewport->content_width(10) == size_of(initial.at("contentWidthAtTen")));
        CHECK(viewport->content_width(1) == size_of(initial.at("contentWidthAtOne")));
        check_state(initial);

        // `setScrollbar` request-render counters: the same value is a no-op,
        // a changed value requests one render (pi `requestRender`).
        {
            std::size_t requests = 0;
            cch::tui::ScrollView probe(std::make_unique<cch::tui::Text>(numbered_lines(8), 0, 0),
                    cch::tui::ScrollViewOptions{.follow_end = true, .scrollbar = cch::tui::ScrollbarMode::Auto});
            probe.update_layout(8, 4, [&requests] { ++requests; });
            probe.set_scrollbar(cch::tui::ScrollbarMode::Auto);
            CHECK(requests == size_of(recorded_case.at("setScrollbarSameValueRequestRenderDelta")));
            probe.set_scrollbar(cch::tui::ScrollbarMode::Hidden);
            CHECK(requests == size_of(recorded_case.at("setScrollbarSameValueRequestRenderDelta")) +
                                    size_of(recorded_case.at("setScrollbarChangedValueRequestRenderDelta")));
        }

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
        const auto& alternate = recorded_case.at("alternateFrame");
        CHECK(viewport->is_scrollbar_visible() == alternate.at("isScrollbarVisible").get_boolean());
        CHECK(viewport->content_width(10) == size_of(alternate.at("window").at("childWidth")));
        CHECK(frame_rows() == strings_of(alternate.at("lines")));
        check_raw_styles(alternate);

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
    // with room to consume every notch and compares the applied line deltas to
    // the artifact's recorded matrix (the per-notch timing/direction are the
    // test's input side).
    struct MatrixRow {
        std::string_view name;
        cch::tui::WheelScrollLines lines;
        std::vector<Notch> notches;
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
            {"autoBurst20", cch::tui::AutoWheelScrollLines{}, burst(std::chrono::milliseconds{20}, 4, true)},
            {"autoSteady100", cch::tui::AutoWheelScrollLines{}, burst(std::chrono::milliseconds{100}, 4, true)},
            {"directionChange", cch::tui::AutoWheelScrollLines{}, std::move(direction_change)},
            {"noAccelerate", cch::tui::AutoWheelScrollLines{}, burst(std::chrono::milliseconds{300}, 3, true)},
            {"fixedFive", std::size_t{5}, burst(std::chrono::milliseconds{0}, 3, true)},
            {"fixedZero", std::size_t{0}, burst(std::chrono::milliseconds{0}, 2, true)},
            {"fixedFraction", std::size_t{2}, burst(std::chrono::milliseconds{0}, 2, true)},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto& expected = matrix.at(std::string{row.name}).get_array();
        REQUIRE(expected.size() == row.notches.size());

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
            CHECK(std::abs(delta) == static_cast<std::ptrdiff_t>(size_of(expected[index])));
            CHECK((delta < 0) == row.notches[index].up);
        }
        REQUIRE(screen.stop());
    }

    // The resolved configurations start at the recorded absolute scroll
    // position and replay the recorded deltas. Each row's raw wire input is
    // reconstructed from the artifact's case metadata (`resolvedFixedFive`
    // persisted 5, `resolvedInvalidZero` the floor of a persisted 0,
    // `resolvedAuto`/`resolvedInvalidString` the auto fallbacks of a persisted
    // `"auto"`/`"5"`) and resolved through the real SettingsManager before the
    // live `AltScreen` consumer runs.
    struct ResolvedRow {
        std::string_view name;
        JsonValue raw;
        std::string resolved;
    };
    const auto numeric_setting = [](double value) {
        return JsonValue{JsonValue::object_t{{"fullscreenWheelScrollLines", JsonValue{value}}}};
    };
    const std::vector<ResolvedRow> resolved{
            {"resolvedFixedFive", numeric_setting(5), "5"},
            {"resolvedInvalidZero", numeric_setting(0), "1"},
            {"resolvedAuto", JsonValue{JsonValue::object_t{{"fullscreenWheelScrollLines", JsonValue{"auto"}}}}, "auto"},
            {"resolvedInvalidString", JsonValue{JsonValue::object_t{{"fullscreenWheelScrollLines", JsonValue{"5"}}}}, "auto"},
    };
    for (const auto& row : resolved) {
        INFO(row.name);
        EvidenceSettingsWorkspace settings;
        settings.write_global(row.raw);
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());
        const auto resolved_value = manager.get_fullscreen_wheel_scroll_lines();
        CHECK(wheel_spelling(resolved_value) == row.resolved);

        const auto& expected = scenario.at("expected").at(std::string{row.name});
        const auto& deltas = expected.at("deltas").get_array();

        ManualWheelClock clock;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
        cch::tui::AltScreen screen(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = toolkit_wheel(resolved_value),
                        .wheel_clock = [&clock] { return clock.now(); },
                });
        auto chat = create_chat_viewport(chat_viewport_options(numbered_lines(40)));
        REQUIRE(chat);
        auto* const viewport = chat->transcript;
        screen.set_layout_root(std::move(chat->root));
        REQUIRE(screen.start());
        REQUIRE(screen.render());
        viewport->scroll_to(static_cast<std::ptrdiff_t>(size_of(deltas[0].at("before"))));
        REQUIRE(screen.render());
        std::vector<std::size_t> advances;
        for (std::size_t index = 0; index < deltas.size(); ++index) {
            const auto& recorded = deltas[index];
            const std::size_t advance =
                    recorded.get_object().contains("advanceMs") ? size_of(recorded.at("advanceMs")) : 0;
            advances.push_back(advance);
            clock.advance(std::chrono::milliseconds{advance});
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
            const auto after = viewport->scroll_top();
            INFO(std::string{"notch "} + std::to_string(index));
            CHECK(before == size_of(recorded.at("before")));
            CHECK(after == size_of(recorded.at("after")));
            CHECK(static_cast<std::ptrdiff_t>(after) - static_cast<std::ptrdiff_t>(before) ==
                    static_cast<std::ptrdiff_t>(recorded.at("delta").get_number()));
        }
        if (row.name == "resolvedAuto" || row.name == "resolvedInvalidString") {
            CHECK(advances == sizes_of(scenario.at("inputs").at("autoClockStepsMs")));
        } else {
            CHECK(advances == std::vector<std::size_t>(deltas.size(), 0));
        }
        CHECK(viewport->scroll_top() == size_of(expected.at("scrolledTop")));
        REQUIRE(screen.stop());
    }

    // Live `setWheelScrollLines`: fixed five first, then the accelerator from a
    // reset state; every recorded delta/advance comes from the artifact.
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
        CHECK(fixed_deltas == numbers_of(expected.at("fixedFiveDeltas")));

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
        std::vector<std::ptrdiff_t> recorded_auto;
        for (const auto& recorded : expected.at("autoDeltas").get_array()) {
            recorded_auto.push_back(static_cast<std::ptrdiff_t>(recorded.at("delta").get_number()));
        }
        CHECK(auto_deltas == recorded_auto);
        REQUIRE(screen.stop());
    }

    // The `createInteractiveTui` option forwarding: the resolved option is the
    // `AltScreen` wheel line count. The live engine forwards the resolved
    // setting end-to-end in `FullscreenSettingsInteractiveTest.cpp`.
    {
        const auto& expected = scenario.at("expected").at("createInteractiveTuiForwardedFixedFiveDeltas");
        EvidenceSettingsWorkspace settings;
        settings.write_global(numeric_setting(5));
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());
        CHECK(wheel_spelling(manager.get_fullscreen_wheel_scroll_lines()) == "5");

        ManualWheelClock clock;
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
        cch::tui::AltScreen screen(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = toolkit_wheel(manager.get_fullscreen_wheel_scroll_lines()),
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
        std::vector<std::ptrdiff_t> forwarded;
        for (std::size_t index = 0; index < expected.get_array().size(); ++index) {
            const auto before = viewport->scroll_top();
            REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
            forwarded.push_back(
                    static_cast<std::ptrdiff_t>(viewport->scroll_top()) - static_cast<std::ptrdiff_t>(before));
        }
        CHECK(forwarded == numbers_of(expected));
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

    // Each case's raw inputs are the resolution-matrix case it names; the real
    // SettingsManager resolves them and the host consumer runs the resolved
    // typed value. `manualCtrlXErrors` is the product `handleCopyCommand`
    // fall-through error after the host helper declines, exercised on the live
    // engine path in `FullscreenSettingsInteractiveTest.cpp`; the typed
    // `createdTuiForwardedCopyOnSelect`/`rendererGetter` raw values are the
    // documented bool here.
    const std::pair<std::string_view, std::string_view> resolved_cases[]{
            {"default-true", "defaults"},
            {"valid-false", "global-valid"},
            {"wrong-type-string-false", "global-copy-wrong-type-string-false"},
            {"wrong-type-zero", "global-copy-wrong-type-zero"},
    };
    for (const auto& [case_name, resolution_case] : resolved_cases) {
        INFO(case_name);
        const auto& expected = named_case_of(scenario.at("expected").at("cases"), case_name);
        EvidenceSettingsWorkspace settings;
        settings.write_global(resolution_case_of(*bundle, resolution_case).at("inputs").at("global"));
        auto manager = settings.manager();
        REQUIRE(manager.errors().empty());
        const bool copy_on_select = manager.get_fullscreen_copy_on_select();
        CHECK(copy_on_select == expected.at("resolvedGetter").at("truthy").get_boolean());
        // The renderer and the created TUI receive the typed bool; the
        // artifact's raw JS values (`rendererGetter`,
        // `createdTuiForwardedCopyOnSelect`) project onto it through
        // JavaScript truthiness.
        CHECK(copy_on_select == js_truthy(expected.at("rendererGetter").at("value")));
        CHECK(copy_on_select == js_truthy(expected.at("createdTuiForwardedCopyOnSelect").at("value")));

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
        // through to the last-message path and reports that error instead. The
        // artifact's `manualCtrlXErrors` names exactly that fall-through, so
        // it is non-empty when the keyboard path is not handled; the live
        // engine asserts the diagnostic text in
        // `FullscreenSettingsInteractiveTest.cpp`.
        CHECK(!expected.at("manualCtrlXErrors").get_array().empty() == copy_on_select);
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
    const auto& inputs = scenario.at("inputs");
    const auto& before_apply = scenario.at("expected").at("beforeApply");
    const auto& after_apply = scenario.at("expected").at("afterApply");
    const auto& after_regular = scenario.at("expected").at("afterRegularSwitch");
    const auto& after_back = scenario.at("expected").at("afterBackSwitch");

    // `inputs.settingsBefore` seeds the real SettingsManager; the product
    // viewport takes the resolved scrollbar (`createChatViewport({scrollbar})`)
    // and the renderer the resolved copy/wheel values.
    EvidenceSettingsWorkspace settings;
    settings.write_global(inputs.at("settingsBefore"));
    auto manager = settings.manager();
    REQUIRE(manager.errors().empty());

    ManualWheelClock clock;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 10});
    cch::tui::AltScreen screen(terminal,
            cch::tui::AltScreenOptions{
                    .copy_on_select = manager.get_fullscreen_copy_on_select(),
                    .wheel_scroll_lines = toolkit_wheel(manager.get_fullscreen_wheel_scroll_lines()),
                    .wheel_clock = [&clock] { return clock.now(); },
            });
    auto viewport = create_chat_viewport(chat_viewport_options(numbered_lines(40)));
    REQUIRE(viewport);
    auto* const transcript = viewport->transcript;
    transcript->set_scrollbar(scrollbar_mode(product_scrollbar_name(manager.get_fullscreen_scrollbar())));
    screen.set_layout_root(std::move(viewport->root));
    REQUIRE(screen.start());
    REQUIRE(screen.render());
    // The frozen harness's 40 literal numbered lines land on the recorded
    // absolute start (33) for the 20x10 product viewport, the same start
    // scenario 6's resolved rows record.
    const auto recorded_start = size_of(scenario_of(*bundle, "wheel-settings-fixed-vs-auto")
                                               .at("expected")
                                               .at("resolvedFixedFive")
                                               .at("deltas")
                                               .get_array()[0]
                                               .at("before"));
    transcript->scroll_to(static_cast<std::ptrdiff_t>(recorded_start));
    REQUIRE(screen.render());
    CHECK(transcript->scroll_top() == recorded_start);

    CHECK(scrollbar_name(transcript->scrollbar()) == before_apply.at("transcriptScrollbar").get_string());
    CHECK(screen.copy_on_select() == before_apply.at("rendererCopyOnSelect").get_boolean());

    // The runtime settings update (`applyRuntimeSettings`):
    // `inputs.settingsAfter` is written through the real manager, the resolved
    // values are checked against the artifact and then applied to the live
    // consumers.
    const auto& settings_after = inputs.at("settingsAfter");
    REQUIRE(manager.set_fullscreen_scrollbar(
            fullscreen_scrollbar_of(settings_after.at("fullscreenScrollbar").get_string())));
    REQUIRE(manager.set_fullscreen_copy_on_select(settings_after.at("fullscreenCopyOnSelect").get_boolean()));
    REQUIRE(manager.set_fullscreen_wheel_scroll_lines(wheel_value(settings_after.at("fullscreenWheelScrollLines"))));
    const auto& resolved = after_apply.at("settingsResolved");
    CHECK(product_scrollbar_name(manager.get_fullscreen_scrollbar()) ==
            resolved.at("fullscreenScrollbar").at("value").get_string());
    CHECK(manager.get_fullscreen_copy_on_select() == resolved.at("fullscreenCopyOnSelect").at("truthy").get_boolean());
    CHECK(wheel_spelling(manager.get_fullscreen_wheel_scroll_lines()) ==
            value_spelling(resolved.at("fullscreenWheelScrollLines").at("value")));

    transcript->set_scrollbar(scrollbar_mode(product_scrollbar_name(manager.get_fullscreen_scrollbar())));
    screen.set_copy_on_select(manager.get_fullscreen_copy_on_select());
    screen.set_wheel_scroll_lines(toolkit_wheel(manager.get_fullscreen_wheel_scroll_lines()));
    REQUIRE(screen.render());
    CHECK(scrollbar_name(transcript->scrollbar()) == after_apply.at("transcriptScrollbar").get_string());
    CHECK_FALSE(screen.copy_on_select());

    std::vector<std::ptrdiff_t> wheel_deltas;
    const auto& apply_deltas = after_apply.at("wheelDeltas").get_array();
    for (std::size_t index = 0; index < apply_deltas.size(); ++index) {
        const auto before = transcript->scroll_top();
        REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
        wheel_deltas.push_back(
                static_cast<std::ptrdiff_t>(transcript->scroll_top()) - static_cast<std::ptrdiff_t>(before));
    }
    CHECK(wheel_deltas == numbers_of(after_apply.at("wheelDeltas")));

    // The engine's regular switch retains this viewport and its resolved
    // consumer state; the actual `switch_tui_mode` calls, its `result` and
    // the `rendererMode`/`optionsTuiMode` fields (the option mode drives which
    // typed mode the switch is driven with, the renderer mode is observed as
    // the terminal's alternate-screen state) run against the live engine in
    // `FullscreenSettingsInteractiveTest.cpp`.
    CHECK(scrollbar_name(transcript->scrollbar()) == after_regular.at("transcriptScrollbar").get_string());

    // The back switch re-applies the resolved settings on the retained
    // viewport before the recorded deltas run again.
    screen.set_copy_on_select(manager.get_fullscreen_copy_on_select());
    screen.set_wheel_scroll_lines(toolkit_wheel(manager.get_fullscreen_wheel_scroll_lines()));
    REQUIRE(screen.render());
    CHECK(scrollbar_name(transcript->scrollbar()) == after_back.at("transcriptScrollbar").get_string());
    CHECK_FALSE(screen.copy_on_select());
    std::vector<std::ptrdiff_t> back_deltas;
    const auto& back_recorded_deltas = after_back.at("wheelDeltas").get_array();
    for (std::size_t index = 0; index < back_recorded_deltas.size(); ++index) {
        const auto before = transcript->scroll_top();
        REQUIRE(terminal.inject_input("\x1b[<64;5;3M"));
        back_deltas.push_back(
                static_cast<std::ptrdiff_t>(transcript->scroll_top()) - static_cast<std::ptrdiff_t>(before));
    }
    CHECK(back_deltas == numbers_of(after_back.at("wheelDeltas")));
    CHECK(transcript->scroll_top() == size_of(after_back.at("transcriptScrollTop")));
    REQUIRE(screen.stop());
}

} // namespace cch::coding_agent::tui
