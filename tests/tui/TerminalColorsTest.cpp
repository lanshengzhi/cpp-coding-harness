// Unit tests for terminal-color parsing, the query burst, and the TUI root's
// color query / live scheme lifecycle (#990). Behavioral baseline: pi 83114817
// packages/tui/src/terminal-colors.ts (parseOscColorResponse,
// parseTerminalColorSchemeReport, TERMINAL_COLOR_QUERY) and
// packages/tui/src/tui.ts (queryTerminalColors, 2031 notifications).

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cch;

TEST_CASE("hex color channels scale to eight-bit values", "[tui][theme][issue990][spec]") {
    CHECK(tui::parse_osc_hex_channel("00") == 0);
    CHECK(tui::parse_osc_hex_channel("ff") == 255);
    CHECK(tui::parse_osc_hex_channel("FF") == 255);
    CHECK(tui::parse_osc_hex_channel("80") == 128);
    // 16-bit-per-channel scaling: 4 hex digits normalized over 0xffff.
    CHECK(tui::parse_osc_hex_channel("ffff") == 255);
    CHECK(tui::parse_osc_hex_channel("0000") == 0);
    CHECK(tui::parse_osc_hex_channel("8000") == 128);
    // Single-digit channels scale over 0xf.
    CHECK(tui::parse_osc_hex_channel("8") == 136);

    CHECK_FALSE(tui::parse_osc_hex_channel("").has_value());
    CHECK_FALSE(tui::parse_osc_hex_channel("0g").has_value());
    CHECK_FALSE(tui::parse_osc_hex_channel("000000000").has_value());
}

TEST_CASE("OSC color values parse every supported form", "[tui][theme][issue990][spec]") {
    const auto short_hex = tui::parse_osc_color_value("#ff0000");
    REQUIRE(short_hex.has_value());
    CHECK(*short_hex == tui::RgbColor{.red = 255, .green = 0, .blue = 0});

    const auto long_hex = tui::parse_osc_color_value("#ffff00000000");
    REQUIRE(long_hex.has_value());
    CHECK(*long_hex == tui::RgbColor{.red = 255, .green = 0, .blue = 0});

    const auto rgb = tui::parse_osc_color_value("rgb:ffff/ffff/ffff");
    REQUIRE(rgb.has_value());
    CHECK(*rgb == tui::RgbColor{.red = 255, .green = 255, .blue = 255});

    // rgba: shape carries an alpha field pi ignores.
    const auto rgba = tui::parse_osc_color_value("rgba:f/0/0/f");
    REQUIRE(rgba.has_value());
    CHECK(*rgba == tui::RgbColor{.red = 255, .green = 0, .blue = 0});

    // Surrounding whitespace is trimmed before parsing.
    const auto padded = tui::parse_osc_color_value("  #010203\t");
    REQUIRE(padded.has_value());
    CHECK(*padded == tui::RgbColor{.red = 1, .green = 2, .blue = 3});

    CHECK_FALSE(tui::parse_osc_color_value("").has_value());
    CHECK_FALSE(tui::parse_osc_color_value("not-a-color").has_value());
    CHECK_FALSE(tui::parse_osc_color_value("#12345").has_value());
    CHECK_FALSE(tui::parse_osc_color_value("#1234567890").has_value());
    CHECK_FALSE(tui::parse_osc_color_value("rgb:ff/00").has_value());
}

