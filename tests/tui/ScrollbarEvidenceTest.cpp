// Named pi-v1.0.4 acceptance for scrollbar presentation (#1001). Every
// expectation is read from the verified named bundle through
// `scripts/tui/evidence.py`; nothing here regenerates expectations from Pike or
// falls back to the historical `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"
#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

/// A deterministic stand-in for pi's `setTimeout`: nothing fires until the
/// replay asks, so a frozen phase that awaited the hide delay is reproduced
/// exactly.
class ManualScrollbarHideTimer final : public cch::tui::ScrollbarHideTimer {
public:
    void start(std::chrono::milliseconds delay, std::function<void()> on_fire) override {
        last_delay = delay;
        pending_ = std::move(on_fire);
    }

    void cancel() override { pending_ = nullptr; }

    [[nodiscard]] bool armed() const { return pending_ != nullptr; }

    void fire() {
        auto callback = std::move(pending_);
        pending_ = nullptr;
        REQUIRE(callback);
        callback();
    }

    std::chrono::milliseconds last_delay{};

private:
    std::function<void()> pending_{};
};

/// A leaf that renders the frozen overlay content: a background-painted row
/// whose scrollbar column carries a border foreground.
class BackgroundBorderChild final : public cch::tui::Component {
public:
    explicit BackgroundBorderChild(std::size_t rows) : rows_(rows) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        std::vector<std::string> lines;
        lines.reserve(rows_);
        for (std::size_t row = 0; row < rows_; ++row)
            lines.push_back("\x1b[42m" + std::string(width - 1, 'x') + "\x1b[31m\u2502\x1b[39m\x1b[49m");
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }

    void invalidate() override {}

private:
    std::size_t rows_;
};

/// The frame's painted rows in the frozen `visibleLines` convention (strip the
/// terminal sequences and the trailing padding, pi `test/layout.test.ts:10-12`).
[[nodiscard]] std::vector<std::string> visible_rows(const cch::tui::detail::LayoutFrame& frame) {
    std::vector<std::string> rows;
    rows.reserve(frame.lines.size());
    for (const auto& row : frame.lines) {
        auto visible = cch::tui::strip_terminal_sequences(row);
        while (!visible.empty() && visible.back() == ' ')
            visible.pop_back();
        rows.push_back(std::move(visible));
    }
    return rows;
}

[[nodiscard]] std::vector<std::string> screen_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

[[nodiscard]] std::ptrdiff_t int_of(const JsonValue& value) {
    return static_cast<std::ptrdiff_t>(std::llround(value.get_number()));
}

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::vector<bool> booleans(const JsonValue& value) {
    std::vector<bool> flags;
    for (const auto& flag : value.get_array())
        flags.push_back(flag.get_boolean());
    return flags;
}

/// A frozen phase's row flags recomputed from a painted row, so the artifact's
/// styles are the only thing the replay trusts.
[[nodiscard]] std::vector<bool> row_flags(
        const cch::tui::detail::LayoutFrame& frame, const std::function<bool(std::string_view)>& probe) {
    std::vector<bool> flags;
    flags.reserve(frame.lines.size());
    for (const auto& line : frame.lines)
        flags.push_back(probe(line));
    return flags;
}

[[nodiscard]] cch::support::Expected<JsonValue> scrollbar_bundle() {
    return cch::tests::read_pi_tui_evidence("scrollbar.json");
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::string joined_lines(const JsonValue& lines) {
    std::string text;
    for (const auto& line : lines.get_array()) {
        if (!text.empty()) text += '\n';
        text += line.get_string();
    }
    return text;
}

/// The `count`-row numbered transcript of the frozen observation (pi's
/// `Text(numbered, 0, 0)`).
[[nodiscard]] std::string numbered_text(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += "line " + std::to_string(index);
    }
    return text;
}

[[nodiscard]] std::string repeated_rows(std::size_t count, std::string_view row) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        if (index > 0) text += '\n';
        text += row;
    }
    return text;
}

/// The frozen window of a frame: the viewport rect and the child rect it
/// allocated.
void check_window(const cch::tui::detail::LayoutFrame& frame, const JsonValue& window) {
    CHECK(frame.root.rect.width == static_cast<std::size_t>(int_of(window.at("width"))));
    CHECK(frame.root.rect.height == static_cast<std::size_t>(int_of(window.at("height"))));
    CHECK(frame.root.children.front().rect.width == static_cast<std::size_t>(int_of(window.at("childWidth"))));
    CHECK(frame.root.children.front().rect.height == static_cast<std::size_t>(int_of(window.at("childHeight"))));
}

