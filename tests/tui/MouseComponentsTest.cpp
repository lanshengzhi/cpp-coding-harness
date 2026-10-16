// Named pi-v1.0.4 acceptance for mouse editing and list/settings selection
// (#1003). Every expectation is read from the verified named bundle through
// `scripts/tui/evidence.py`; nothing here regenerates expectations from Pike or
// falls back to the historical `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;
using cch::tui::MouseButton;
using cch::tui::MouseEvent;
using cch::tui::MouseEventResult;
using cch::tui::MouseEventType;

[[nodiscard]] bool is_null(const JsonValue& value) { return value.holds<std::nullptr_t>(); }

[[nodiscard]] std::ptrdiff_t int_of(const JsonValue& value) {
    return static_cast<std::ptrdiff_t>(std::llround(value.get_number()));
}

[[nodiscard]] std::size_t size_of(const JsonValue& value) { return static_cast<std::size_t>(int_of(value)); }

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

/// Rendered rows as the frozen observations record them: terminal sequences
/// stripped (`plainRows` in the capture, `visibleLines` in the pi tests) and
/// trailing padding removed.
[[nodiscard]] std::string drawn_row(std::string_view row) {
    auto text = cch::tui::strip_terminal_sequences(row);
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

[[nodiscard]] std::vector<std::string> drawn_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen())
        rows.push_back(drawn_row(row));
    return rows;
}

[[nodiscard]] std::vector<std::string> drawn_rows(const cch::tui::RenderResult& rendered) {
    std::vector<std::string> rows;
    rows.reserve(rendered.lines.size());
    for (const auto& line : rendered.lines)
        rows.push_back(drawn_row(line));
    return rows;
}

/// Frozen rows as the toolkit's own `render()` output exposes them. pi records
/// the `render()` result including the reverse-video cursor cell and the width
/// padding (the capture's `plainRows` only strips the former's styling), while
/// the toolkit returns the same text without trailing padding.
[[nodiscard]] std::vector<std::string> plain_lines(const JsonValue& rows) {
    std::vector<std::string> lines;
    for (const auto& row : rows.get_array())
        lines.push_back(drawn_row(row.get_string()));
    return lines;
}

/// The cell column of the reverse-video cursor cell the toolkit paints into its
/// own row (pi `\x1b[7m<grapheme>\x1b[27m`, `input.ts:480-485`).
[[nodiscard]] std::size_t cursor_column_of(std::string_view row) {
    const auto marker = row.find("\x1b[7m");
    REQUIRE(marker != std::string_view::npos);
    return cch::tui::visible_width(row.substr(0, marker));
}

/// Drop settings hint rows from a screen comparison. Frozen pi hardcodes the
/// hint text (`components/settings-list.ts:319-322`: `Enter/Space to change ·
/// Esc to cancel`), while the toolkit builds the same line from the keybinding
/// registry (`SettingsList.cpp:376-387`), which spells the keys in its own
/// style. That divergence predates #1003 and is not what the family records, so
/// every other row — in particular every row a mouse event resolved to — is
/// compared exactly.
[[nodiscard]] bool is_settings_hint(std::string_view row) {
    return row.find(" to change · ") != std::string_view::npos;
}

[[nodiscard]] std::vector<std::string> without_settings_hint(std::vector<std::string> rows) {
    std::erase_if(rows, [](const std::string& row) { return is_settings_hint(row); });
    return rows;
}

[[nodiscard]] std::size_t hint_row_count(const std::vector<std::string>& rows) {
    return static_cast<std::size_t>(
            std::count_if(rows.begin(), rows.end(), [](const std::string& row) { return is_settings_hint(row); }));
}

void check_screen(const std::vector<std::string>& actual, const JsonValue& frozen, std::string_view label) {
    INFO(label);
    const auto expected = string_lines(frozen);
    CHECK(without_settings_hint(actual) == without_settings_hint(expected));
    // The hint row itself stays evidence: it must exist on both sides, only its
    // keybinding spelling differs.
    CHECK(hint_row_count(actual) == hint_row_count(expected));
}

[[nodiscard]] MouseEventType type_of(std::string_view value) {
    if (value == "press") return MouseEventType::Press;
    if (value == "release") return MouseEventType::Release;
    if (value == "move") return MouseEventType::Move;
    if (value == "drag") return MouseEventType::Drag;
    if (value == "click") return MouseEventType::Click;
    if (value == "wheel") return MouseEventType::Wheel;
    REQUIRE(false);
    return MouseEventType::Press;
}

[[nodiscard]] MouseButton button_of(std::string_view value) {
    if (value == "left") return MouseButton::Left;
    if (value == "middle") return MouseButton::Middle;
    if (value == "right") return MouseButton::Right;
    if (value == "none") return MouseButton::None;
    REQUIRE(false);
    return MouseButton::None;
}

