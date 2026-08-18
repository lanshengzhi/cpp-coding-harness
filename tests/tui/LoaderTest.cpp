#include <cch/tui/CancellableLoader.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class ManualAnimationTimer final : public cch::tui::AnimationTimer {
public:
    [[nodiscard]] cch::support::ExpectedVoid start(
        std::chrono::milliseconds interval,
        cch::tui::AnimationTickSink tick) override {
        interval_ = interval;
        tick_ = std::move(tick);
        ++starts;
        return {};
    }

    void stop() override {
        tick_ = {};
        ++stops;
    }

    void fire() {
        if (tick_) (void)tick_();
    }

    std::chrono::milliseconds interval_{0};
    std::size_t starts{0};
    std::size_t stops{0};

private:
    cch::tui::AnimationTickSink tick_;
};

} // namespace

TEST_CASE("Loader requests renders for animation and stops without busy waiting", "[tui][loader][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto timer = std::make_unique<ManualAnimationTimer>();
    auto* timer_ptr = timer.get();
    std::size_t requests = 0;
    auto loader = std::make_unique<cch::tui::Loader>(cch::tui::LoaderOptions{
        .request_render = [&]() -> cch::support::ExpectedVoid {
            ++requests;
            tui.invalidate();
            return {};
        },
        .message = "Working",
        .indicator = cch::tui::LoaderIndicatorOptions{
            .frames = std::vector<std::string>{"A", "B"},
            .interval = std::chrono::milliseconds(25),
        },
        .animation_timer = std::move(timer),
    });
    auto* loader_ptr = loader.get();
    REQUIRE(tui.add_child(std::move(loader)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find_first_not_of(' ') == std::string::npos);
    CHECK(terminal.screen()[1].find("A Working") != std::string::npos);
    CHECK(timer_ptr->interval_ == std::chrono::milliseconds(25));
    CHECK(timer_ptr->starts == 1);

    const auto requests_before_tick = requests;
    timer_ptr->fire();
    CHECK(requests == requests_before_tick + 1);
    REQUIRE(tui.render());
    CHECK(terminal.screen()[1].find("B Working") != std::string::npos);

    loader_ptr->set_message("Still working");
    REQUIRE(tui.render());
    CHECK(terminal.screen()[1].find("B Still working") != std::string::npos);

    loader_ptr->stop();
    const auto requests_after_stop = requests;
    timer_ptr->fire();
    CHECK(requests == requests_after_stop);
    CHECK_FALSE(loader_ptr->running());
}

TEST_CASE("Loader safely replaces its indicator and handles an empty frame list", "[tui][loader][issue52]") {
    auto timer = std::make_unique<ManualAnimationTimer>();
    auto* timer_ptr = timer.get();
    cch::tui::Loader loader(cch::tui::LoaderOptions{
        .message = "Loading",
        .animation_timer = std::move(timer),
    });

    loader.set_indicator(cch::tui::LoaderIndicatorOptions{
        .frames = std::vector<std::string>{},
        .interval = std::chrono::milliseconds(0),
    });
    auto rendered = loader.render(20);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 2);
    CHECK(rendered->lines[1] == " Loading");
    CHECK(timer_ptr->starts == 1);

    loader.set_indicator(cch::tui::LoaderIndicatorOptions{
        .frames = std::vector<std::string>{"-"},
        .interval = std::chrono::milliseconds(0),
    });
    rendered = loader.render(20);
    REQUIRE(rendered);
    CHECK(rendered->lines[1] == " - Loading");
    CHECK(timer_ptr->starts == 1);
}

TEST_CASE("CancellableLoader mutations permit render-request reentry", "[tui][loader][issue52]") {
    cch::tui::CancellableLoader* loader_ptr = nullptr;
    bool armed = false;
    std::size_t reentries = 0;
    cch::tui::CancellableLoader loader(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .request_render = [&]() -> cch::support::ExpectedVoid {
                if (!armed) return {};
                CHECK(loader_ptr->state() == cch::tui::CancellableLoaderState::Active);
                ++reentries;
                return {};
            },
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
    });
    loader_ptr = &loader;
    armed = true;

    loader.set_message("Updated");
    loader.set_indicator(cch::tui::LoaderIndicatorOptions{
        .frames = std::vector<std::string>{"."},
    });
    CHECK(reentries == 2);
}

TEST_CASE("CancellableLoader arbitrates completion and cancellation exactly once", "[tui][loader][issue52]") {
    std::size_t completions = 0;
    std::size_t cancellations = 0;
    cch::tui::CancellableLoader completed(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .on_complete = [&completions]() -> cch::support::ExpectedVoid { ++completions; return {}; },
        .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { ++cancellations; return {}; },
    });
    CHECK(completed.complete());
    CHECK_FALSE(completed.complete());
    CHECK_FALSE(completed.cancel());
    CHECK(completions == 1);
    CHECK(cancellations == 0);
    CHECK(completed.state() == cch::tui::CancellableLoaderState::Completed);

    cch::tui::CancellableLoader cancelled(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .on_complete = [&completions]() -> cch::support::ExpectedVoid { ++completions; return {}; },
        .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { ++cancellations; return {}; },
    });
    const auto token = cancelled.cancellation_token();
    std::optional<cch::tui::CancellableLoaderState> callback_state;
    std::stop_callback callback(token, [&]() { callback_state = cancelled.state(); });
    cancelled.handle_input(cch::tui::KeyEvent{.key = "escape"});
    cancelled.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(token.stop_requested());
    REQUIRE(callback_state);
    CHECK(*callback_state == cch::tui::CancellableLoaderState::Cancelled);
    CHECK(cancellations == 1);
    CHECK_FALSE(cancelled.complete());
    CHECK(cancelled.cancelled());
}

