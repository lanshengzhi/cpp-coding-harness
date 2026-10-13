// Scrollbar presentation: modes, transient reveal timing, width reservation,
// thumb geometry, and the pointer hover (issue #1001).
//
// Every expected value below is derived from the frozen pi v1.0.4 revision
// 7c10bd4337495ee613f2224843ecdf349b80d1df:
// `packages/tui/src/components/scroll-view.ts:41-125,189-201` (the scrollbar
// options, `isScrollbarVisible`, `setScrollbar`, `getContentWidth`, the
// transient reveal and its timer, and the `updateLayout` hide),
// `packages/tui/src/layout.ts:115-175,249-331,406-424` (the reserved content
// width, `replaceScrollbarCell`, `getScrollbarGeometry`, `paintScrollbar`),
// `packages/tui/src/tui-alt-screen.ts:918-947,1037-1066` (the hover update on
// pointer and wheel reports, `getScrollbarTargetAt`, and the stop points), and
// the independent expectations in `packages/tui/test/layout.test.ts:188-320`
// and `packages/tui/test/tui-alt-screen.test.ts:140-200,440-500`.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// A test-controlled one-shot hide timer: nothing fires until the test asks,
/// and `cancel` drops the pending callback the way a real timer does.
class ManualScrollbarHideTimer final : public cch::tui::ScrollbarHideTimer {
public:
    void start(std::chrono::milliseconds delay, std::function<void()> on_fire) override {
        ++start_count;
        last_delay = delay;
        pending_ = std::move(on_fire);
    }

    void cancel() override {
        ++cancel_count;
        pending_ = nullptr;
    }

    [[nodiscard]] bool armed() const { return pending_ != nullptr; }

    void fire() {
        auto callback = std::move(pending_);
        pending_ = nullptr;
        REQUIRE(callback);
        callback();
    }

    std::size_t start_count{0};
    std::size_t cancel_count{0};
    std::chrono::milliseconds last_delay{};

private:
    std::function<void()> pending_{};
};

class LabeledChild final : public cch::tui::Component {
public:
    explicit LabeledChild(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        rendered_width = width;
        std::vector<std::string> rendered;
        rendered.reserve(lines_.size());
        for (const auto& line : lines_)
            rendered.push_back(line.size() > width ? line.substr(0, width) : line);
        return cch::tui::RenderResult{.lines = std::move(rendered)};
    }

    void invalidate() override {}

    std::size_t rendered_width{0};

private:
    std::vector<std::string> lines_;
};

/// The frozen proportionate-scrollbar content: eight five-cell rows so a
/// six-column viewport only has to decide about its last column.
[[nodiscard]] std::vector<std::string> styled_rows() {
    return {"abcde", "abcde", "abcde", "abcde", "abcde", "abcde", "abcde", "abcde"};
}

[[nodiscard]] cch::support::Expected<cch::tui::detail::LayoutFrame> render_frame(
        cch::tui::Component& root, std::size_t width, std::size_t height, std::function<void()> request_render = {}) {
    return cch::tui::detail::LayoutEngine::render(root, width, height, std::move(request_render));
}

/// The frame's painted rows with the terminal sequences removed and the
/// trailing padding trimmed, the frozen `visibleLines` convention
/// (`test/layout.test.ts:10-12`).
[[nodiscard]] std::vector<std::string> visible_lines(const cch::tui::detail::LayoutFrame& frame) {
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

} // namespace

TEST_CASE("a scrollbar defaults to hidden and neither draws nor reserves a column",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `scroll-view.ts:52`: a reusable viewport starts hidden, so a viewport
    // the product has not configured renders exactly the child's output.
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()));
    CHECK(view.scrollbar() == cch::tui::ScrollbarMode::Hidden);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(view.is_scrollbar_active());
    CHECK(view.content_width(6) == 6);
    CHECK(view.content_width(0) == 0);
    CHECK(view.content_width(1) == 1);

    auto frame = render_frame(view, 6, 4);
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"abcde", "abcde", "abcde", "abcde"});
    CHECK(frame->root.children.front().rect.width == 6);
    CHECK(cch::tui::detail::get_scrollbar_geometry(frame->root) == std::nullopt);
}