/// One frozen event record. The capture stores the fields the frozen `mouse()`
/// helper (`mouse-components.test.ts:9-22`) was called with: `x`/`y` are the
/// component-local coordinates and the screen coordinates default to them. The
/// named `press`/`click` observations of a scenario record the same fields
/// without repeating the kind, so the caller names the kind it replays.
[[nodiscard]] MouseEvent event_of(
        const JsonValue& recorded, MouseEventType type, MouseButton button = MouseButton::Left) {
    MouseEvent event;
    event.type = type;
    event.button = button;
    event.x = int_of(recorded.at("x"));
    event.y = int_of(recorded.at("y"));
    event.screen_x = event.x;
    event.screen_y = event.y;
    event.width = size_of(recorded.at("width"));
    event.height = size_of(recorded.at("height"));
    if (event.type == MouseEventType::Click) event.click_count = 1;
    return event;
}

/// One frozen answer: `null` when the frozen `handleMouse` returned undefined.
struct FrozenAnswer {
    bool present{false};
    bool handled{false};
    bool focus{false};
    std::optional<bool> render{};
};

[[nodiscard]] FrozenAnswer answer_of(const JsonValue& value) {
    FrozenAnswer answer;
    if (is_null(value)) return answer;
    answer.present = true;
    answer.handled = value.at("handled").get_boolean();
    answer.focus = value.at("focus").get_boolean();
    if (!is_null(value.at("render"))) answer.render = value.at("render").get_boolean();
    return answer;
}

void check_answer(const std::optional<MouseEventResult>& actual, const JsonValue& frozen, std::string_view label) {
    INFO(label);
    const auto expected = answer_of(frozen);
    REQUIRE(actual.has_value() == expected.present);
    if (!actual) return;
    CHECK(actual->handled == expected.handled);
    CHECK(actual->focus == expected.focus);
    CHECK(actual->render == expected.render);
}

[[nodiscard]] cch::support::Expected<JsonValue> mouse_components_bundle() {
    return cch::tests::read_pi_tui_evidence("mouse-components.json");
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] cch::tui::InputOptions prompt_options() { return cch::tui::InputOptions{.prompt = "> "}; }

template <typename Component> void type_key(Component& component, std::string text) {
    static_cast<void>(component.handle_input(cch::tui::KeyEvent{.key = std::move(text)}));
}

[[nodiscard]] std::vector<cch::tui::SelectItem> letter_items(const JsonValue& inputs) {
    std::vector<cch::tui::SelectItem> items;
    for (const auto& value : inputs.at("items").get_array()) {
        auto text = value.get_string();
        auto label = text;
        if (!label.empty()) label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
        items.push_back(cch::tui::SelectItem{.value = std::move(text), .label = std::move(label)});
    }
    return items;
}

/// The frozen editor fixtures pass `borderColor: identity`
/// (`mouse-components.test.ts:37`), and only a border hook renders the two
/// border rows the recorded screens and clicks are positioned against.
void use_frozen_border(cch::tui::Editor& editor) {
    editor.set_border_style([](std::string text) { return text; });
}

/// The frozen settings fixtures style the cursor with `"> "`
/// (`mouse-components.test.ts:35`), not the toolkit's default.
[[nodiscard]] cch::tui::SettingsListTheme frozen_settings_theme() {
    return cch::tui::SettingsListTheme{.cursor = "> "};
}

/// Numbered settings rows the frozen observations build: `Id <i>`, `Item <i>`,
/// optional description, `off` value cycled through `on`.
[[nodiscard]] std::vector<cch::tui::SettingItem> numbered_settings(std::size_t count, bool with_description) {
    std::vector<cch::tui::SettingItem> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        cch::tui::SettingItem item{.id = "item-" + std::to_string(index),
                .label = "Item " + std::to_string(index),
                .current_value = "off",
                .control = cch::tui::SettingValues{.values = {"off", "on"}}};
        if (with_description) item.description = "Description " + std::to_string(index);
        items.push_back(std::move(item));
    }
    return items;
}

[[nodiscard]] std::vector<cch::tui::SelectItem> numbered_select_items(std::size_t count) {
    std::vector<cch::tui::SelectItem> items;
    items.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        items.push_back(cch::tui::SelectItem{
                .value = "item-" + std::to_string(index), .label = "Item " + std::to_string(index)});
    }
    return items;
}

[[nodiscard]] std::vector<std::string> recorded_changes(const JsonValue& value) {
    std::vector<std::string> changes;
    for (const auto& change : value.get_array())
        changes.push_back(change.get_string());
    return changes;
}