[[nodiscard]] cch::tui::ScrollbarMode mode_of(const JsonValue& value) {
    const auto name = value.get_string();
    if (name == "hidden") return cch::tui::ScrollbarMode::Hidden;
    if (name == "auto") return cch::tui::ScrollbarMode::Auto;
    if (name == "always") return cch::tui::ScrollbarMode::Always;
    REQUIRE(false);
    return cch::tui::ScrollbarMode::Hidden;
}

/// A frozen phase covering the viewport's public state and the frame it paints.
struct ViewportPhase {
    std::vector<std::string> lines;
    std::size_t viewport_width{0};
    std::size_t viewport_height{0};
    std::size_t child_width{0};
    std::size_t child_height{0};
    cch::tui::ScrollbarMode mode{cch::tui::ScrollbarMode::Hidden};
    bool visible{false};
    bool active{false};
    std::size_t scroll_top{0};
};

[[nodiscard]] ViewportPhase phase_of(const cch::tui::ScrollView& view, const cch::tui::detail::LayoutFrame& frame) {
    const auto& box = frame.root;
    const auto& child = box.children.front();
    return ViewportPhase{.lines = visible_rows(frame),
            .viewport_width = box.rect.width,
            .viewport_height = view.viewport_height(),
            .child_width = child.rect.width,
            .child_height = child.rect.height,
            .mode = view.scrollbar(),
            .visible = view.is_scrollbar_visible(),
            .active = view.is_scrollbar_active(),
            .scroll_top = view.scroll_top()};
}

[[nodiscard]] ViewportPhase phase_of(const JsonValue& frozen) {
    const auto& window = frozen.at("window");
    return ViewportPhase{.lines = string_lines(frozen.at("lines")),
            .viewport_width = static_cast<std::size_t>(int_of(window.at("width"))),
            .viewport_height = static_cast<std::size_t>(int_of(frozen.at("viewportHeight"))),
            .child_width = static_cast<std::size_t>(int_of(window.at("childWidth"))),
            .child_height = static_cast<std::size_t>(int_of(window.at("childHeight"))),
            .mode = mode_of(frozen.at("scrollbar")),
            .visible = frozen.at("isScrollbarVisible").get_boolean(),
            .active = frozen.at("isScrollbarActive").get_boolean(),
            .scroll_top = static_cast<std::size_t>(int_of(frozen.at("scrollTop")))};
}

void check_phase(const ViewportPhase& observed, const ViewportPhase& frozen) {
    CHECK(observed.lines == frozen.lines);
    CHECK(observed.viewport_width == frozen.viewport_width);
    CHECK(observed.viewport_height == frozen.viewport_height);
    CHECK(observed.child_width == frozen.child_width);
    CHECK(observed.child_height == frozen.child_height);
    CHECK(observed.mode == frozen.mode);
    CHECK(observed.visible == frozen.visible);
    CHECK(observed.active == frozen.active);
    CHECK(observed.scroll_top == frozen.scroll_top);
}

} // namespace

TEST_CASE("scrollbar modes reserve the frozen content column and re-wrap the child",
        "[tui][scroll-view][scrollbar][issue1001][compat-pi]") {
    const auto bundle = scrollbar_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scrollbar-modes-and-reservation");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto& dimensions = scenario.at("dimensions");
    const auto width = static_cast<std::size_t>(int_of(dimensions.at("columns")));
    const auto height = static_cast<std::size_t>(int_of(dimensions.at("rows")));

    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>(inputs.at("content").get_string(), 0, 0));
    CHECK(view.scrollbar() == cch::tui::ScrollbarMode::Hidden);
    CHECK(view.content_width(width) == static_cast<std::size_t>(int_of(frozen.at("hidden").at("contentWidth"))));
    CHECK(view.content_width(1) == static_cast<std::size_t>(int_of(frozen.at("hidden").at("contentWidthAtOne"))));

    auto frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("hidden")));

    // A real caller changes the option at runtime and the next frame reserves
    // the column before the child wraps.
    view.set_scrollbar(cch::tui::ScrollbarMode::Always);
    CHECK(view.content_width(width) == static_cast<std::size_t>(int_of(frozen.at("always").at("contentWidth"))));
    CHECK(view.content_width(1) == static_cast<std::size_t>(int_of(frozen.at("always").at("contentWidthAtOne"))));
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("always")));

    view.set_scrollbar(cch::tui::ScrollbarMode::Hidden);
    CHECK(view.content_width(width) == static_cast<std::size_t>(int_of(frozen.at("restored").at("contentWidth"))));
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("restored")));
}