TEST_CASE("OSC color responses demux foreground, background, and palette targets", "[tui][theme][issue990][spec]") {
    const auto foreground = tui::parse_osc_color_response("\x1b]10;#010203\x07");
    REQUIRE(foreground.has_value());
    CHECK(foreground->target.kind == tui::OscColorTargetKind::Foreground);
    REQUIRE(foreground->rgb.has_value());
    CHECK(*foreground->rgb == tui::RgbColor{.red = 1, .green = 2, .blue = 3});

    // The ST terminator is equivalent to BEL.
    const auto background = tui::parse_osc_color_response("\x1b]11;rgb:ffff/ffff/ffff\x1b\\");
    REQUIRE(background.has_value());
    CHECK(background->target.kind == tui::OscColorTargetKind::Background);
    REQUIRE(background->rgb.has_value());
    CHECK(*background->rgb == tui::RgbColor{.red = 255, .green = 255, .blue = 255});

    const auto palette = tui::parse_osc_color_response("\x1b]4;3;#0000ff\x07");
    REQUIRE(palette.has_value());
    CHECK(palette->target.kind == tui::OscColorTargetKind::Palette);
    CHECK(palette->target.palette_index == 3);
    REQUIRE(palette->rgb.has_value());
    CHECK(*palette->rgb == tui::RgbColor{.red = 0, .green = 0, .blue = 255});

    // A well-formed reply with an unparseable color still counts as a target
    // reply: the target survives, the color does not.
    const auto unparseable = tui::parse_osc_color_response("\x1b]11;not-a-color\x07");
    REQUIRE(unparseable.has_value());
    CHECK(unparseable->target.kind == tui::OscColorTargetKind::Background);
    CHECK_FALSE(unparseable->rgb.has_value());

    // Out-of-range palette targets parse with their index; the consumer drops
    // them from the result.
    const auto out_of_range = tui::parse_osc_color_response("\x1b]4;99;#ffffff\x07");
    REQUIRE(out_of_range.has_value());
    CHECK(out_of_range->target.palette_index == 99);

    CHECK_FALSE(tui::parse_osc_color_response("\x1b]10;#010203").has_value());
    CHECK_FALSE(tui::parse_osc_color_response("\x1b]12;#010203\x07").has_value());
    CHECK_FALSE(tui::parse_osc_color_response("\x1b[?997;1n").has_value());
    CHECK_FALSE(tui::parse_osc_color_response("\x1b]4;#010203\x07").has_value());
}

TEST_CASE("terminal color scheme reports parse single and concatenated reports", "[tui][theme][issue990][spec]") {
    CHECK(tui::parse_terminal_color_scheme_report("\x1b[?997;1n") == tui::TerminalColorScheme::Dark);
    CHECK(tui::parse_terminal_color_scheme_report("\x1b[?997;2n") == tui::TerminalColorScheme::Light);
    // Concatenated reports keep the last valid value.
    CHECK(tui::parse_terminal_color_scheme_report("\x1b[?997;1n\x1b[?997;2n") == tui::TerminalColorScheme::Light);
    CHECK(tui::parse_terminal_color_scheme_report("\x1b[?997;2n\x1b[?997;1n") == tui::TerminalColorScheme::Dark);

    CHECK_FALSE(tui::parse_terminal_color_scheme_report("").has_value());
    CHECK_FALSE(tui::parse_terminal_color_scheme_report("\x1b[?997;3n").has_value());
    CHECK_FALSE(tui::parse_terminal_color_scheme_report("\x1b[?997;1n\x1b[?997;9n").has_value());
}

TEST_CASE("query burst asks for foreground, background, sixteen palette entries, and DA1",
        "[tui][theme][issue990][spec]") {
    const auto query = tui::build_terminal_color_query();
    std::string expected = "\x1b]10;?\x07\x1b]11;?\x07";
    for (int index = 0; index < 16; ++index) {
        expected += "\x1b]4;" + std::to_string(index) + ";?\x07";
    }
    expected += "\x1b[c";
    CHECK(query == expected);
}

namespace {

/// Records the keys the Tui actually delivers as typed input, so a test can
/// prove terminal replies never reach a component.
class KeyRecordingComponent final : public tui::Component, public tui::InputHandler, public tui::Focusable {
public:
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = {}};
    }

    void invalidate() override {}

    tui::InputAdmissionOutcome handle_input(const tui::InputEventVariant& event) override {
        const auto* key = std::get_if<tui::KeyEvent>(&event);
        if (key == nullptr || key->type != tui::KeyEventType::Press) return tui::InputAdmissionOutcome::Unhandled;
        keys.push_back(key->key);
        return tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::vector<std::string> keys;

private:
    bool focused_{false};
};

/// A started Tui over a VirtualTerminal double: every byte it writes is
/// recorded in `output()`, and `inject_input` drives the real input pipeline.
class ColorQueryHost {
public:
    ColorQueryHost() : terminal_({.columns = 40, .rows = 6}), tui_(terminal_) { REQUIRE(tui_.start()); }
    ~ColorQueryHost() { (void)tui_.stop(); }

    ColorQueryHost(const ColorQueryHost&) = delete;
    ColorQueryHost& operator=(const ColorQueryHost&) = delete;

    [[nodiscard]] tui::Tui& tui() noexcept { return tui_; }
    [[nodiscard]] tui::VirtualTerminal& terminal() noexcept { return terminal_; }

    [[nodiscard]] support::ExpectedVoid type(const std::string& bytes) { return terminal_.inject_input(bytes); }

    [[nodiscard]] std::string written() const {
        std::string all;
        for (const auto& chunk : terminal_.output())
            all += chunk;
        return all;
    }

private:
    tui::VirtualTerminal terminal_;
    tui::Tui tui_;
};

} // namespace