/// A host that renders one child and delegates both keys and mouse events to
/// it, like the frozen `Container` subclasses (`InputOverlay`,
/// `SubmenuHost`): the handler routes keys to the child itself, so a forwarded
/// result keeps this host as the keyboard focus target.
class DelegatingHost : public cch::tui::Component,
                       public cch::tui::MouseHandler,
                       public cch::tui::InputHandler,
                       public cch::tui::Focusable {
public:
    explicit DelegatingHost(std::unique_ptr<cch::tui::Component> child) : child_(std::move(child)) {}

    DelegatingHost(DelegatingHost&&) noexcept = default;
    DelegatingHost& operator=(DelegatingHost&&) noexcept = default;
    ~DelegatingHost() override = default;

    DelegatingHost(const DelegatingHost&) = delete;
    DelegatingHost& operator=(const DelegatingHost&) = delete;

    [[nodiscard]] cch::tui::Component& child() const { return *child_; }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        return child_->render(width);
    }
    void invalidate() override { child_->invalidate(); }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return child_->cursor_location();
    }
    [[nodiscard]] bool contains(const cch::tui::Component* target) const override {
        return target == this || child_->contains(target);
    }
    [[nodiscard]] cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        auto* handler = dynamic_cast<cch::tui::InputHandler*>(child_.get());
        if (handler == nullptr) return cch::tui::InputAdmissionOutcome::Unhandled;
        return handler->handle_input(input);
    }
    [[nodiscard]] std::optional<cch::tui::MouseDispatchResult> handle_mouse_forwarded(
            const cch::tui::MouseEvent& event) override {
        return cch::tui::dispatch_mouse_event(*child_, event);
    }
    void set_focused(bool focused) override {
        auto* focusable = dynamic_cast<cch::tui::Focusable*>(child_.get());
        if (focusable != nullptr) focusable->set_focused(focused);
    }
    [[nodiscard]] bool focused() const override {
        const auto* focusable = dynamic_cast<const cch::tui::Focusable*>(child_.get());
        return focusable != nullptr && focusable->focused();
    }

private:
    std::unique_ptr<cch::tui::Component> child_;
};

/// The frozen submenu (`mouse-components.test.ts:63-84`): a two-row list that
/// reports the picked value through the settings list's done sink.
class FrozenSubmenuHost final : public DelegatingHost {
public:
    explicit FrozenSubmenuHost(cch::tui::SettingsSubmenuDoneSink done)
        : DelegatingHost(std::make_unique<cch::tui::SelectList>(frozen_submenu_items(),
                  cch::tui::SelectListOptions{.max_visible = 5,
                          .on_select = [this](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                              return done_(item.value, std::nullopt);
                          }})),
          done_(std::move(done)) {}

private:
    [[nodiscard]] static std::vector<cch::tui::SelectItem> frozen_submenu_items() {
        return std::vector<cch::tui::SelectItem>{
                {.value = "first", .label = "First"}, {.value = "second", .label = "Second"}};
    }

    cch::tui::SettingsSubmenuDoneSink done_;
};

} // namespace

TEST_CASE("an input press positions the cursor and later gestures stay unhandled",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "input-press-cursor-and-unhandled-gestures");
    const auto& frozen = scenario.at("expected");
    const auto typed = scenario.at("inputs").at("typed").get_string();

    for (const auto& expected : frozen.at("cases").get_array()) {
        const auto label = expected.at("label").get_string();
        INFO(label);
        cch::tui::Input input(cch::tui::InputOptions{.prompt = expected.at("prompt").get_string()});
        input.set_value(expected.at("value").get_string());
        const auto width = size_of(expected.at("width"));
        const auto before = input.render(width);
        REQUIRE(before);

        const auto answer = input.handle_mouse(event_of(
                expected, type_of(expected.at("type").get_string()), button_of(expected.at("button").get_string())));
        check_answer(answer, expected.at("result"), label);

        const auto after = input.render(width);
        REQUIRE(after);
        REQUIRE(after->lines.size() == 1);
        CHECK(drawn_row(after->lines.front()) == expected.at("row").get_string());
        CHECK(cursor_column_of(after->lines.front()) == size_of(expected.at("cursorColumn")));

        type_key(input, typed);
        CHECK(input.value() == expected.at("valueAfterType").get_string());
    }
}

TEST_CASE("an input press maps grapheme widths and the scrolled window's start column",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "input-press-maps-grapheme-widths-and-scrolled-columns");
    const auto& frozen = scenario.at("expected");
    const auto typed = scenario.at("inputs").at("typed").get_string();

    for (const auto& expected : frozen.at("cases").get_array()) {
        const auto label = expected.at("label").get_string();
        INFO(label);
        cch::tui::Input input(cch::tui::InputOptions{.prompt = expected.at("prompt").get_string()});
        input.set_value(expected.at("value").get_string());
        const auto width = size_of(expected.at("width"));
        const auto y = int_of(expected.at("y"));
        for (const auto& step : expected.at("steps").get_array()) {
            const auto before = input.render(width);
            REQUIRE(before);
            const auto answer = input.handle_mouse(MouseEvent{.type = MouseEventType::Press,
                    .button = MouseButton::Left,
                    .x = int_of(step.at("x")),
                    .y = y,
                    .screen_x = int_of(step.at("x")),
                    .screen_y = y,
                    .width = width,
                    .height = size_of(expected.at("height"))});
            check_answer(answer, step.at("result"), label);

            const auto after = input.render(width);
            REQUIRE(after);
            REQUIRE(after->lines.size() == 1);
            INFO("press at " << int_of(step.at("x")));
            CHECK(drawn_row(after->lines.front()) == step.at("row").get_string());
            CHECK(cursor_column_of(after->lines.front()) == size_of(step.at("cursorColumn")));
        }
        type_key(input, typed);
        CHECK(input.value() == expected.at("valueAfterType").get_string());
    }
}