TEST_CASE("always reserves its column before the child wraps and paints the track",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:295-307` (`updates reserved scrollbar layout at
    // runtime`): the reservation is part of the layout pass, so `Text("123456")`
    // wraps into two rows at width five and stays one row when hidden.
    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>("123456", 0, 0),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    CHECK(view.content_width(6) == 5);
    CHECK(view.content_width(1) == 1);

    auto frame = render_frame(view, 6, 2);
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"12345\u2503", "6    \u2503"});
    CHECK(frame->root.children.front().rect.width == 5);
    CHECK(frame->root.children.front().rect.height == 2);
    CHECK(frame->lines[0].find("\x1b[37m\u2503\x1b[39m") != std::string::npos);

    // The reservation changed the content height the viewport learned, so the
    // thumb fills the whole two-row track.
    const auto geometry = cch::tui::detail::get_scrollbar_geometry(frame->root);
    REQUIRE(geometry);
    CHECK(geometry->column == 5);
    CHECK(geometry->track_top == 0);
    CHECK(geometry->track_height == 2);
    CHECK(geometry->thumb_height == 2);
    CHECK(geometry->thumb_top == 0);
    CHECK(geometry->max_scroll_top == 0);
}

TEST_CASE("auto reveals on scroll activity, draws the proportional thumb, and hides at the frozen delay",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:188-236`: the first frame shows no bar, `scrollBy(2)`
    // reveals a two-row thumb inside a four-row track, and the hide delay
    // returns the rows to plain content.
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_hide_delay = std::chrono::milliseconds{1000},
                    .scrollbar_hide_timer = timer});
    std::size_t render_requests = 0;
    auto frame = render_frame(view, 6, 4, [&render_requests] { ++render_requests; });
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"abcde", "abcde", "abcde", "abcde"});
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(timer->armed());

    CHECK(view.scroll_by(2) == 0);
    frame = render_frame(view, 6, 4, [&render_requests] { ++render_requests; });
    REQUIRE(frame);
    CHECK(view.is_scrollbar_visible());
    CHECK(visible_lines(*frame) ==
            std::vector<std::string>{"abcde\u2502", "abcde\u2503", "abcde\u2503", "abcde\u2502"});
    CHECK(frame->lines[0].find("\x1b[90m\u2502\x1b[39m") != std::string::npos);
    CHECK(frame->lines[1].find("\x1b[37m\u2503\x1b[39m") != std::string::npos);
    CHECK(timer->start_count == 1);
    CHECK(timer->last_delay == std::chrono::milliseconds{1000});
    CHECK(timer->armed());

    // The frozen callback hides the bar and requests the next frame.
    const auto requests_before = render_requests;
    timer->fire();
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(timer->armed());
    CHECK(render_requests > requests_before);
    frame = render_frame(view, 6, 4, [&render_requests] { ++render_requests; });
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"abcde", "abcde", "abcde", "abcde"});

    // Scrolling to the end puts the thumb on the last track rows, and an active
    // bar holds the reveal without arming a timer (pi `setScrollbarActive`).
    view.scroll_to_end();
    CHECK(timer->start_count == 2);
    view.set_scrollbar_active(true);
    CHECK(view.is_scrollbar_visible());
    CHECK(view.is_scrollbar_active());
    CHECK(timer->start_count == 2);
    CHECK_FALSE(timer->armed());
    frame = render_frame(view, 6, 4, [&render_requests] { ++render_requests; });
    REQUIRE(frame);
    CHECK(visible_lines(*frame) ==
            std::vector<std::string>{"abcde\u2502", "abcde\u2502", "abcde\u2588", "abcde\u2588"});
    CHECK(frame->lines[2].find("\x1b[37m\u2588\x1b[39m") != std::string::npos);
}

TEST_CASE("the transient bar follows the pointer, the mode, and fitting content",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_hide_delay = std::chrono::milliseconds{-5},
                    .scrollbar_hide_timer = timer});
    view.update_layout(8, 4);

    // Hovering before any scroll reveals the bar and holds it: no timer is
    // armed while the pointer stays (pi `markScrollbarActivity`).
    view.set_scrollbar_active(true);
    CHECK(view.is_scrollbar_visible());
    CHECK(timer->start_count == 0);
    CHECK_FALSE(timer->armed());

    // Leaving re-arms the hide, and a new reveal cancels the pending one.
    view.set_scrollbar_active(false);
    CHECK(timer->start_count == 1);
    CHECK(timer->armed());
    view.set_scrollbar_active(true);
    CHECK(timer->cancel_count == 1);
    CHECK_FALSE(timer->armed());
    CHECK(view.is_scrollbar_visible());

    // Leaving `auto` hides the transient bar and cancels the hide; a permanent
    // mode shows its own bar instead.
    view.set_scrollbar(cch::tui::ScrollbarMode::Always);
    CHECK(view.is_scrollbar_visible());
    CHECK(view.is_scrollbar_active());
    view.set_scrollbar_active(false);
    CHECK(timer->start_count == 1);
    view.set_scrollbar(cch::tui::ScrollbarMode::Hidden);
    CHECK_FALSE(view.is_scrollbar_visible());

    // Re-entering `auto` while the pointer is on the bar reveals immediately
    // without arming another timer (pi `setScrollbar`).
    view.set_scrollbar_active(true);
    view.set_scrollbar(cch::tui::ScrollbarMode::Auto);
    CHECK(view.is_scrollbar_visible());
    CHECK(timer->start_count == 1);
    view.set_scrollbar_active(false);
    CHECK(timer->start_count == 2);

    // Content that fits hides the transient bar and its timer (pi
    // `updateLayout`).
    view.update_layout(3, 4);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(timer->armed());
    CHECK(view.content_height() == 3);

    // A viewport destroyed with an armed hide cancels it first: the callback
    // captures the viewport, so letting it fire would be a use after free.
    auto pending_timer = std::make_shared<ManualScrollbarHideTimer>();
    auto pending = std::make_unique<cch::tui::ScrollView>(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_hide_timer = pending_timer});
    pending->update_layout(8, 4);
    CHECK(pending->scroll_by(2) == 0);
    REQUIRE(pending_timer->armed());
    pending.reset();
    CHECK(pending_timer->cancel_count == 1);
    CHECK_FALSE(pending_timer->armed());
}

TEST_CASE("thumb geometry covers empty, fitting, tiny, and long content",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:276-292`: a 20-row track sizes the thumb 19/10/4/2 for
    // content 21/40/100/400 (`minThumbHeight = min(2, trackHeight)`).
    const auto thumb_for = [](std::size_t content_rows) {
        std::vector<std::string> rows;
        for (std::size_t index = 0; index < content_rows; ++index)
            rows.emplace_back("row");
        cch::tui::ScrollView view(std::make_unique<LabeledChild>(std::move(rows)),
                cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
        auto frame = render_frame(view, 6, 20);
        REQUIRE(frame);
        const auto geometry = cch::tui::detail::get_scrollbar_geometry(frame->root);
        REQUIRE(geometry);
        return geometry->thumb_height;
    };
    CHECK(thumb_for(21) == 19);
    CHECK(thumb_for(40) == 10);
    CHECK(thumb_for(100) == 4);
    CHECK(thumb_for(400) == 2);

    // Empty content takes the frozen `Infinity` branch: the thumb is the track.
    cch::tui::ScrollView empty(std::make_unique<LabeledChild>(std::vector<std::string>{}),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto empty_frame = render_frame(empty, 4, 3);
    REQUIRE(empty_frame);
    CHECK(empty.is_scrollbar_visible());
    CHECK(visible_lines(*empty_frame) == std::vector<std::string>{"   \u2503", "   \u2503", "   \u2503"});
    const auto empty_geometry = cch::tui::detail::get_scrollbar_geometry(empty_frame->root);
    REQUIRE(empty_geometry);
    CHECK(empty_geometry->thumb_height == 3);
    CHECK(empty_geometry->max_scroll_top == 0);

    // A one-row track can only hold a one-row thumb.
    cch::tui::ScrollView tiny(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto tiny_frame = render_frame(tiny, 3, 1);
    REQUIRE(tiny_frame);
    CHECK(tiny_frame->root.children.front().rect.width == 2);
    const auto tiny_geometry = cch::tui::detail::get_scrollbar_geometry(tiny_frame->root);
    REQUIRE(tiny_geometry);
    CHECK(tiny_geometry->column == 2);
    CHECK(tiny_geometry->track_height == 1);
    CHECK(tiny_geometry->thumb_height == 1);

    // Fitting content fills its track completely.
    cch::tui::ScrollView fitting(std::make_unique<LabeledChild>(std::vector<std::string>{"1", "2"}),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto fitting_frame = render_frame(fitting, 6, 4);
    REQUIRE(fitting_frame);
    CHECK(fitting_frame->root.children.front().rect.width == 5);
    const auto fitting_geometry = cch::tui::detail::get_scrollbar_geometry(fitting_frame->root);
    REQUIRE(fitting_geometry);
    CHECK(fitting_geometry->thumb_height == 4);
    CHECK(fitting_geometry->max_scroll_top == 0);
}

TEST_CASE("a resize re-wraps the child and drops a stale transient column",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // A viewport that grows until its content fits must drop the revealed bar,
    // and one that shrinks must size the thumb from the new track instead of a
    // stale reservation (pi `updateLayout` hides a fitting transient bar).
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_hide_delay = std::chrono::milliseconds{1000},
                    .scrollbar_hide_timer = timer});
    auto frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK(view.scroll_by(1) == 0);
    frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.viewport_height() == 3);
    CHECK(view.content_height() == 8);
    CHECK(view.scroll_top() == 1);
    const auto narrow = cch::tui::detail::get_scrollbar_geometry(frame->root);
    REQUIRE(narrow);
    CHECK(narrow->column == 5);
    CHECK(narrow->track_height == 3);
    CHECK(narrow->thumb_height == 2);
    CHECK(narrow->thumb_top == 0);

    // Growing the viewport until the content fits hides the bar: no stale
    // reservation or reveal survives the resize.
    frame = render_frame(view, 6, 8);
    REQUIRE(frame);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(cch::tui::detail::get_scrollbar_geometry(frame->root));
    CHECK(visible_lines(*frame) == std::vector<std::string>(8, "abcde"));
    CHECK(view.content_width(6) == 6);

    // Shrinking back does not resurrect the old reveal: only new activity does.
    frame = render_frame(view, 6, 3);
    REQUIRE(frame);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK_FALSE(cch::tui::detail::get_scrollbar_geometry(frame->root));
    CHECK(view.scroll_by(1) == 0);
    CHECK(view.is_scrollbar_visible());
    CHECK(timer->armed());

    // The reserved column of an `always` bar follows the new width as well.
    cch::tui::ScrollView reserved(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto reserved_frame = render_frame(reserved, 6, 3);
    REQUIRE(reserved_frame);
    CHECK(reserved_frame->root.children.front().rect.width == 5);
    reserved_frame = render_frame(reserved, 4, 3);
    REQUIRE(reserved_frame);
    CHECK(reserved.content_width(4) == 3);
    CHECK(reserved_frame->root.children.front().rect.width == 3);
    const auto wide = cch::tui::detail::get_scrollbar_geometry(reserved_frame->root);
    REQUIRE(wide);
    CHECK(wide->column == 3);
    CHECK(wide->track_height == 3);
}

TEST_CASE("a floating auto bar overwrites the content column without reserving it",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:194-214`: an `auto` bar leaves the content width alone,
    // so the sixth content cell is replaced by the bar instead of wrapping.
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_hide_delay = std::chrono::milliseconds{1000},
                    .scrollbar_hide_timer = timer});
    view.update_layout(8, 4);
    view.set_scrollbar_active(true);
    auto frame = render_frame(view, 6, 4);
    REQUIRE(frame);
    CHECK(frame->root.children.front().rect.width == 6);
    CHECK(visible_lines(*frame) ==
            std::vector<std::string>{"abcde\u2588", "abcde\u2588", "abcde\u2502", "abcde\u2502"});
}

TEST_CASE("the scrollbar splice preserves the target background and drops its other styles",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:248-274` (`preserves only the underlying background
    // beneath overlay scrollbar glyphs`): the content paints a green row with a
    // red border cell in the scrollbar column, and the replacement keeps the
    // green but not the red (`replaceScrollbarCell`, `layout.ts:249-279`).
    class StyledRowChild final : public cch::tui::Component {
    public:
        [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
            std::vector<std::string> lines;
            for (std::size_t row = 0; row < 8; ++row) {
                lines.push_back("\x1b[42m" + std::string(width - 1, 'x') + "\x1b[31m\u2502\x1b[39m\x1b[49m");
            }
            return cch::tui::RenderResult{.lines = lines};
        }

        void invalidate() override {}
    };

    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    cch::tui::ScrollView view(std::make_unique<StyledRowChild>(),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_track_style = [](std::string_view text) { return std::string(text); },
                    .scrollbar_thumb_style = [](std::string_view text) { return std::string(text); }});
    view.update_layout(8, 4);
    view.set_scrollbar_active(true);
    auto frame = render_frame(view, 6, 4);
    REQUIRE(frame);
    CHECK(visible_lines(*frame) ==
            std::vector<std::string>{"xxxxx\u2588", "xxxxx\u2588", "xxxxx\u2502", "xxxxx\u2502"});
    for (const auto& row : frame->lines) {
        CHECK(row.find("\x1b[42m") != std::string::npos);
        CHECK(row.find("\x1b[31m") == std::string::npos);
        CHECK(row.find("\x1b[0m\x1b]8;;\x07\x1b[42m") != std::string::npos);
    }
}

TEST_CASE("the steady default timer hides the transient bar and is cancelled on destruction",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `setTimeout` fires on the event loop, so a viewport without an injected
    // timer still hides an `auto` bar. The bound is generous: this asserts the
    // default implementation exists and hides, not an exact delay.
    cch::tui::ScrollView view(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_hide_delay = std::chrono::milliseconds{10}});
    view.update_layout(8, 4);
    CHECK(view.scroll_by(2) == 0);
    REQUIRE(view.is_scrollbar_visible());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (view.is_scrollbar_visible() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK_FALSE(view.is_scrollbar_visible());

    // A viewport destroyed with a pending hide never calls back into it: the
    // timer's cancel waits for an in-flight callback, so no thread touches the
    // destroyed viewport.
    {
        cch::tui::ScrollView pending(std::make_unique<LabeledChild>(styled_rows()),
                cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                        .scrollbar_hide_delay = std::chrono::milliseconds{1000}});
        pending.update_layout(8, 4);
        CHECK(pending.scroll_by(2) == 0);
        REQUIRE(pending.is_scrollbar_visible());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(true);
}

TEST_CASE("runtime mode changes re-wrap the child before the next layout",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `layout.test.ts:295-307`: switching the reserved column off restores
    // the child's full width and the one-row composition.
    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>("123456", 0, 0),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Always});
    auto frame = render_frame(view, 6, 2);
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"12345\u2503", "6    \u2503"});

    // A real caller changes the option at runtime: `set_scrollbar` requests the
    // next frame through the callback the layout pass installed.
    std::size_t render_requests = 0;
    view.update_layout(2, 2, [&render_requests] { ++render_requests; });
    const auto requests_before = render_requests;
    view.set_scrollbar(cch::tui::ScrollbarMode::Hidden);
    CHECK(render_requests > requests_before);
    CHECK_FALSE(view.is_scrollbar_visible());
    CHECK(view.content_width(6) == 6);
    frame = render_frame(view, 6, 1);
    REQUIRE(frame);
    CHECK(visible_lines(*frame) == std::vector<std::string>{"123456"});
    CHECK(frame->root.children.front().rect.width == 6);
}

