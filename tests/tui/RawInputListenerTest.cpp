#include <cch/tui/Input.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// One real reusable TUI host: a Tui over a VirtualTerminal with a focused
/// single-line Input, so every case observes both the raw-input listener stage
/// and the text a focused editor actually received.
class ListenerHost {
public:
    ListenerHost() : terminal_({.columns = 24, .rows = 2}), tui_(terminal_) {
        auto input = std::make_unique<tui::Input>();
        input_ = input.get();
        (void)tui_.add_child(std::move(input));
        (void)tui_.start();
        (void)tui_.set_focus(input_);
    }

    [[nodiscard]] tui::Tui& tui() noexcept { return tui_; }
    [[nodiscard]] tui::VirtualTerminal& terminal() noexcept { return terminal_; }
    [[nodiscard]] std::string value() const { return input_->value(); }

    [[nodiscard]] support::ExpectedVoid focus() { return tui_.set_focus(input_); }

    [[nodiscard]] support::ExpectedVoid type(const std::string& bytes) { return terminal_.inject_input(bytes); }

    /// Records the text every listener observed, in call order.
    class Recorder {
    public:
        std::vector<std::string> observed;

        [[nodiscard]] tui::RawInputListener pass() {
            return [this](std::string_view text) {
                observed.emplace_back(text);
                return tui::RawInputListenerResult::pass();
            };
        }

        [[nodiscard]] tui::RawInputListener named(const std::string& name) {
            return [this, name](std::string_view text) {
                observed.push_back(name + "=" + std::string(text));
                return tui::RawInputListenerResult::pass();
            };
        }
    };

private:
    tui::VirtualTerminal terminal_;
    tui::Tui tui_;
    tui::Input* input_{nullptr};
};

} // namespace

TEST_CASE("raw-input listeners observe input in registration order before the focused editor",
        "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto first = host.tui().add_raw_input_listener(recorder.named("A"));
    auto second = host.tui().add_raw_input_listener(recorder.named("B"));

    REQUIRE(host.type("ab"));
    REQUIRE(first.active());
    REQUIRE(second.active());

    // The listeners saw the raw bytes in registration order, and the focused
    // editor received exactly the text that passed the chain.
    CHECK(recorder.observed == std::vector<std::string>{"A=ab", "B=ab"});
    CHECK(host.value() == "ab");
}

TEST_CASE("raw-input listeners observe bracketed paste with terminal framing", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto handle = host.tui().add_raw_input_listener(recorder.pass());

    REQUIRE(host.type("\x1b[200~hello\x1b[201~"));

    CHECK(recorder.observed == std::vector<std::string>{"\x1b[200~hello\x1b[201~"});
    CHECK(host.value() == "hello");
    CHECK(handle.active());
}

TEST_CASE("raw-input listeners observe a Kitty key-release report as raw input", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto handle = host.tui().add_raw_input_listener(recorder.pass());

    REQUIRE(host.type("\x1b[97;2u"));

    CHECK(recorder.observed == std::vector<std::string>{"\x1b[97;2u"});
    CHECK(handle.active());
}

TEST_CASE("consuming raw input stops later listeners and typed dispatch", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto consuming = host.tui().add_raw_input_listener([&recorder](std::string_view text) {
        recorder.observed.emplace_back(text);
        return tui::RawInputListenerResult::consume();
    });
    auto later = host.tui().add_raw_input_listener(recorder.pass());

    REQUIRE(host.type("a"));
    REQUIRE(host.type("b"));

    // Neither the later listener nor the focused editor observes consumed input.
    CHECK(recorder.observed == std::vector<std::string>{"a", "b"});
    CHECK(host.value().empty());
    CHECK(consuming.active());
    CHECK(later.active());
}

TEST_CASE("rewrites continue through the chain and the last rewrite reaches the focused editor",
        "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto first = host.tui().add_raw_input_listener([&recorder](std::string_view text) {
        recorder.observed.emplace_back(text);
        return tui::RawInputListenerResult::replace("");
    });
    auto second = host.tui().add_raw_input_listener([&recorder](std::string_view text) {
        recorder.observed.emplace_back(text);
        return tui::RawInputListenerResult::replace("x");
    });

    REQUIRE(host.type("a"));

    // B observes the empty string A produced, not the original bytes, and the
    // editor receives B's replacement instead of an empty dispatch.
    CHECK(recorder.observed == std::vector<std::string>{"a", ""});
    CHECK(host.value() == "x");
}

TEST_CASE(
        "a rewrite to the empty string runs every listener and stops typed dispatch", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto replacing = host.tui().add_raw_input_listener([&recorder](std::string_view text) {
        recorder.observed.emplace_back(text);
        return tui::RawInputListenerResult::replace("");
    });
    auto observing = host.tui().add_raw_input_listener(recorder.pass());

    REQUIRE(host.type("a"));

    // Every listener ran, the last observed the empty rewrite, and no editor
    // text was produced from the discarded input.
    CHECK(recorder.observed == std::vector<std::string>{"a", ""});
    CHECK(host.value().empty());
    CHECK(replacing.active());
    CHECK(observing.active());
}

