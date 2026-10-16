#include <cch/tui/ITui.hpp>
#include <cch/tui/Overlay.hpp>
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