TEST_CASE("query completes when every color target replies", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    CHECK(host.written().empty());
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});
    // The exact single request burst pi emits: OSC 10, OSC 11, OSC 4;0..15, DA1.
    CHECK(host.written() == tui::build_terminal_color_query());

    // Replies may arrive split across reads and each target counts once.
    REQUIRE(host.type("\x1b]10;#010203\x07"));
    REQUIRE(host.type("\x1b]11;#040506\x07"));
    for (int index = 0; index < 16; ++index) {
        REQUIRE(host.type("\x1b]4;" + std::to_string(index) + ";#0" + std::to_string(index % 10) + "0000\x07"));
    }

    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    const auto colors = future.get();
    REQUIRE(colors.foreground.has_value());
    CHECK(*colors.foreground == tui::RgbColor{.red = 1, .green = 2, .blue = 3});
    REQUIRE(colors.background.has_value());
    CHECK(*colors.background == tui::RgbColor{.red = 4, .green = 5, .blue = 6});
    // The palette is exposed only when all sixteen entries arrived.
    REQUIRE(colors.palette.has_value());
    REQUIRE(colors.palette->size() == 16);
    CHECK(colors.palette->at(0) == tui::RgbColor{.red = 0, .green = 0, .blue = 0});
    CHECK(colors.palette->at(5) == tui::RgbColor{.red = 5, .green = 0, .blue = 0});
    CHECK(colors.palette->at(15) == tui::RgbColor{.red = 5, .green = 0, .blue = 0});
}

TEST_CASE("a DA1 reply completes a query that ignored the color requests", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});

    // Terminals that do not answer OSC color queries still answer DA1, which
    // is the end-of-replies sentinel.
    REQUIRE(host.type("\x1b]10;#010203\x07\x1b[?62;c"));

    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    const auto colors = future.get();
    REQUIRE(colors.foreground.has_value());
    CHECK(*colors.foreground == tui::RgbColor{.red = 1, .green = 2, .blue = 3});
    CHECK_FALSE(colors.background.has_value());
    CHECK_FALSE(colors.palette.has_value());
}

TEST_CASE("a partial palette is never exposed", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});

    REQUIRE(host.type("\x1b]4;0;#010203\x07\x1b]4;1;#040506\x07\x1b[?1;2c"));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    const auto colors = future.get();
    CHECK_FALSE(colors.palette.has_value());
}

TEST_CASE("duplicate replies do not resolve a query early", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});

    // Eighteen replies of the background target only: every duplicate is
    // counted once, so the query stays pending.
    for (int index = 0; index < 18; ++index) {
        REQUIRE(host.type("\x1b]11;#010203\x07"));
    }
    CHECK(future.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

    REQUIRE(host.type("\x1b[?1;2c"));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    const auto colors = future.get();
    REQUIRE(colors.background.has_value());
    CHECK_FALSE(colors.palette.has_value());
}

TEST_CASE(
        "a timed-out query resolves partial results and later replies advance state", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    std::atomic<int> complete_calls{0};
    std::atomic<int> late_calls{0};
    tui::TerminalColors late_colors;

    auto future = host.tui().query_terminal_colors(tui::TerminalColorsQueryOptions{
            .timeout = std::chrono::milliseconds(30),
            .on_complete =
                    [&](tui::TerminalColors colors) {
                        ++complete_calls;
                        CHECK_FALSE(colors.background.has_value());
                    },
            .on_late_reply =
                    [&](tui::TerminalColors colors) {
                        ++late_calls;
                        late_colors = colors;
                    },
    });

    // One reply before the deadline: the timeout resolves the partial result.
    REQUIRE(host.type("\x1b]10;#010203\x07"));
    const auto partial = future.get();
    CHECK(complete_calls.load() == 1);
    REQUIRE(partial.foreground.has_value());
    CHECK(*partial.foreground == tui::RgbColor{.red = 1, .green = 2, .blue = 3});
    CHECK_FALSE(partial.background.has_value());

    // A late background reply updates observable state through the late
    // callback instead of freezing the partial startup fallback.
    REQUIRE(host.type("\x1b]11;#040506\x07"));
    CHECK(late_calls.load() >= 1);
    REQUIRE(late_colors.background.has_value());
    CHECK(*late_colors.background == tui::RgbColor{.red = 4, .green = 5, .blue = 6});
    CHECK(late_colors.foreground.has_value());
}