TEST_CASE("the transient scrollbar follows the frozen timeline and overlay splice",
        "[tui][scroll-view][scrollbar][issue1001][compat-pi]") {
    const auto bundle = scrollbar_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scrollbar-transient-timeline");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto& styles = inputs.at("styles");
    const auto track_color = styles.at("trackColor").get_string();
    const auto thumb_color = styles.at("thumbColor").get_string();
    const auto content_background = styles.at("contentBackground").get_string();
    const auto width = static_cast<std::size_t>(int_of(scenario.at("dimensions").at("columns")));
    const auto height = static_cast<std::size_t>(int_of(scenario.at("dimensions").at("rows")));
    const auto content = joined_lines(inputs.at("contentLines"));

    const auto track_style = [track_color](
                                     std::string_view text) { return track_color + std::string(text) + "\x1b[39m"; };
    const auto thumb_style = [thumb_color](
                                     std::string_view text) { return thumb_color + std::string(text) + "\x1b[39m"; };
    const auto background_hook = [content_background](
                                         std::string text) { return content_background + text + "\x1b[49m"; };

    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>(content, 0, 0, background_hook),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_track_style = track_style,
                    .scrollbar_thumb_style = thumb_style,
                    .scrollbar_hide_delay =
                            std::chrono::milliseconds{int_of(inputs.at("options").at("scrollbarHideDelayMs"))},
                    .scrollbar_hide_timer = timer});

    auto frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("first")));

    const auto styled_phase = [&](const JsonValue& expected) {
        check_phase(phase_of(view, *frame), phase_of(expected));
        CHECK(row_flags(*frame, [&](std::string_view line) {
            return line.find(track_color) != std::string_view::npos;
        }) == booleans(expected.at("track")));
        CHECK(row_flags(*frame, [&](std::string_view line) {
            return line.find(thumb_color) != std::string_view::npos;
        }) == booleans(expected.at("thumb")));
        CHECK(row_flags(*frame, [&](std::string_view line) {
            return line.find("\x1b[0m\x1b]8;;\x07" + content_background) != std::string_view::npos;
        }) == booleans(expected.at("preservedBackgroundReemit")));
    };

    for (const auto& request : inputs.at("requests").get_array()) {
        INFO("scroll request " << int_of(request));
        CHECK(view.scroll_by(int_of(request)) == 0);
    }
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    styled_phase(frozen.at("scrolled"));

    view.set_scrollbar_active(true);
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    styled_phase(frozen.at("hovered"));

    // The frozen phase awaited the hide delay; the replay fires the timer the
    // viewport armed instead.
    view.set_scrollbar_active(false);
    CHECK(timer->last_delay == std::chrono::milliseconds{int_of(inputs.at("options").at("scrollbarHideDelayMs"))});
    REQUIRE(timer->armed());
    timer->fire();
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("expired")));

    view.scroll_to_end();
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("atEnd")));

    view.scroll_to_start();
    frame = cch::tui::detail::LayoutEngine::render(view, width, height);
    REQUIRE(frame);
    check_phase(phase_of(view, *frame), phase_of(frozen.at("atStart")));

    // Followed content that grows past the viewport keeps following; fitting
    // content never reveals a bar.
    auto followed_text = std::make_unique<cch::tui::Text>(content, 0, 0);
    auto* followed_pointer = followed_text.get();
    cch::tui::ScrollView followed(std::move(followed_text),
            cch::tui::ScrollViewOptions{.follow_end = true,
                    .scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_track_style = track_style,
                    .scrollbar_thumb_style = thumb_style});
    frame = cch::tui::detail::LayoutEngine::render(followed, width, height);
    REQUIRE(frame);
    check_phase(phase_of(followed, *frame), phase_of(frozen.at("followedBefore")));

    followed_pointer->set_text(content + "\nabcde9");
    frame = cch::tui::detail::LayoutEngine::render(followed, width, height);
    REQUIRE(frame);
    check_phase(phase_of(followed, *frame), phase_of(frozen.at("followedAfter")));

    cch::tui::ScrollView fitting(std::make_unique<cch::tui::Text>("1\n2", 0, 0),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_thumb_style = thumb_style});
    frame = cch::tui::detail::LayoutEngine::render(fitting, width, height);
    REQUIRE(frame);
    check_phase(phase_of(fitting, *frame), phase_of(frozen.at("fittingBefore")));

    // Fitting content cannot consume the delta, so the request stays unused.
    CHECK(fitting.scroll_by(1) == 1);
    frame = cch::tui::detail::LayoutEngine::render(fitting, width, height);
    REQUIRE(frame);
    check_phase(phase_of(fitting, *frame), phase_of(frozen.at("fittingAfter")));

    cch::tui::ScrollView always_fitting(std::make_unique<cch::tui::Text>("1\n2", 0, 0),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Always, .scrollbar_thumb_style = thumb_style});
    frame = cch::tui::detail::LayoutEngine::render(always_fitting, width, height);
    REQUIRE(frame);
    check_phase(phase_of(always_fitting, *frame), phase_of(frozen.at("alwaysFitting")));

    // pi `test/layout.test.ts:269-286`: the `alwaysFitting` bar remains visible
    // despite fitting content, and `alwaysOverflowing` reserves and paints it.
    cch::tui::ScrollView always_overflowing(std::make_unique<cch::tui::Text>(content, 0, 0, background_hook),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always,
                    .scrollbar_track_style = track_style,
                    .scrollbar_thumb_style = thumb_style});
    frame = cch::tui::detail::LayoutEngine::render(always_overflowing, width, height);
    REQUIRE(frame);
    check_phase(phase_of(always_overflowing, *frame), phase_of(frozen.at("alwaysOverflowing")));
    CHECK(row_flags(*frame, [&](std::string_view line) {
        const auto style_index = std::max(line.rfind(track_color), line.rfind(thumb_color));
        const auto prefix_index = line.rfind("\x1b[0m\x1b]8;;\x07", style_index);
        return prefix_index != std::string_view::npos && prefix_index > line.rfind(content_background);
    }) == booleans(frozen.at("alwaysOverflowingReset")));
}

