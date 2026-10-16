#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/ITui.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>

namespace {

struct InputStats {
    bool destroyed{false};
    bool focused{false};
    std::size_t received{0};
    std::size_t invalidation_count{0};
};

class FocusableInput final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    explicit FocusableInput(InputStats* stats = nullptr) : stats_(stats) {}
    ~FocusableInput() override {
        if (stats_ != nullptr) stats_->destroyed = true;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"input"}};
    }

    void invalidate() override {
        if (stats_ != nullptr) ++stats_->invalidation_count;
    }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        if (stats_ != nullptr) ++stats_->received;
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override {
        focused_ = focused;
        if (stats_ != nullptr) stats_->focused = focused;
        if (!focused && on_focus_lost) on_focus_lost();
    }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::function<void()> on_focus_lost;

private:
    InputStats* stats_;
    bool focused_{false};
};

class FocusableCursorInput final : public cch::tui::Component, public cch::tui::Focusable {
public:
    explicit FocusableCursorInput(
            std::vector<std::string> lines, std::optional<cch::tui::CursorPosition> cursor = std::nullopt)
        : lines_(std::move(lines)), cursor_(cursor) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    void set_cursor(std::optional<cch::tui::CursorPosition> cursor) { cursor_ = cursor; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return focused_ ? cursor_ : std::nullopt;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

private:
    std::vector<std::string> lines_;
    std::optional<cch::tui::CursorPosition> cursor_;
    bool focused_{false};
};

} // namespace

TEST_CASE("ITui drives the regular MainScreen lifecycle through the common contract", "[tui][issue977][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::MainScreen main_screen{terminal};
    cch::tui::ITui& tui = main_screen;

    InputStats stats;
    auto input = std::make_unique<FocusableInput>(&stats);
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    CHECK(&tui.terminal() == &terminal);
    CHECK(tui.focus() == nullptr);

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(input_pointer));
    CHECK(tui.focus() == input_pointer);
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("x"));
    CHECK(stats.received == 1);
    CHECK(stats.focused);
    bool focus_loss_callback_saw_cleared{false};
    input_pointer->on_focus_lost = [&tui, input_pointer, &focus_loss_callback_saw_cleared] {
        focus_loss_callback_saw_cleared = tui.focus() == nullptr;
        CHECK_FALSE(tui.set_focus(input_pointer));
    };

    tui.invalidate();
    CHECK(stats.invalidation_count == 1);
    REQUIRE(tui.stop());
    CHECK_FALSE(stats.focused);
    CHECK(focus_loss_callback_saw_cleared);
    tui.clear_children();
    CHECK(stats.destroyed);
}

TEST_CASE("ITui removal detaches before focus callbacks and preserves child lifetime", "[tui][issue977][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::MainScreen main_screen{terminal};
    cch::tui::ITui& tui = main_screen;

    InputStats stats;
    auto input = std::make_unique<FocusableInput>(&stats);
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(input_pointer));

    bool focus_callback_saw_detached{false};
    input_pointer->on_focus_lost = [&tui, input_pointer, &focus_callback_saw_detached, &stats] {
        focus_callback_saw_detached = tui.focus() != input_pointer && !stats.destroyed;
    };
    REQUIRE(tui.remove_child(input_pointer));
    CHECK(tui.focus() == nullptr);
    CHECK(focus_callback_saw_detached);
    CHECK(stats.destroyed);
    CHECK_FALSE(tui.remove_child(nullptr));

    InputStats fallback_stats;
    auto fallback = std::make_unique<FocusableInput>(&fallback_stats);
    auto* fallback_pointer = fallback.get();
    REQUIRE(tui.add_child(std::move(fallback)));
    REQUIRE(tui.set_focus(fallback_pointer));

    // A focus callback may remove its component. Nested child operations defer
    // destruction until focus dispatch has finished.
    InputStats reentrant_stats;
    auto reentrant_input = std::make_unique<FocusableInput>(&reentrant_stats);
    auto* reentrant_pointer = reentrant_input.get();
    bool reentrant_callback_saw_detached{false};
    bool reentrant_removal_succeeded{false};
    reentrant_pointer->on_focus_lost = [&tui,
                                               reentrant_pointer,
                                               &reentrant_stats,
                                               &reentrant_callback_saw_detached,
                                               &reentrant_removal_succeeded] {
        reentrant_removal_succeeded = static_cast<bool>(tui.remove_child(reentrant_pointer));
        reentrant_callback_saw_detached = tui.focus() != reentrant_pointer && !reentrant_stats.destroyed;
    };
    REQUIRE(tui.add_child(std::move(reentrant_input)));
    REQUIRE(tui.set_focus(reentrant_pointer));
    REQUIRE(tui.set_focus(fallback_pointer));
    CHECK(reentrant_removal_succeeded);
    CHECK(reentrant_callback_saw_detached);
    CHECK(reentrant_stats.destroyed);
    CHECK(tui.focus() == fallback_pointer);

    REQUIRE(terminal.inject_input("x"));
    tui.clear_children();
    REQUIRE(tui.stop());
}