TEST_CASE("removing a listener during dispatch stops its later callbacks", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    tui::RawInputListenerId later_id = 0;
    auto self_removing = host.tui().add_raw_input_listener([&recorder](std::string_view) {
        recorder.observed.emplace_back("self");
        return tui::RawInputListenerResult::pass();
    });
    auto removing = host.tui().add_raw_input_listener([&](std::string_view) {
        recorder.observed.emplace_back("removing");
        // Both the listener before and one after leave the chain from here on.
        (void)host.tui().remove_raw_input_listener(self_removing.id());
        (void)host.tui().remove_raw_input_listener(later_id);
        return tui::RawInputListenerResult::pass();
    });
    auto later = host.tui().add_raw_input_listener([&recorder](std::string_view) {
        recorder.observed.emplace_back("later");
        return tui::RawInputListenerResult::pass();
    });
    later_id = later.id();

    REQUIRE(host.type("a"));
    CHECK(recorder.observed == std::vector<std::string>{"self", "removing"});
    CHECK_FALSE(self_removing.active());
    CHECK_FALSE(later.active());

    // No stale callback survives the removals, and the removed listeners' bytes
    // are no longer typed.
    recorder.observed.clear();
    REQUIRE(host.type("b"));
    CHECK(recorder.observed.empty());
    CHECK(host.value() == "b");
}

TEST_CASE("destroying a listener handle removes it, and a handle outliving its host is inert",
        "[tui][input][issue952][spec]") {
    ListenerHost::Recorder recorder;
    tui::RawInputListenerHandle surviving;
    {
        ListenerHost host;
        {
            auto scoped = host.tui().add_raw_input_listener(recorder.pass());
            REQUIRE(host.type("a"));
            CHECK(recorder.observed == std::vector<std::string>{"a"});
            CHECK(host.value() == "a");
        }
        // The scoped handle is gone: no callback and no typed text for "b".
        REQUIRE(host.type("b"));
        CHECK(recorder.observed == std::vector<std::string>{"a"});
        CHECK(host.value() == "a");

        surviving = host.tui().add_raw_input_listener(recorder.pass());
        REQUIRE(surviving.active());
    }
    // The host was destroyed before its handle; disposing the orphaned handle
    // touches nothing and leaves no stale callback.
    CHECK_FALSE(surviving.active());
    CHECK(surviving.id() == 0);
    surviving.reset();
}

TEST_CASE("stop and restart keep registered listeners live without duplicating them", "[tui][input][issue952][spec]") {
    ListenerHost host;
    ListenerHost::Recorder recorder;
    auto handle = host.tui().add_raw_input_listener(recorder.pass());

    REQUIRE(host.type("a"));
    REQUIRE(host.tui().stop());
    REQUIRE(host.tui().start());
    REQUIRE(host.focus());
    REQUIRE(host.type("b"));

    // Exactly one callback per input across the restart, and the editor holds
    // the text of both.
    CHECK(recorder.observed == std::vector<std::string>{"a", "b"});
    CHECK(host.value() == "ab");
    CHECK(handle.active());
}

TEST_CASE("a cell-size reply reaches raw listeners before the downstream cell-size consumer",
        "[tui][input][issue952][spec]") {
    ListenerHost host;
    std::vector<tui::CellPixelDimensions> observed_during_dispatch;
    auto handle = host.tui().add_raw_input_listener([&](std::string_view text) {
        observed_during_dispatch.push_back(host.terminal().capabilities().cell_pixels.value_or({}));
        return tui::RawInputListenerResult::replace(std::string(text));
    });

    REQUIRE(host.type("\x1b[6;20;10t"));

    // The listener observed the reply verbatim while the cell size was still the
    // 9x18 default, so the downstream consumer ran after the chain.
    CHECK(observed_during_dispatch == std::vector<tui::CellPixelDimensions>{tui::CellPixelDimensions{}});
    const auto capabilities = host.terminal().capabilities();
    REQUIRE(capabilities.cell_pixels.has_value());
    CHECK(*capabilities.cell_pixels == tui::CellPixelDimensions{.width = 10, .height = 20});
    // The reply is never typed into the focused editor.
    CHECK(host.value().empty());
    CHECK(handle.active());
}

TEST_CASE("consuming or rewriting a cell-size reply decides the resulting cell size", "[tui][input][issue952][spec]") {
    ListenerHost host;
    auto consuming =
            host.tui().add_raw_input_listener([](std::string_view) { return tui::RawInputListenerResult::consume(); });

    REQUIRE(host.type("\x1b[6;20;10t"));
    CHECK(host.terminal().capabilities().cell_pixels == tui::CellPixelDimensions{.width = 9, .height = 18});

    consuming.reset();
    auto rewriting = host.tui().add_raw_input_listener(
            [](std::string_view) { return tui::RawInputListenerResult::replace("\x1b[6;30;40t"); });
    REQUIRE(rewriting.active());

    // The rewritten bytes are what the downstream consumer parses.
    REQUIRE(host.type("\x1b[6;20;10t"));
    CHECK(host.terminal().capabilities().cell_pixels == tui::CellPixelDimensions{.width = 40, .height = 30});
    CHECK(host.value().empty());
}

TEST_CASE("removing an unknown raw-input listener reports a validation error", "[tui][input][issue952][spec]") {
    ListenerHost host;
    const auto handle =
            host.tui().add_raw_input_listener([](std::string_view) { return tui::RawInputListenerResult::pass(); });

    const auto removed = host.tui().remove_raw_input_listener(handle.id());
    REQUIRE(removed);
    CHECK_FALSE(handle.active());

    const auto unknown = host.tui().remove_raw_input_listener(handle.id());
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == support::ErrorCode::Validation);
}