TEST_CASE("a select-list press selects the rendered row and a click activates it",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "select-list-press-selects-and-click-activates");
    const auto& frozen = scenario.at("expected").at("pressClick");

    std::optional<std::string> activated;
    auto options = cch::tui::SelectListOptions{.max_visible = size_of(frozen.at("maxVisible"))};
    options.on_select = [&activated](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
        activated = item.value;
        return {};
    };
    cch::tui::SelectList list(letter_items(scenario.at("inputs")), std::move(options));

    const auto& press = frozen.at("press");
    check_answer(list.handle_mouse(event_of(press, MouseEventType::Press)), press.at("result"), "press");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == press.at("selected").get_string());
    CHECK_FALSE(activated.has_value());

    const auto& click = frozen.at("click");
    check_answer(list.handle_mouse(event_of(click, MouseEventType::Click)), click.at("result"), "click");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == click.at("selected").get_string());
    REQUIRE(activated.has_value());
    CHECK(*activated == click.at("selected").get_string());
}

TEST_CASE("a select-list wheel moves one row per notch and hover never moves the selection",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "select-list-wheel-hover-and-scrolled-rows");
    const auto& inputs = scenario.at("inputs");
    const auto& frozen = scenario.at("expected");

    const auto width = size_of(scenario.at("dimensions").at("columns"));
    const auto wheel_delta = int_of(inputs.at("wheelDelta"));
    for (const auto& expected : frozen.at("rows").get_array()) {
        const auto row = int_of(expected.at("row"));
        INFO("row " << row);
        std::vector<std::string> changes;
        auto options = cch::tui::SelectListOptions{.max_visible = size_of(inputs.at("maxVisible"))};
        options.on_selection_change = [&changes](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
            changes.push_back(item.value);
            return {};
        };
        cch::tui::SelectList list(numbered_select_items(size_of(inputs.at("itemCount"))), std::move(options));
        list.set_selected_index(size_of(inputs.at("selectedIndex")));

        const auto& after_wheel = expected.at("afterWheel");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Wheel,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row,
                             .screen_x = 1,
                             .screen_y = row,
                             .width = width,
                             .height = 10,
                             .wheel_delta = wheel_delta}),
                after_wheel.at("result"),
                "wheel");
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == after_wheel.at("selected").get_string());
        CHECK(changes == recorded_changes(after_wheel.at("changes")));

        const auto before = list.render(width);
        REQUIRE(before);
        CHECK(drawn_rows(*before) == string_lines(expected.at("rows")));

        for (const auto& hover : expected.at("hover").get_array()) {
            const auto y = int_of(hover.at("y"));
            INFO("hover row " << y);
            check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Move,
                                 .button = MouseButton::None,
                                 .x = 1,
                                 .y = y,
                                 .screen_x = 1,
                                 .screen_y = y,
                                 .width = width,
                                 .height = 10}),
                    hover.at("result"),
                    "hover");
            const auto after_hover = list.render(width);
            REQUIRE(after_hover);
            CHECK(drawn_rows(*after_hover) == string_lines(hover.at("rows")));
        }

        const auto& press = expected.at("press");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Press,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row,
                             .screen_x = 1,
                             .screen_y = row,
                             .width = width,
                             .height = 10}),
                press.at("result"),
                "press");
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == press.at("selected").get_string());

        const auto& click = expected.at("click");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Click,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row,
                             .screen_x = 1,
                             .screen_y = row,
                             .width = width,
                             .height = 10,
                             .click_count = 1}),
                click.at("result"),
                "click");
        CHECK(changes == recorded_changes(click.at("changes")));
    }
}