TEST_CASE("CancellableLoader dispatches cancellation from its effective registry", "[tui][loader][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.select.cancel", .keys = {"f2"}}};
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    cch::tui::CancellableLoader loader(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .keybindings = keybindings->registry,
    });
    loader.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(loader.state() == cch::tui::CancellableLoaderState::Active);
    loader.handle_input(cch::tui::KeyEvent{.key = "f2"});
    CHECK(loader.state() == cch::tui::CancellableLoaderState::Cancelled);
}

TEST_CASE("CancellableLoader resolves concurrent terminal outcomes exactly once", "[tui][loader][issue52]") {
    std::atomic_size_t completions{0};
    std::atomic_size_t cancellations{0};
    cch::tui::CancellableLoader loader(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .on_complete = [&completions]() -> cch::support::ExpectedVoid { completions.fetch_add(1); return {}; },
        .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { cancellations.fetch_add(1); return {}; },
    });

    std::thread complete_thread([&loader]() { (void)loader.complete(); });
    std::thread cancel_thread([&loader]() { (void)loader.cancel(); });
    complete_thread.join();
    cancel_thread.join();

    CHECK(completions.load() + cancellations.load() == 1);
    CHECK((loader.state() == cch::tui::CancellableLoaderState::Completed ||
           loader.state() == cch::tui::CancellableLoaderState::Cancelled));
}

TEST_CASE("Cancelling a stacked loader restores the previous overlay input target", "[tui][loader][overlay][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 5});
    cch::tui::Tui tui(terminal);
    cch::tui::OverlayOptions lower_options;
    lower_options.z_index = 1;
    auto lower = std::make_unique<cch::tui::Overlay>(std::move(lower_options));
    auto lower_list = std::make_unique<cch::tui::SelectList>(
        std::vector<cch::tui::SelectItem>{
            {.value = "first", .label = "First"},
            {.value = "second", .label = "Second"},
        });
    auto* lower_list_ptr = lower_list.get();
    REQUIRE(lower->add_child(std::move(lower_list)));
    REQUIRE(lower->focus_first());
    auto* lower_ptr = lower.get();
    REQUIRE(tui.add_overlay(std::move(lower)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(lower_ptr));

    cch::tui::Overlay* upper_ptr = nullptr;
    cch::tui::OverlayOptions upper_options;
    upper_options.z_index = 2;
    auto upper = std::make_unique<cch::tui::Overlay>(std::move(upper_options));
    upper_ptr = upper.get();
    auto loader = std::make_unique<cch::tui::CancellableLoader>(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .on_cancel = [&]() -> cch::support::ExpectedVoid { REQUIRE(tui.remove_overlay(upper_ptr)); return {}; },
    });
    REQUIRE(upper->add_child(std::move(loader)));
    REQUIRE(upper->focus_first());
    REQUIRE(tui.add_overlay(std::move(upper)));
    REQUIRE(tui.set_focus(upper_ptr));

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(lower_ptr->focused());
    REQUIRE(lower_list_ptr->focused());
    REQUIRE(terminal.inject_input("\x1b[B"));
    REQUIRE(lower_list_ptr->selected_item());
    CHECK(lower_list_ptr->selected_item()->value == "second");
}

TEST_CASE("Cancelling an overlay loader restores focus through VirtualTerminal", "[tui][loader][overlay][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 5});
    cch::tui::Tui tui(terminal);
    auto fallback = std::make_unique<cch::tui::SelectList>(
        std::vector<cch::tui::SelectItem>{{.value = "fallback", .label = "Fallback"}});
    auto* fallback_ptr = fallback.get();
    REQUIRE(tui.add_child(std::move(fallback)));
    auto previous = std::make_unique<cch::tui::SelectList>(
        std::vector<cch::tui::SelectItem>{{.value = "previous", .label = "Previous"}});
    auto* previous_ptr = previous.get();
    REQUIRE(tui.add_child(std::move(previous)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(previous_ptr));

    cch::tui::Overlay* overlay_ptr = nullptr;
    std::size_t cancellations = 0;
    auto overlay = std::make_unique<cch::tui::Overlay>();
    overlay_ptr = overlay.get();
    auto loader = std::make_unique<cch::tui::CancellableLoader>(cch::tui::CancellableLoaderOptions{
        .loader = cch::tui::LoaderOptions{
            .message = "Press escape",
            .indicator = cch::tui::LoaderIndicatorOptions{.frames = std::vector<std::string>{}},
            .animation_timer = std::make_unique<ManualAnimationTimer>(),
        },
        .on_cancel = [&]() -> cch::support::ExpectedVoid {
            ++cancellations;
            REQUIRE(tui.remove_overlay(overlay_ptr));
            return {};
        },
    });
    REQUIRE(overlay->add_child(std::move(loader)));
    REQUIRE(overlay->focus_first());
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.set_focus(overlay_ptr));

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.flush_input());

    CHECK(cancellations == 1);
    CHECK(previous_ptr->focused());
    CHECK_FALSE(fallback_ptr->focused());
}