TEST_CASE("scrollbar thumb proportions, empty, tiny and single-column viewports match the frozen frames",
        "[tui][scroll-view][scrollbar][issue1001][compat-pi]") {
    const auto bundle = scrollbar_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scrollbar-geometry-edges");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto identity_thumb = [](std::string_view text) { return std::string(text); };

    for (const auto& frozen_thumb : frozen.at("thumbs").get_array()) {
        const auto content_height = static_cast<std::size_t>(int_of(frozen_thumb.at("contentHeight")));
        std::string content;
        for (std::size_t row = 0; row < content_height; ++row) {
            if (row > 0) content += '\n';
            content += "x";
        }
        cch::tui::ScrollView view(std::make_unique<cch::tui::Text>(content, 0, 0),
                cch::tui::ScrollViewOptions{
                        .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_thumb_style = identity_thumb});
        auto frame = cch::tui::detail::LayoutEngine::render(view, 6, 20);
        REQUIRE(frame);
        CHECK(view.scroll_by(1) == 0);
        frame = cch::tui::detail::LayoutEngine::render(view, 6, 20);
        REQUIRE(frame);
        std::size_t thumb_rows = 0;
        std::size_t track_rows = 0;
        for (const auto& row : visible_rows(*frame)) {
            if (row.ends_with("\u2503")) ++thumb_rows;
            if (row.ends_with("\u2502")) ++track_rows;
        }
        INFO("content height " << content_height);
        CHECK(thumb_rows == static_cast<std::size_t>(int_of(frozen_thumb.at("thumbRows"))));
        CHECK(track_rows == static_cast<std::size_t>(int_of(frozen_thumb.at("trackRows"))));
    }

    // Empty content takes the frozen `Infinity` branch: the thumb is the track.
    class EmptyChild final : public cch::tui::Component {
    public:
        [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
            return cch::tui::RenderResult{};
        }

        void invalidate() override {}
    };

    cch::tui::ScrollView empty(
            std::make_unique<EmptyChild>(), cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto frame = cch::tui::detail::LayoutEngine::render(empty, 4, 3);
    REQUIRE(frame);
    CHECK(visible_rows(*frame) == string_lines(frozen.at("empty").at("lines")));
    check_window(*frame, frozen.at("empty").at("window"));

    // A one-row track holds a one-row thumb, and a text child wraps into the
    // narrow column the reservation leaves.
    cch::tui::ScrollView tiny(
            std::make_unique<cch::tui::Text>(
                    repeated_rows(static_cast<std::size_t>(int_of(inputs.at("tinyContentLines"))), "abcde"), 0, 0),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    frame = cch::tui::detail::LayoutEngine::render(tiny, 3, 1);
    REQUIRE(frame);
    CHECK(visible_rows(*frame) == string_lines(frozen.at("tiny").at("lines")));
    check_window(*frame, frozen.at("tiny").at("window"));

    // A single-column viewport keeps one content column: `getContentWidth` has
    // no room to reserve.
    cch::tui::ScrollView single_column(std::make_unique<cch::tui::Text>("abcde", 0, 0),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    frame = cch::tui::detail::LayoutEngine::render(single_column, 1, 2);
    REQUIRE(frame);
    CHECK(single_column.content_width(1) ==
            static_cast<std::size_t>(int_of(frozen.at("singleColumn").at("contentWidth"))));
    CHECK(visible_rows(*frame) == string_lines(frozen.at("singleColumn").at("lines")));
    check_window(*frame, frozen.at("singleColumn").at("window"));
}

TEST_CASE("the overlay scrollbar keeps the frozen background splice",
        "[tui][scroll-view][scrollbar][issue1001][compat-pi]") {
    const auto bundle = scrollbar_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scrollbar-overlay-preserves-background");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto background = inputs.at("contentBackground").get_string();
    const auto border_foreground = inputs.at("borderForeground").get_string();
    const auto identity = [](std::string_view text) { return std::string(text); };

    cch::tui::ScrollView view(
            std::make_unique<BackgroundBorderChild>(static_cast<std::size_t>(int_of(inputs.at("contentLines")))),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_track_style = identity,
                    .scrollbar_thumb_style = identity});
    auto frame = cch::tui::detail::LayoutEngine::render(view, 6, 4);
    REQUIRE(frame);
    CHECK(view.scroll_by(int_of(inputs.at("request"))) == 0);
    frame = cch::tui::detail::LayoutEngine::render(view, 6, 4);
    REQUIRE(frame);

    CHECK(visible_rows(*frame) == string_lines(frozen.at("lines")));
    CHECK(row_flags(*frame, [&](std::string_view line) { return line.find(background) != std::string_view::npos; }) ==
            booleans(frozen.at("keepsBackground")));
    CHECK(row_flags(*frame, [&](std::string_view line) {
        return line.find(border_foreground) == std::string_view::npos;
    }) == booleans(frozen.at("dropsBorderForeground")));
    CHECK(row_flags(*frame, [&](std::string_view line) {
        return line.find("\x1b[0m\x1b]8;;\x07" + background) != std::string_view::npos;
    }) == booleans(frozen.at("preservedBackgroundReemit")));
}

TEST_CASE("the frozen hover phases drive the alternate-screen scrollbar",
        "[tui][scroll-view][scrollbar][issue1001][compat-pi]") {
    const auto bundle = scrollbar_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scrollbar-alt-screen-hover");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto& dimensions = scenario.at("dimensions");
    const auto columns = static_cast<std::size_t>(int_of(dimensions.at("columns")));
    const auto rows = static_cast<std::size_t>(int_of(dimensions.at("rows")));
    const auto& options = inputs.at("options");

    cch::tui::VirtualTerminal terminal({.columns = columns, .rows = rows});
    cch::tui::AltScreen tui(terminal);
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    auto view = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::Text>(
                    numbered_text(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = options.at("follow").get_string() == "end",
                    .primary = options.at("primary").get_boolean(),
                    .scrollbar = mode_of(options.at("scrollbar")),
                    .scrollbar_hide_delay = std::chrono::milliseconds{int_of(options.at("scrollbarHideDelayMs"))},
                    .scrollbar_hide_timer = timer});
    auto* view_pointer = view.get();
    tui.set_layout_root(std::move(view));
    REQUIRE(tui.start());

    const auto reports = inputs.at("reports").get_array();
    std::size_t phase_index = 0;
    for (const auto& phase : frozen.at("phases").get_array()) {
        const auto action = phase.at("action").get_string();
        INFO("phase " << action);
        if (phase_index > 0 && phase_index - 1 < reports.size() && action != "stopped") {
            REQUIRE(terminal.inject_input(reports[phase_index - 1].at("data").get_string()));
            if (timer->armed()) timer->fire();
        }
        // `stop()` is observed before the pending hide runs, exactly as the
        // frozen capture observes it. Leaving the alternate screen clears this
        // terminal, so the stopped phase is compared through the viewport state
        // alone.
        if (action == "stopped") {
            REQUIRE(tui.stop());
        } else {
            REQUIRE(tui.render());
            CHECK(screen_rows(terminal) == string_lines(phase.at("viewport")));
        }

        CHECK(view_pointer->is_scrollbar_visible() == phase.at("isScrollbarVisible").get_boolean());
        CHECK(view_pointer->is_scrollbar_active() == phase.at("isScrollbarActive").get_boolean());
        CHECK(view_pointer->scroll_top() == static_cast<std::size_t>(int_of(phase.at("scrollTop"))));
        CHECK(view_pointer->viewport_height() == static_cast<std::size_t>(int_of(phase.at("viewportHeight"))));
        ++phase_index;
    }
    CHECK(phase_index == frozen.at("phases").get_array().size());
}