TEST_CASE("a settings-list press selects the rendered row and a click activates it",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "settings-list-press-selects-and-click-activates");
    const auto& frozen = scenario.at("expected").at("pressClick");

    std::vector<std::pair<std::string, std::string>> changes;
    auto options = cch::tui::SettingsListOptions{
            .max_visible = size_of(frozen.at("maxVisible")), .theme = frozen_settings_theme()};
    options.on_change = [&changes](std::string id, std::string value) -> cch::support::ExpectedVoid {
        changes.emplace_back(std::move(id), std::move(value));
        return {};
    };
    std::vector<cch::tui::SettingItem> items{{.id = "mode",
                                                     .label = "Mode",
                                                     .current_value = "one",
                                                     .control = cch::tui::SettingValues{.values = {"one", "two"}}},
            {.id = "other",
                    .label = "Other",
                    .current_value = "off",
                    .control = cch::tui::SettingValues{.values = {"off", "on"}}},
            {.id = "third",
                    .label = "Third",
                    .current_value = "low",
                    .control = cch::tui::SettingValues{.values = {"low", "high"}}},
            {.id = "fourth",
                    .label = "Fourth",
                    .current_value = "x",
                    .control = cch::tui::SettingValues{.values = {"x", "y"}}}};
    cch::tui::SettingsList list(std::move(items), std::move(options));

    check_answer(list.handle_mouse(event_of(frozen.at("press"), MouseEventType::Press)),
            frozen.at("press").at("result"),
            "press");
    CHECK(changes.empty());
    check_answer(list.handle_mouse(event_of(frozen.at("click"), MouseEventType::Click)),
            frozen.at("click").at("result"),
            "click");

    std::vector<std::pair<std::string, std::string>> expected_changes;
    for (const auto& change : frozen.at("changes").get_array())
        expected_changes.emplace_back(change.at("id").get_string(), change.at("value").get_string());
    CHECK(changes == expected_changes);
}

TEST_CASE("a settings-list wheel moves one row per notch and hover never moves the selection",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "settings-list-wheel-hover-and-scrolled-rows");
    const auto& inputs = scenario.at("inputs");
    const auto& frozen = scenario.at("expected");

    const auto width = size_of(scenario.at("dimensions").at("columns"));
    const auto wheel_delta = int_of(inputs.at("wheelDelta"));
    for (const auto& expected : frozen.at("rows").get_array()) {
        const auto row = int_of(expected.at("row"));
        INFO("row " << row);
        std::vector<std::pair<std::string, std::string>> changes;
        auto options = cch::tui::SettingsListOptions{.max_visible = size_of(inputs.at("maxVisible")),
                .enable_search = true,
                .theme = frozen_settings_theme()};
        options.on_change = [&changes](std::string id, std::string value) -> cch::support::ExpectedVoid {
            changes.emplace_back(std::move(id), std::move(value));
            return {};
        };
        cch::tui::SettingsList list(numbered_settings(size_of(inputs.at("itemCount")), true), std::move(options));
        list.select_item(inputs.at("selectedItem").get_string());
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->id == inputs.at("selectedItem").get_string());

        const auto row_offset = int_of(expected.at("rowOffset"));
        CHECK(row_offset == 2);

        const auto& after_wheel = expected.at("afterWheel");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Wheel,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row + row_offset,
                             .screen_x = 1,
                             .screen_y = row + row_offset,
                             .width = width,
                             .height = 10,
                             .wheel_delta = wheel_delta}),
                after_wheel.at("result"),
                "wheel");
        const auto before = list.render(width);
        REQUIRE(before);
        check_screen(drawn_rows(*before), after_wheel.at("rows"), "wheel rows");

        for (const auto& hover : expected.at("hover").get_array()) {
            const auto y = int_of(hover.at("y"));
            INFO("hover row " << y);
            check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Move,
                                 .button = MouseButton::None,
                                 .x = 1,
                                 .y = y,
                                 .screen_x = 1,
                                 .screen_y = y,
                                 .width = width,
                                 .height = 10}),
                    hover.at("result"),
                    "hover");
            const auto after_hover = list.render(width);
            REQUIRE(after_hover);
            check_screen(drawn_rows(*after_hover), hover.at("rows"), "hover rows");
        }
        CHECK(changes.empty());

        const auto& press = expected.at("press");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Press,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row + row_offset,
                             .screen_x = 1,
                             .screen_y = row + row_offset,
                             .width = width,
                             .height = 10}),
                press.at("result"),
                "press");
        const auto after_press = list.render(width);
        REQUIRE(after_press);
        check_screen(drawn_rows(*after_press), press.at("rows"), "press rows");
        const auto& click = expected.at("click");
        check_answer(list.handle_mouse(MouseEvent{.type = MouseEventType::Click,
                             .button = MouseButton::Left,
                             .x = 1,
                             .y = row + row_offset,
                             .screen_x = 1,
                             .screen_y = row + row_offset,
                             .width = width,
                             .height = 10,
                             .click_count = 1}),
                click.at("result"),
                "click");
        const auto after_click = list.render(width);
        REQUIRE(after_click);
        check_screen(drawn_rows(*after_click), click.at("rows"), "click rows");

        std::vector<std::pair<std::string, std::string>> expected_changes;
        for (const auto& change : click.at("changes").get_array())
            expected_changes.emplace_back(change.at("id").get_string(), change.at("value").get_string());
        CHECK(changes == expected_changes);
    }
}

