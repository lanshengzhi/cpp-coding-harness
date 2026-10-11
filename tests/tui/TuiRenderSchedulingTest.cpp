#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class ManualTuiScheduler final : public cch::tui::TuiScheduler {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override { return now_; }

    void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }

    void schedule(std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        scheduled_delay_ = delay;
        scheduled_ = std::move(work);
        ++schedule_count_;
    }

    void cancel() noexcept override {
        scheduled_ = nullptr;
        ++cancel_count_;
    }

    void advance(std::chrono::steady_clock::duration duration) { now_ += duration; }

    void run_posted() {
        auto posted = std::move(posted_);
        posted_.clear();
        for (auto& work : posted)
            work();
    }

    void run_scheduled() {
        auto scheduled = take_scheduled();
        if (scheduled) scheduled();
    }

    [[nodiscard]] cch::tui::TuiScheduledWork take_scheduled() { return std::exchange(scheduled_, nullptr); }
    [[nodiscard]] bool has_scheduled() const noexcept { return static_cast<bool>(scheduled_); }

    std::chrono::steady_clock::duration scheduled_delay_{};
    std::size_t schedule_count_{0};
    std::size_t cancel_count_{0};

private:
    std::chrono::steady_clock::time_point now_{};
    cch::tui::TuiScheduledWork scheduled_;
    std::vector<cch::tui::TuiScheduledWork> posted_;
};

[[nodiscard]] std::string visible_screen(const cch::tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text += line;
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE("Tui schedules its first frame and coalesces requests at 16 milliseconds", "[tui][render][issue978][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    cch::tui::Tui tui{terminal};
    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("first frame", 0, 0)));
    REQUIRE(tui.start());

    CHECK(visible_screen(terminal).find("first frame") == std::string::npos);
    CHECK(scheduler.scheduled_delay_ == std::chrono::steady_clock::duration::zero());
    scheduler.run_scheduled();
    CHECK(visible_screen(terminal).find("first frame") != std::string::npos);

    tui.request_render();
    CHECK(scheduler.scheduled_delay_ == std::chrono::milliseconds{16});
    tui.request_render();
    CHECK(scheduler.schedule_count_ == 2);
    scheduler.advance(std::chrono::milliseconds{16});
    scheduler.run_scheduled();
    REQUIRE(tui.stop());
}

TEST_CASE("Tui renders keyboard input on the next execution turn", "[tui][render][issue978][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    cch::tui::Tui tui{terminal};
    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    auto input = std::make_unique<cch::tui::Input>();
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.start());
    scheduler.run_scheduled();
    REQUIRE(tui.set_focus(input_pointer));
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("x"));
    CHECK(visible_screen(terminal).find("x") == std::string::npos);
    scheduler.run_posted();
    CHECK(visible_screen(terminal).find("x") != std::string::npos);
    REQUIRE(tui.stop());
}

TEST_CASE("Tui immediate and forced renders preempt pending work", "[tui][render][issue978][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    cch::tui::Tui tui{terminal};
    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    auto text = std::make_unique<cch::tui::Text>("before", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    scheduler.run_scheduled();

    tui.request_render();
    REQUIRE(scheduler.schedule_count_ == 2);
    text_pointer->set_text("keyboard");
    tui.request_immediate_render();
    CHECK(visible_screen(terminal).find("before") != std::string::npos);
    scheduler.run_posted();
    CHECK(visible_screen(terminal).find("keyboard") != std::string::npos);
    scheduler.run_scheduled();
    CHECK(visible_screen(terminal).find("keyboard") != std::string::npos);

    text_pointer->set_text("forced");
    tui.request_render(true);
    CHECK(visible_screen(terminal).find("forced") != std::string::npos);
    const auto scheduled_count = scheduler.schedule_count_;
    scheduler.run_scheduled();
    CHECK(scheduler.schedule_count_ == scheduled_count);
    REQUIRE(tui.stop());
}

TEST_CASE("Tui cancels scheduled work across stop and restart", "[tui][render][issue978][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 2});
    cch::tui::Tui tui{terminal};
    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("lifecycle", 0, 0)));
    REQUIRE(tui.start());
    auto old_work = scheduler.take_scheduled();
    REQUIRE(tui.stop());
    REQUIRE(tui.start());
    REQUIRE(scheduler.has_scheduled());

    old_work();
    CHECK(visible_screen(terminal).find("lifecycle") == std::string::npos);
    scheduler.run_scheduled();
    CHECK(visible_screen(terminal).find("lifecycle") != std::string::npos);
    REQUIRE(tui.stop());
}