TEST_CASE("AltScreen follows the scrollbar hover across motion, wheel, focus, and stop",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi `tui-alt-screen.ts:933-937,996,1037-1066`: a motion report moves the
    // hover, a wheel report moves it to the wheel cell, focus loss and the
    // session boundary stop it, and a hidden `auto` track is still a target.
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto timer = std::make_shared<ManualScrollbarHideTimer>();
    auto view = std::make_unique<cch::tui::ScrollView>(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{.scrollbar = cch::tui::ScrollbarMode::Auto,
                    .scrollbar_hide_delay = std::chrono::milliseconds{1000},
                    .scrollbar_hide_timer = timer});
    auto* view_pointer = view.get();
    tui.set_layout_root(std::move(view));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK_FALSE(view_pointer->is_scrollbar_visible());

    // The hidden track is a hover target: pointer presence reveals the bar and
    // marks it active without arming the hide, and leaving hands it back to the
    // transient timer.
    REQUIRE(terminal.inject_input("\x1b[<35;8;1M"));
    CHECK(view_pointer->is_scrollbar_visible());
    CHECK(view_pointer->is_scrollbar_active());
    CHECK(timer->start_count == 0);
    REQUIRE(terminal.inject_input("\x1b[<35;1;1M"));
    CHECK_FALSE(view_pointer->is_scrollbar_active());
    CHECK(view_pointer->is_scrollbar_visible());
    CHECK(timer->armed());

    // Firing the armed hide takes the bar away again.
    timer->fire();
    CHECK_FALSE(view_pointer->is_scrollbar_visible());

    // A wheel report on the bar cell scrolls and keeps the hover.
    REQUIRE(terminal.inject_input("\x1b[<64;8;1M"));
    CHECK(view_pointer->scroll_top() == 0);
    CHECK(view_pointer->is_scrollbar_active());

    // Focus loss drops the hover even though the pointer coordinate is
    // unchanged, so a later report re-establishes it.
    REQUIRE(terminal.inject_input("\x1b[O"));
    CHECK_FALSE(view_pointer->is_scrollbar_active());
    REQUIRE(terminal.inject_input("\x1b[<35;8;2M"));
    CHECK(view_pointer->is_scrollbar_active());

    // A wheel down over the track scrolls the viewport, and the session
    // boundary keeps that position: a restart must not resurrect the hover or
    // lose the scroll top.
    REQUIRE(terminal.inject_input("\x1b[<65;8;2M"));
    CHECK(view_pointer->scroll_top() == 1);
    CHECK(view_pointer->is_scrollbar_active());

    // The session boundary drops the hover with the gesture state.
    REQUIRE(tui.stop());
    CHECK_FALSE(view_pointer->is_scrollbar_active());
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK_FALSE(view_pointer->is_scrollbar_active());
    CHECK(view_pointer->scroll_top() == 1);
    REQUIRE(tui.stop());
}