TEST_CASE("the settings search row forwards clicks and the separator row stays inert",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "settings-list-search-row-forwards-and-separator-is-inert");
    const auto& inputs = scenario.at("inputs");
    const auto& frozen = scenario.at("expected").at("search");

    std::vector<std::pair<std::string, std::string>> changes;
    auto options = cch::tui::SettingsListOptions{
            .max_visible = size_of(inputs.at("maxVisible")), .enable_search = true, .theme = frozen_settings_theme()};
    options.on_change = [&changes](std::string id, std::string value) -> cch::support::ExpectedVoid {
        changes.emplace_back(std::move(id), std::move(value));
        return {};
    };
    cch::tui::SettingsList list(numbered_settings(size_of(inputs.at("itemCount")), false), std::move(options));
    const auto query = inputs.at("typedQuery").get_string();
    for (const auto ch : query)
        type_key(list, std::string(1, ch));

    REQUIRE(list.search_query() == frozen.at("typedQuery").get_string());
    const auto filtered = list.render(size_of(frozen.at("searchRow").at("width")));
    REQUIRE(filtered);
    check_screen(drawn_rows(*filtered), frozen.at("rows"), "search rows");

    const auto& press = frozen.at("searchRow");
    const auto& separator = frozen.at("separatorRow");
    REQUIRE(filtered->lines.size() > 1);

    // The frozen observation reads the search row's cursor cell before and
    // after the press: the forwarded press moves the search input's cursor.
    const auto before_click = list.render(size_of(press.at("width")));
    REQUIRE(before_click);
    CHECK(cursor_column_of(before_click->lines.front()) == size_of(press.at("cursorColumnBeforeClick")));

    check_answer(list.handle_mouse(event_of(press, MouseEventType::Press)), press.at("result"), "search row");

    const auto after_click = list.render(size_of(press.at("width")));
    REQUIRE(after_click);
    CHECK(drawn_row(after_click->lines.front()) == press.at("afterClick").get_string());
    CHECK(cursor_column_of(after_click->lines.front()) == size_of(press.at("cursorColumnAfterClick")));

    check_answer(
            list.handle_mouse(event_of(separator, MouseEventType::Press)), separator.at("result"), "separator row");

    const auto& first_item = frozen.at("firstItemRow");
    check_answer(
            list.handle_mouse(event_of(first_item, MouseEventType::Press)), first_item.at("press"), "first item press");
    const auto after_press = list.render(size_of(first_item.at("width")));
    REQUIRE(after_press);
    check_screen(drawn_rows(*after_press), first_item.at("rowsAfterPress"), "first item rows");

    check_answer(
            list.handle_mouse(event_of(first_item, MouseEventType::Click)), first_item.at("click"), "first item click");

    std::vector<std::pair<std::string, std::string>> expected_changes;
    for (const auto& change : first_item.at("changesAfterClick").get_array())
        expected_changes.emplace_back(change.at("id").get_string(), change.at("value").get_string());
    CHECK(changes == expected_changes);

    // Typing after the press edits the search input at its clicked cursor
    // instead of replacing the query.
    type_key(list, inputs.at("edited").get_string());
    CHECK(list.search_query() == press.at("queryAfterType").get_string());
}

TEST_CASE("a delegating host keeps the focus when its nested input is clicked",
        "[tui][mouse-components][issue1003][compat-pi]") {
    // Criterion 3: the focus inheritance rule of `dispatch_mouse_event`
    // (`Mouse.cpp:13-31`, pi `tui.ts:81-105`). The frozen end-to-end case
    // (`mouse-components.test.ts:195-215`) shows the same property through the
    // alternate screen's overlay dispatch.
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto host = std::make_unique<DelegatingHost>(std::make_unique<cch::tui::Input>(prompt_options()));
    auto* host_pointer = host.get();
    auto* input_pointer = dynamic_cast<cch::tui::Input*>(&host_pointer->child());
    REQUIRE(input_pointer != nullptr);
    input_pointer->set_value("hi");
    REQUIRE(tui.add_child(std::move(host)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(host_pointer));
    REQUIRE(tui.render());

    const MouseEvent press{.type = MouseEventType::Press,
            .button = MouseButton::Left,
            .x = 4,
            .y = 0,
            .screen_x = 4,
            .screen_y = 0,
            .width = 20,
            .height = 1};
    const auto answer = cch::tui::dispatch_mouse_event(*host_pointer, press);
    REQUIRE(answer);
    CHECK(answer->focus);
    REQUIRE(answer->focus_target == host_pointer);
    REQUIRE(tui.focus() == host_pointer);

    type_key(*input_pointer, "!");
    CHECK(input_pointer->value() == "hi!");
    REQUIRE(tui.stop());
}

TEST_CASE("an alternate-screen click in an overlay keeps its delegating host focused",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alternate-screen-nested-input-click-keeps-host-focus");
    const auto& inputs = scenario.at("inputs");
    const auto& overlays = scenario.at("expected").at("overlays").get_array();

    for (const auto& expected : overlays) {
        const auto column = size_of(expected.at("column"));
        INFO("overlay click column " << column);
        cch::tui::VirtualTerminal terminal({.columns = size_of(scenario.at("dimensions").at("columns")),
                .rows = size_of(scenario.at("dimensions").at("rows"))});
        cch::tui::AltScreen tui(terminal);
        auto host = std::make_unique<DelegatingHost>(
                std::make_unique<cch::tui::Input>(cch::tui::InputOptions{.prompt = "> "}));
        auto* host_pointer = host.get();
        auto* input_pointer = dynamic_cast<cch::tui::Input*>(&host_pointer->child());
        REQUIRE(input_pointer != nullptr);
        input_pointer->set_value(inputs.at("value").get_string());
        auto overlay = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
                .width = cch::tui::SizeValue::absolute(static_cast<double>(size_of(inputs.at("overlayWidth")))),
                .anchor = cch::tui::OverlayAnchor::TopLeft});
        REQUIRE(overlay->add_child(std::move(host)));
        REQUIRE(overlay->focus_first());
        auto* overlay_pointer = overlay.get();
        auto shown = tui.show_overlay(std::move(overlay));
        REQUIRE(shown);
        REQUIRE(tui.start());
        REQUIRE(tui.render());

        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(column) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(column) + ";1m"));
        CHECK(tui.focus() == overlay_pointer);

        type_key(*input_pointer, inputs.at("typed").get_string());
        CHECK(input_pointer->value() == expected.at("value").get_string());

        REQUIRE(tui.render());
        check_screen(drawn_rows(terminal), expected.at("screen"), "overlay screen");
        CHECK(expected.at("hostFocusedAfterClick").get_boolean());
        REQUIRE(tui.stop());
    }
}