TEST_CASE("a late color callback can stop the Tui without deadlocking", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    std::atomic<int> late_calls{0};
    std::atomic<bool> stopped{false};
    auto future = host.tui().query_terminal_colors(tui::TerminalColorsQueryOptions{
            .timeout = std::chrono::milliseconds(30),
            .on_late_reply =
                    [&](tui::TerminalColors) {
                        ++late_calls;
                        stopped.store(static_cast<bool>(host.tui().stop()));
                    },
    });

    REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    (void)future.get();
    REQUIRE(host.type("\x1b]11;#040506\x07"));
    CHECK(late_calls.load() == 1);
    CHECK(stopped.load());
}

TEST_CASE("stopping the Tui cancels pending query callbacks", "[tui][theme][issue990][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 6});
    tui::Tui tui(terminal);
    REQUIRE(tui.start());

    std::atomic<int> complete_calls{0};
    std::atomic<int> late_calls{0};
    auto future = tui.query_terminal_colors(tui::TerminalColorsQueryOptions{
            .timeout = std::chrono::milliseconds(30),
            .on_complete = [&](tui::TerminalColors) { ++complete_calls; },
            .on_late_reply = [&](tui::TerminalColors) { ++late_calls; },
    });

    REQUIRE(tui.stop());
    // stop() resolves the pending promise deterministically without invoking
    // the query callbacks into a stopped TUI.
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    CHECK(future.get() == tui::TerminalColors{});
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(complete_calls.load() == 0);
    CHECK(late_calls.load() == 0);

    // A stale reply after the restart must not touch the old query.
    REQUIRE(tui.start());
    REQUIRE(terminal.inject_input("\x1b]11;#010203\x07"));
    CHECK(late_calls.load() == 0);
    REQUIRE(tui.stop());
}