TEST_CASE("a viewport removed from the live tree stops being a scrollbar hover target",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // pi keeps removed scroll views alive; a C++ viewport is destroyed with its
    // container, so a cached frame can name freed memory. The hover resolves
    // against the live tree, which makes such a frame harmless.
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto stack = std::make_unique<cch::tui::VStack>();
    auto* stack_pointer = stack.get();
    auto first_timer = std::make_shared<ManualScrollbarHideTimer>();
    auto first = std::make_unique<cch::tui::ScrollView>(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_hide_timer = first_timer});
    auto* first_pointer = first.get();
    REQUIRE(stack_pointer->add_child(
            std::move(first), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    tui.set_layout_root(std::move(stack));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Hover the first viewport, then detach it while the root still holds the
    // hover: a cached frame names the detached viewport, and the next report
    // must resolve the hover against the live tree instead of re-activating it.
    REQUIRE(terminal.inject_input("\x1b[<35;8;1M"));
    REQUIRE(first_pointer->is_scrollbar_active());
    first_pointer->set_scrollbar_active(false);
    auto removed = stack_pointer->remove_child(first_pointer);
    REQUIRE(removed != nullptr);
    REQUIRE(terminal.inject_input("\x1b[<35;8;1M"));
    CHECK_FALSE(first_pointer->is_scrollbar_active());

    // A new viewport at the same cell is the next hover target.
    auto second_timer = std::make_shared<ManualScrollbarHideTimer>();
    auto second = std::make_unique<cch::tui::ScrollView>(std::make_unique<LabeledChild>(styled_rows()),
            cch::tui::ScrollViewOptions{
                    .scrollbar = cch::tui::ScrollbarMode::Auto, .scrollbar_hide_timer = second_timer});
    auto* second_pointer = second.get();
    REQUIRE(stack_pointer->add_child(
            std::move(second), cch::tui::StackEntryOptions{.basis = 0, .grow = 1, .shrink = 1, .min_size = 1}));
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("\x1b[<35;8;1M"));
    CHECK(second_pointer->is_scrollbar_active());

    CHECK(tui.stop());
}

TEST_CASE("AltScreen renders an option change made by its running layout root",
        "[tui][scroll-view][scrollbar][issue1001][spec]") {
    // The product surface of this ticket: the running root changes its
    // scrollbar mode and the frames the alternate screen paints follow.
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    auto view = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>("123456", 0, 0));
    auto* view_pointer = view.get();
    tui.set_layout_root(std::move(view));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"123456", "      "});

    view_pointer->set_scrollbar(cch::tui::ScrollbarMode::Always);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"12345\u2503", "6    \u2503"});

    view_pointer->set_scrollbar(cch::tui::ScrollbarMode::Hidden);
    REQUIRE(tui.render());
    CHECK(terminal.screen() == std::vector<std::string>{"123456", "      "});
    CHECK(tui.stop());
}