TEST_CASE("a settings submenu click closes it and keys still reach the visible list",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "settings-submenu-click-closes-and-keeps-host-focus");
    const auto& frozen = scenario.at("expected").at("submenu");

    cch::tui::VirtualTerminal terminal({.columns = size_of(scenario.at("dimensions").at("columns")),
            .rows = size_of(scenario.at("dimensions").at("rows"))});
    cch::tui::AltScreen tui(terminal);
    std::vector<std::pair<std::string, std::string>> changes;
    auto options = cch::tui::SettingsListOptions{.max_visible = 5, .theme = frozen_settings_theme()};
    options.on_change = [&changes](std::string id, std::string value) -> cch::support::ExpectedVoid {
        changes.emplace_back(std::move(id), std::move(value));
        return {};
    };
    options.submenu_factory = [](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink done) {
        return std::make_unique<FrozenSubmenuHost>(std::move(done));
    };
    std::vector<cch::tui::SettingItem> items{
            {.id = "theme", .label = "Theme", .current_value = "first", .control = cch::tui::SettingSubmenu{}},
            {.id = "other",
                    .label = "Other",
                    .current_value = "off",
                    .control = cch::tui::SettingValues{.values = {"off", "on"}}}};
    auto list = std::make_unique<cch::tui::SettingsList>(std::move(items), std::move(options));
    auto* list_pointer = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.set_focus(list_pointer));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array().front(), "closed screen");

    const auto& keys = frozen.at("keys").get_array();
    REQUIRE(keys.size() >= 1);
    REQUIRE(terminal.inject_input(keys[0].get_string()));
    REQUIRE(tui.render());
    CHECK(list_pointer->submenu_open() == frozen.at("submenuOpen").get_boolean());
    CHECK(list_pointer->submenu_open());
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array()[1], "submenu screen");

    const auto& click = frozen.at("click");
    REQUIRE(terminal.inject_input(click.at("press").get_string()));
    REQUIRE(terminal.inject_input(click.at("release").get_string()));
    REQUIRE(tui.render());

    std::vector<std::pair<std::string, std::string>> expected_after_click;
    for (const auto& change : frozen.at("changesAfterClick").get_array())
        expected_after_click.emplace_back(change.at("id").get_string(), change.at("value").get_string());
    CHECK(changes == expected_after_click);
    CHECK_FALSE(list_pointer->submenu_open());
    CHECK(frozen.at("closedAfterClick").get_boolean());
    CHECK(tui.focus() == list_pointer);
    CHECK(frozen.at("focusedAfterClick").get_boolean());
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array()[2], "clicked screen");

    REQUIRE(keys.size() >= 3);
    for (std::size_t index = 1; index < keys.size(); ++index)
        REQUIRE(terminal.inject_input(keys[index].get_string()));
    REQUIRE(tui.render());
    std::vector<std::pair<std::string, std::string>> expected_after_keys;
    for (const auto& change : frozen.at("changesAfterKeys").get_array())
        expected_after_keys.emplace_back(change.at("id").get_string(), change.at("value").get_string());
    CHECK(changes == expected_after_keys);
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array()[3], "keys screen");
    REQUIRE(tui.stop());
}