TEST_CASE("Renderer handoff preserves query deadline partial completion and rejects stale input",
        "[tui][theme][issue1037][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 6});
    std::unique_ptr<tui::ITui> root;
    SECTION("Regular root") { root = std::make_unique<tui::Tui>(terminal); }
    SECTION("Fullscreen root") { root = std::make_unique<tui::AltScreen>(terminal); }
    REQUIRE(root->start());
    std::atomic<int> complete{0};
    std::atomic<int> late{0};
    std::promise<void> delivered;
    auto delivered_future = delivered.get_future();
    auto result = root->query_terminal_colors(tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds{600},
            .on_complete =
                    [&](tui::TerminalColors) {
                        ++complete;
                        delivered.set_value();
                    },
            .on_late_reply = [&](tui::TerminalColors) { ++late; }});
    REQUIRE(terminal.inject_input("\x1b]11;#112233\x07"));
    // Observe another actual query's completion before handoff. Restarting
    // the first deadline on handoff would leave it pending after marker2.
    auto marker1 =
            root->query_terminal_colors(tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds{400}});
    REQUIRE(marker1.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(root->stop({.preserve_screen = true, .preserve_pending_color_query_completion = true}));
    CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
    REQUIRE(root->start());
    REQUIRE(terminal.inject_input("\x1b]11;#ffffff\x07"));
    REQUIRE(terminal.inject_input("\x1b[?1;2c"));
    auto marker2 =
            root->query_terminal_colors(tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds{400}});
    REQUIRE(marker2.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
    CHECK(result.get().background == tui::RgbColor{.red = 17, .green = 34, .blue = 51});
    REQUIRE(delivered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(complete.load() == 1);
    CHECK(late.load() == 0);
    REQUIRE(root->stop());
}

TEST_CASE("Renderer handoff completion outlives root and never duplicates completed queries",
        "[tui][theme][issue1037][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 6});
    std::atomic<int> completed{0};
    std::promise<void> delivered;
    auto delivered_future = delivered.get_future();
    std::future<tui::TerminalColors> pending;
    {
        tui::Tui root{terminal};
        REQUIRE(root.start());
        auto done = root.query_terminal_colors(tui::TerminalColorsQueryOptions{
                .timeout = std::chrono::milliseconds{100}, .on_complete = [&](tui::TerminalColors) { ++completed; }});
        REQUIRE(terminal.inject_input("\x1b[?1;2c"));
        REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
        pending = root.query_terminal_colors(tui::TerminalColorsQueryOptions{
                .timeout = std::chrono::milliseconds{150}, .on_complete = [&](tui::TerminalColors) {
                    ++completed;
                    delivered.set_value();
                }});
        REQUIRE(root.stop({.preserve_pending_color_query_completion = true}));
    }
    REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    REQUIRE(delivered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(completed.load() == 2);
}

TEST_CASE("scheme notifications toggle mode 2031 while running and on start/stop", "[tui][theme][issue990][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 6});
    tui::Tui tui(terminal);

    // Enabled before start: start() emits the enable sequence once.
    tui.set_terminal_color_scheme_notifications(true);
    REQUIRE(tui.start());
    const auto started_output = [&] {
        std::string all;
        for (const auto& chunk : terminal.output())
            all += chunk;
        return all;
    }();
    CHECK(started_output.find("\x1b[?2031h") != std::string::npos);

    const auto output_size_before = terminal.output().size();
    tui.set_terminal_color_scheme_notifications(true);
    CHECK(terminal.output().size() == output_size_before); // idempotent

    tui.set_terminal_color_scheme_notifications(false);
    const auto disabled_output = [&] {
        std::string all;
        for (std::size_t index = output_size_before; index < terminal.output().size(); ++index)
            all += terminal.output()[index];
        return all;
    }();
    CHECK(disabled_output.find("\x1b[?2031l") != std::string::npos);

    // Re-enabled before stop: stop() emits the disable sequence.
    tui.set_terminal_color_scheme_notifications(true);
    const auto output_size_mid = terminal.output().size();
    REQUIRE(tui.stop());
    const auto stopped_output = [&] {
        std::string all;
        for (std::size_t index = output_size_mid; index < terminal.output().size(); ++index)
            all += terminal.output()[index];
        return all;
    }();
    CHECK(stopped_output.find("\x1b[?2031l") != std::string::npos);
}

TEST_CASE("scheme listener handles remain safe after the TUI root is destroyed", "[tui][theme][issue990]") {
    auto host = std::make_unique<ColorQueryHost>();
    auto handle = host->tui().on_terminal_color_scheme_change([](tui::TerminalColorScheme) {});
    REQUIRE(handle.active());

    host.reset();
    CHECK(handle.active());
    handle.reset();
    CHECK_FALSE(handle.active());
}

TEST_CASE("scheme listeners fire per report and unsubscribe by handle", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    std::vector<tui::TerminalColorScheme> observed;
    auto handle = host.tui().on_terminal_color_scheme_change(
            [&](tui::TerminalColorScheme scheme) { observed.push_back(scheme); });
    CHECK(handle.active());

    // Concatenated reports each fire the listener.
    REQUIRE(host.type("\x1b[?997;1n\x1b[?997;2n"));
    REQUIRE(observed == std::vector<tui::TerminalColorScheme>{
                                tui::TerminalColorScheme::Dark,
                                tui::TerminalColorScheme::Light,
                        });

    handle.reset();
    CHECK_FALSE(handle.active());
    REQUIRE(host.type("\x1b[?997;1n"));
    CHECK(observed.size() == 2);

    // Reports never reach a component's typed input, and typing mixed with a
    // report and color replies still resolves the pending query.
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});
    REQUIRE(host.type("abc\x1b[?997;2n\x1b]11;#040506\x07\x1b[?1;2c"));
    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    const auto colors = future.get();
    REQUIRE(colors.background.has_value());
    CHECK(*colors.background == tui::RgbColor{.red = 4, .green = 5, .blue = 6});
    // The report after unsubscription still fires no listener.
    CHECK(observed.size() == 2);
}

TEST_CASE("color and scheme replies never reach a component's typed input", "[tui][theme][issue990][spec]") {
    ColorQueryHost host;
    auto component = std::make_unique<KeyRecordingComponent>();
    auto* component_ptr = component.get();
    REQUIRE(host.tui().add_child(std::move(component)));
    REQUIRE(host.tui().set_focus(component_ptr));

    // One read mixes ordinary typing, a scheme report, OSC default-color
    // replies, and the DA1 sentinel that ends the reply burst.
    auto future = host.tui().query_terminal_colors(
            tui::TerminalColorsQueryOptions{.timeout = std::chrono::milliseconds(5000)});
    REQUIRE(host.type("ab\x1b]10;#010203\x07\x1b[?997;1n"
                      "c\x1b]11;#040506\x07\x1b[?1;2c"));

    REQUIRE(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    // The component saw exactly the typed letters: no reply bytes and no
    // escape-sequence fragments were delivered as typed input.
    CHECK(component_ptr->keys == std::vector<std::string>{"a", "b", "c"});
    const auto colors = future.get();
    REQUIRE(colors.foreground.has_value());
    CHECK(*colors.foreground == tui::RgbColor{.red = 1, .green = 2, .blue = 3});
    REQUIRE(colors.background.has_value());
    CHECK(*colors.background == tui::RgbColor{.red = 4, .green = 5, .blue = 6});
}