TEST_CASE("ITui take_children transfers live components and sees hidden overlay entries",
        "[tui][lifecycle][issue1018][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::MainScreen main_screen{terminal};
    cch::tui::ITui& tui = main_screen;

    InputStats stats;
    auto input = std::make_unique<FocusableInput>(&stats);
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.set_focus(input_pointer));
    CHECK_FALSE(tui.has_overlay_entries());

    auto overlay = std::make_unique<cch::tui::Overlay>();
    overlay->set_visible(false);
    REQUIRE(tui.add_overlay(std::move(overlay)));
    CHECK(tui.has_overlay_entries());

    auto children = tui.take_children();
    REQUIRE(children.size() == 1);
    CHECK(children.front().get() == input_pointer);
    CHECK(tui.focus() == nullptr);
    CHECK_FALSE(stats.destroyed);
    CHECK_FALSE(stats.focused);
    CHECK(tui.has_overlay_entries());

    auto remounted = tui.add_child(std::move(children.front()));
    REQUIRE(remounted);
    CHECK(&remounted->get() == input_pointer);
    REQUIRE(tui.set_focus(input_pointer));
    CHECK(tui.focus() == input_pointer);
    tui.clear_children();
    CHECK(stats.destroyed);
}

TEST_CASE("ITui clear_children does not dispatch to detached children", "[tui][issue977][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::MainScreen main_screen{terminal};
    cch::tui::ITui& tui = main_screen;
    InputStats stats;
    auto input = std::make_unique<FocusableInput>(&stats);
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(input_pointer));

    tui.clear_children();
    CHECK(tui.focus() == nullptr);
    CHECK_FALSE(stats.focused);
    REQUIRE(terminal.inject_input("x"));
    CHECK(stats.received == 0);
    REQUIRE(tui.render());
    REQUIRE(tui.stop());
}