TEST_CASE("an alternate-screen click positions and focuses the editor cursor",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "editor-alternate-screen-click-positions-cursor");
    const auto& frozen = scenario.at("expected").at("editor");

    cch::tui::VirtualTerminal terminal({.columns = size_of(frozen.at("columns")), .rows = size_of(frozen.at("rows"))});
    cch::tui::AltScreen tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    use_frozen_border(*editor_pointer);
    editor_pointer->set_text(frozen.at("text").get_string());
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(frozen.at("screens").get_array().size() == 2);
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array().front(), "closed screen");

    const auto& click = frozen.at("click");
    REQUIRE(terminal.inject_input(click.at("press").get_string()));
    REQUIRE(terminal.inject_input(click.at("release").get_string()));
    REQUIRE(tui.render());
    CHECK(editor_pointer->cursor().line == size_of(frozen.at("cursorAfterClick").at("line")));
    CHECK(editor_pointer->cursor().column == size_of(frozen.at("cursorAfterClick").at("col")));
    CHECK(tui.focus() == editor_pointer);
    CHECK(frozen.at("focused").get_boolean());

    type_key(*editor_pointer, scenario.at("inputs").at("typed").get_string());
    CHECK(editor_pointer->text() == frozen.at("textAfterType").get_string());
    REQUIRE(tui.render());
    check_screen(drawn_rows(terminal), frozen.at("screens").get_array()[1], "submenu screen");
    REQUIRE(tui.stop());
}

TEST_CASE("an editor click respects the borders, the wrapped segments and the wide graphemes",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "editor-click-bounds-wrapped-segments-and-wide-graphemes");
    const auto& frozen = scenario.at("expected");
    const auto typed = scenario.at("inputs").at("typed").get_string();

    for (const auto& expected : frozen.at("cases").get_array()) {
        const auto label = expected.at("label").get_string();
        INFO(label);
        cch::tui::Editor editor;
        use_frozen_border(editor);
        editor.set_padding_x(size_of(expected.at("paddingX")));
        editor.set_text(expected.at("text").get_string());
        const auto width = size_of(expected.at("width"));
        const auto rendered = editor.render(width);
        REQUIRE(rendered);
        CHECK(drawn_rows(*rendered) == plain_lines(expected.at("rows")));
        CHECK(editor.cursor().line == size_of(expected.at("cursorBefore").at("line")));
        CHECK(editor.cursor().column == size_of(expected.at("cursorBefore").at("col")));

        const auto answer = editor.handle_mouse(MouseEvent{.type = MouseEventType::Click,
                .button = MouseButton::Left,
                .x = int_of(expected.at("x")),
                .y = int_of(expected.at("y")),
                .screen_x = int_of(expected.at("x")),
                .screen_y = int_of(expected.at("y")),
                .width = width,
                .height = rendered->lines.size(),
                .click_count = 1});
        check_answer(answer, expected.at("result"), label);
        CHECK(editor.cursor().line == size_of(expected.at("cursorAfterClick").at("line")));
        CHECK(editor.cursor().column == size_of(expected.at("cursorAfterClick").at("col")));

        type_key(editor, typed);
        CHECK(editor.text() == expected.at("textAfterType").get_string());
    }
}

TEST_CASE("an editor drag selects and copies instead of moving the cursor",
        "[tui][mouse-components][issue1003][compat-pi]") {
    const auto bundle = mouse_components_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "editor-drag-selects-and-copies-instead-of-moving-the-cursor");
    const auto& frozen = scenario.at("expected").at("drag");

    cch::tui::VirtualTerminal terminal({.columns = size_of(scenario.at("dimensions").at("columns")),
            .rows = size_of(scenario.at("dimensions").at("rows"))});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    use_frozen_border(*editor_pointer);
    editor_pointer->set_text(frozen.at("text").get_string());
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(editor_pointer->cursor().line == size_of(frozen.at("cursorBefore").at("line")));
    CHECK(editor_pointer->cursor().column == size_of(frozen.at("cursorBefore").at("col")));

    REQUIRE(terminal.inject_input(frozen.at("press").get_string()));
    REQUIRE(terminal.inject_input(frozen.at("drag").get_string()));
    REQUIRE(terminal.inject_input(frozen.at("release").get_string()));
    REQUIRE(tui.render());

    CHECK(copied == string_lines(frozen.at("copied")));
    CHECK(editor_pointer->cursor().line == size_of(frozen.at("cursorAfter").at("line")));
    CHECK(editor_pointer->cursor().column == size_of(frozen.at("cursorAfter").at("col")));
    CHECK(editor_pointer->text() == frozen.at("textAfterDrag").get_string());
    const auto screen = drawn_rows(terminal);
    REQUIRE(screen.size() > 1);
    REQUIRE(frozen.at("screen").get_array().size() > 1);
    CHECK(screen[1] == drawn_row(frozen.at("screen").get_array()[1].get_string()));
    REQUIRE(tui.stop());
}