TEST_CASE("ITui roots support hardware-cursor preference and IME positioning",
        "[tui][hardware-cursor][issue1037][spec]") {
    SECTION("defaults to false on both roots") {
        cch::tui::VirtualTerminal term_main({.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{term_main};
        CHECK_FALSE(main_screen.show_hardware_cursor());
        CHECK_FALSE(main_screen.get_show_hardware_cursor());

        cch::tui::VirtualTerminal term_alt({.columns = 20, .rows = 5});
        cch::tui::AltScreen alt_screen{term_alt};
        CHECK_FALSE(alt_screen.show_hardware_cursor());
        CHECK_FALSE(alt_screen.get_show_hardware_cursor());
    }

    SECTION("preference survives stop and restart on ITui") {
        cch::tui::VirtualTerminal term_main({.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{term_main};
        cch::tui::ITui& tui = main_screen;

        tui.set_show_hardware_cursor(true);
        CHECK(tui.show_hardware_cursor());
        REQUIRE(tui.start());
        CHECK(tui.show_hardware_cursor());
        REQUIRE(tui.stop());
        CHECK(tui.show_hardware_cursor());
        REQUIRE(tui.start());
        CHECK(tui.show_hardware_cursor());
        REQUIRE(tui.stop());
    }

    SECTION("MainScreen positions cursor for IME with visibility disabled by default") {
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 5});
        cch::tui::MainScreen main_screen{terminal};
        auto input = std::make_unique<FocusableCursorInput>(
                std::vector<std::string>{"test text"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* input_pointer = input.get();
        REQUIRE(main_screen.add_child(std::move(input)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(input_pointer));
        REQUIRE(main_screen.render());

        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 4);
        CHECK(terminal.cursor().row == 0);

        // When preference enabled, cursor becomes visible at same position
        main_screen.set_show_hardware_cursor(true);
        REQUIRE(main_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 4);
        CHECK(terminal.cursor().row == 0);

        // When cursor is invalid/nullopt, cursor hides even if preference enabled
        input_pointer->set_cursor(std::nullopt);
        REQUIRE(main_screen.render());
        CHECK_FALSE(terminal.modes().cursor_visible);

        REQUIRE(main_screen.stop());
    }

    SECTION("AltScreen positions cursor for IME with visibility disabled by default") {
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 5});
        cch::tui::AltScreen alt_screen{terminal};
        auto input = std::make_unique<FocusableCursorInput>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* input_pointer = input.get();
        REQUIRE(alt_screen.add_child(std::move(input)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(input_pointer));
        REQUIRE(alt_screen.render());

        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 6);
        CHECK(terminal.cursor().row == 1);

        // When preference enabled, cursor becomes visible at same position
        alt_screen.set_show_hardware_cursor(true);
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 6);
        CHECK(terminal.cursor().row == 1);

        // When preference toggled off while running, terminal cursor is immediately hidden
        alt_screen.set_show_hardware_cursor(false);
        CHECK_FALSE(terminal.modes().cursor_visible);

        // When cursor is invalid/nullopt, cursor hides even if preference enabled
        alt_screen.set_show_hardware_cursor(true);
        input_pointer->set_cursor(std::nullopt);
        REQUIRE(alt_screen.render());
        CHECK_FALSE(terminal.modes().cursor_visible);

        REQUIRE(alt_screen.stop());
    }

    SECTION("nested component positions cursor for IME with visibility disabled on MainScreen") {
        cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 10});
        cch::tui::MainScreen main_screen{terminal};

        auto container = std::make_unique<cch::tui::Container>();
        REQUIRE(container->add_child(std::make_unique<cch::tui::Text>("header line", 0, 0)));
        auto box = std::make_unique<cch::tui::Box>(2, 1);
        auto input = std::make_unique<FocusableCursorInput>(
                std::vector<std::string>{"nested input"}, cch::tui::CursorPosition{.column = 3, .row = 0});
        auto* input_pointer = input.get();
        REQUIRE(box->add_child(std::move(input)));
        REQUIRE(container->add_child(std::move(box)));

        REQUIRE(main_screen.add_child(std::move(container)));
        REQUIRE(main_screen.start());
        REQUIRE(main_screen.set_focus(input_pointer));
        REQUIRE(main_screen.render());

        // Box padding is left=2, top=1, plus 1 header line before box -> row = 1 + 1 = 2, col = 2 + 3 = 5
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 5);
        CHECK(terminal.cursor().row == 2);

        // When preference enabled, visible at same nested coordinates
        main_screen.set_show_hardware_cursor(true);
        REQUIRE(main_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 5);
        CHECK(terminal.cursor().row == 2);

        REQUIRE(main_screen.stop());
    }

    SECTION("nested component positions cursor for IME with visibility disabled on AltScreen") {
        cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 10});
        cch::tui::AltScreen alt_screen{terminal};

        auto container = std::make_unique<cch::tui::Container>();
        REQUIRE(container->add_child(std::make_unique<cch::tui::Text>("first row", 0, 0)));
        auto box = std::make_unique<cch::tui::Box>(3, 1);
        auto input = std::make_unique<FocusableCursorInput>(
                std::vector<std::string>{"alt nested"}, cch::tui::CursorPosition{.column = 4, .row = 0});
        auto* input_pointer = input.get();
        REQUIRE(box->add_child(std::move(input)));
        REQUIRE(container->add_child(std::move(box)));

        REQUIRE(alt_screen.add_child(std::move(container)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(input_pointer));
        REQUIRE(alt_screen.render());

        // Box padding left=3, top=1, header 1 row -> row = 1 + 1 = 2, col = 3 + 4 = 7
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 7);
        CHECK(terminal.cursor().row == 2);

        // Enabled visibility
        alt_screen.set_show_hardware_cursor(true);
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 7);
        CHECK(terminal.cursor().row == 2);

        REQUIRE(alt_screen.stop());
    }

    SECTION("scrolled and clipped cursor positions for IME and hides when scrolled off on AltScreen") {
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
        cch::tui::AltScreen alt_screen{terminal};

        // Create 8 lines of text followed by a cursor input
        std::vector<std::string> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(std::format("line {}", i));
        auto input = std::make_unique<FocusableCursorInput>(lines, cch::tui::CursorPosition{.column = 5, .row = 2});
        auto* input_pointer = input.get();

        REQUIRE(alt_screen.add_child(std::move(input)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(input_pointer));
        REQUIRE(alt_screen.render());

        // Following document end in a 4-row terminal with 8 lines scrolls viewport_top to 4
        // Cursor at line 2 is above viewport_top (4), so it is not visible
        CHECK_FALSE(terminal.modes().cursor_visible);

        // Scroll to top: line 2 is visible at row 2
        alt_screen.scroll_to_top();
        REQUIRE(alt_screen.render());
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 5);
        CHECK(terminal.cursor().row == 2);

        // Turn on preference: cursor visible at row 2, col 5
        alt_screen.set_show_hardware_cursor(true);
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 5);
        CHECK(terminal.cursor().row == 2);

        // Scroll by 1 down: viewport_top is 1, so line 2 is at row 2 - 1 = 1
        alt_screen.scroll_by(1);
        REQUIRE(alt_screen.render());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 5);
        CHECK(terminal.cursor().row == 1);

        // Scroll to bottom: line 2 is scrolled off, so cursor hides even with preference true
        alt_screen.scroll_to_bottom();
        REQUIRE(alt_screen.render());
        CHECK_FALSE(terminal.modes().cursor_visible);

        REQUIRE(alt_screen.stop());
    }

    SECTION("AltScreen retains hardware-cursor visibility across restore and start") {
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 5});
        cch::tui::AltScreen alt_screen{terminal};
        alt_screen.set_show_hardware_cursor(true);

        auto input = std::make_unique<FocusableCursorInput>(
                std::vector<std::string>{"line 0", "line 1"}, cch::tui::CursorPosition{.column = 6, .row = 1});
        auto* input_pointer = input.get();
        REQUIRE(alt_screen.add_child(std::move(input)));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(input_pointer));
        REQUIRE(alt_screen.render());

        CHECK(alt_screen.show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 6);
        CHECK(terminal.cursor().row == 1);

        const auto state = alt_screen.capture_render_state();
        REQUIRE(alt_screen.stop());

        REQUIRE(alt_screen.restore_render_state(state));
        REQUIRE(alt_screen.start());
        REQUIRE(alt_screen.set_focus(input_pointer));
        REQUIRE(alt_screen.render());

        CHECK(alt_screen.show_hardware_cursor());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.cursor().column == 6);
        CHECK(terminal.cursor().row == 1);

        REQUIRE(alt_screen.stop());
    }
}
