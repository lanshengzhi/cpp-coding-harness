// The pi `InteractiveThemeController` subset (G5): the frozen v1.0.4
// COLORFGBG basic-16 rule, `resolveThemeSetting` with automatic pairs,
// `applyThemeName` fallback semantics with the verbatim failure message,
// `applyFromSettings` with the terminal color query (timeout, late reply,
// live mode-2031 reports, and the automatic/defined precedence), in-memory
// `preview`, registered themes + `discover_themes` dedupe with pi's
// collision diagnostics, and the boot theme init (`init_boot_theme`).

#include "coding_agent/tui/ThemeController.hpp"
#include "coding_agent/tui/theme_color_math.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/ProtocolRecordingTerminal.hpp"
#include "support/ThemeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

class InvalidatingText final : public tui::Component {
public:
    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = {"base"}};
    }

    void invalidate() override {
        ++invalidations;
    }

    std::size_t invalidations{0};
};

/// One report the TUI delivered to the host hook (the interactive engine
/// posts it to the executor; the harness records it and `pump` applies it).
struct ThemeReport {
    std::optional<tui::TerminalColors> colors;
    std::optional<coding_agent::tui::TerminalTheme> scheme;
};

struct ControllerHarness {
    tui::VirtualTerminal terminal{{.columns = 40, .rows = 6}};
    tui::Tui root;
    std::unique_ptr<InvalidatingText> base;
    InvalidatingText* base_pointer{nullptr};
    std::vector<std::string> errors;
    std::size_t changed{0};
    bool started{false};
    mutable std::mutex reports_mutex;
    std::vector<ThemeReport> reports;

    ControllerHarness()
        : root(terminal) {
        base = std::make_unique<InvalidatingText>();
        base_pointer = base.get();
        REQUIRE(root.add_child(std::move(base)));
        REQUIRE(root.start());
        started = true;
    }

    ControllerHarness(const ControllerHarness&) = delete;
    ControllerHarness& operator=(const ControllerHarness&) = delete;

    ~ControllerHarness() {
        if (started) (void)root.stop();
    }

    /// Everything the terminal received so far (the color query writes its
    /// OSC 10/11/4 + DA1 burst here, and mode 2031 toggles are visible).
    [[nodiscard]] std::string written() const {
        std::string output;
        for (const auto& chunk : terminal.output())
            output += chunk;
        return output;
    }

    void record_colors(tui::TerminalColors colors) {
        std::lock_guard lock(reports_mutex);
        reports.push_back(ThemeReport{.colors = std::move(colors), .scheme = std::nullopt});
    }

    void record_scheme(coding_agent::tui::TerminalTheme scheme) {
        std::lock_guard lock(reports_mutex);
        reports.push_back(ThemeReport{.colors = std::nullopt, .scheme = scheme});
    }

    [[nodiscard]] std::size_t report_count() const {
        std::lock_guard lock(reports_mutex);
        return reports.size();
    }

    /// Apply every report the TUI thread delivered, in order (the host's
    /// executor hop), and return how many were applied.
    [[nodiscard]] std::size_t pump(coding_agent::tui::ThemeController& controller) {
        std::vector<ThemeReport> pending;
        {
            std::lock_guard lock(reports_mutex);
            pending.swap(reports);
        }
        for (auto& report : pending) {
            if (report.colors) {
                controller.apply_terminal_colors(std::move(*report.colors));
            } else if (report.scheme) {
                controller.apply_terminal_color_scheme(*report.scheme);
            }
        }
        return pending.size();
    }
};

/// The number of OSC 10/11 + DA1 query bursts the terminal received.
[[nodiscard]] std::size_t count_query_bursts(const std::string& output) {
    static constexpr std::string_view kQuery{"\x1b]10;?\x07\x1b]11;?\x07"};
    std::size_t count = 0;
    for (auto at = output.find(kQuery); at != std::string::npos; at = output.find(kQuery, at + kQuery.size())) {
        ++count;
    }
    return count;
}

/// Wait for the 100 ms query timeout to deliver its report.
[[nodiscard]] bool wait_for_reports(const ControllerHarness& harness, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (harness.report_count() < count) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return true;
}

[[nodiscard]] coding_agent::tui::ThemeController make_controller(ControllerHarness& harness,
        std::filesystem::path custom_themes_dir = {},
        std::vector<coding_agent::tui::RegisteredTheme> registered = {},
        std::optional<std::string> theme_setting = std::nullopt) {
    return coding_agent::tui::ThemeController(
            std::move(custom_themes_dir),
            std::move(registered),
            [theme_setting = std::move(theme_setting)]() mutable { return theme_setting; },
            tui::TerminalColorCapability::TrueColor,
            harness.root,
            [&harness](std::string message) { harness.errors.push_back(std::move(message)); },
            [&harness] { ++harness.changed; },
            [&harness](tui::TerminalColors colors) { harness.record_colors(std::move(colors)); },
            [&harness](coding_agent::tui::TerminalTheme scheme) { harness.record_scheme(scheme); });
}

[[nodiscard]] coding_agent::tui::RgbThemeColor accent_of(
    const coding_agent::tui::ResolvedTheme& theme) {
    const auto* color = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(theme, coding_agent::tui::ThemeToken::Accent));
    REQUIRE(color != nullptr);
    return *color;
}

} // namespace

TEST_CASE("Real roots deliver frozen theme RGB and ordered rebind query phases",
        "[coding_agent][theme][controller][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-theme-rebind-protocol.json");
    REQUIRE(artifact);
    tests::EnvVarGuard env("COLORFGBG", "0;15");
    const auto scenario = [&](std::string_view name) -> const support::JsonValue& {
        for (const auto& value : artifact->at("scenarios").get_array()) {
            if (value.at("name").get_string() == name) return value;
        }
        REQUIRE(false);
        return *artifact;
    };
    const auto commands_of = [](const support::JsonValue& value) {
        std::vector<std::string> commands;
        for (const auto& entry : value.get_array())
            commands.emplace_back(entry.get_string());
        return commands;
    };
    const auto rgb_of = [](const support::JsonValue& value) {
        return tui::RgbColor{.red = value.at("r").get_number(),
                .green = value.at("g").get_number(),
                .blue = value.at("b").get_number()};
    };

    bool to_fullscreen = true;
    SECTION("Regular to fullscreen") { to_fullscreen = true; }
    SECTION("Fullscreen to regular") { to_fullscreen = false; }
    const auto& frozen =
            scenario(to_fullscreen ? "direction_a_regular_to_fullscreen" : "direction_b_fullscreen_to_regular");
    const auto& expected = frozen.at("expected");
    const auto& inputs = frozen.at("inputs");
    tui::VirtualTerminal terminal{{.columns = 80, .rows = 24}};
    tui::Tui regular{terminal};
    tui::AltScreen fullscreen{terminal};
    tui::ITui& old_root = to_fullscreen ? static_cast<tui::ITui&>(regular) : static_cast<tui::ITui&>(fullscreen);
    tui::ITui& active_root = to_fullscreen ? static_cast<tui::ITui&>(fullscreen) : static_cast<tui::ITui&>(regular);
    REQUIRE(old_root.start());
    std::vector<std::string> phases;
    std::vector<tui::TerminalColors> colors;
    std::mutex pending_mutex;
    std::vector<ThemeReport> pending;
    auto controller = coding_agent::tui::ThemeController(
            {},
            {},
            [] { return std::optional<std::string>{"system"}; },
            tui::TerminalColorCapability::TrueColor,
            old_root,
            [](std::string error) { FAIL(error); },
            [&] { phases.emplace_back("changed"); },
            [&](tui::TerminalColors report) {
                std::lock_guard lock(pending_mutex);
                pending.push_back(ThemeReport{.colors = std::move(report), .scheme = std::nullopt});
            },
            [&](coding_agent::tui::TerminalTheme scheme) {
                std::lock_guard lock(pending_mutex);
                pending.push_back(ThemeReport{.colors = std::nullopt, .scheme = scheme});
            });
    // Like the engine's executor hop, sinks only enqueue; controller state
    // and phase observations are applied on the test's owning thread.
    const auto pump = [&] {
        std::vector<ThemeReport> reports;
        {
            std::lock_guard lock(pending_mutex);
            reports.swap(pending);
        }
        for (auto& report : reports) {
            if (report.colors) {
                phases.emplace_back("colors");
                colors.push_back(*report.colors);
                controller.apply_terminal_colors(std::move(*report.colors));
            } else if (report.scheme) {
                phases.emplace_back("scheme");
                controller.apply_terminal_color_scheme(*report.scheme);
            }
        }
    };
    controller.apply_from_settings();
    CHECK(tests::extract_theme_protocol_commands(terminal.output()) ==
            commands_of(scenario("same_root_late_query").at("expected").at("initialQueryCommands")));
    const auto before_switch = terminal.output().size();
    REQUIRE(old_root.stop({.preserve_screen = true}));
    REQUIRE(active_root.start());
    controller.rebind_tui(active_root);
    CHECK(tests::extract_theme_protocol_commands(
                  std::span<const std::string>{terminal.output()}.subspan(before_switch)) ==
            commands_of(expected.at(to_fullscreen ? "phaseA2SwitchCommands" : "phaseB2SwitchCommands")));
    phases.clear();
    const auto reply = [&](std::string_view target, const std::string& hex) {
        REQUIRE(terminal.inject_input("\x1b]" + std::string{target} + ";" + hex + "\x07"));
    };
    reply("10", inputs.at("oldFg").get_string());
    reply("11", inputs.at("oldBg").get_string());
    REQUIRE(terminal.inject_input("\x1b[?1;2c"));
    pump();
    CHECK(colors.empty());
    CHECK(phases.empty());
    CHECK(controller.active_theme_name() == "light");
    // Repeated listener rebinds must still deliver one scheme callback and burst.
    controller.rebind_terminal_color_scheme_listener();
    controller.rebind_terminal_color_scheme_listener();
    controller.rebind_terminal_color_scheme_listener();
    const auto before_scheme = terminal.output().size();
    REQUIRE(terminal.inject_input(to_fullscreen ? "\x1b[?997;2n" : "\x1b[?997;1n"));
    pump();
    CHECK(phases ==
            (to_fullscreen ? std::vector<std::string>{"scheme"} : std::vector<std::string>{"scheme", "changed"}));
    CHECK(tests::extract_theme_protocol_commands(
                  std::span<const std::string>{terminal.output()}.subspan(before_scheme)) ==
            commands_of(expected.at(to_fullscreen ? "phaseA5QueryCommands" : "phaseB5QueryCommands")));
    phases.emplace_back("query-written");
    reply("10", inputs.at("newFg").get_string());
    reply("11", inputs.at("newBg").get_string());
    pump();
    CHECK(colors.empty());
    REQUIRE(terminal.inject_input("\x1b[?1;2c"));
    pump();
    REQUIRE(colors.size() == 1);
    CHECK(colors[0].foreground == rgb_of(expected.at("settledColors").at("foreground")));
    CHECK(colors[0].background == rgb_of(expected.at("settledColors").at("background")));
    CHECK_FALSE(colors[0].palette.has_value());
    CHECK(phases ==
            (to_fullscreen ? std::vector<std::string>{"scheme", "query-written", "colors", "changed"}
                           : std::vector<std::string>{"scheme", "changed", "query-written", "colors", "changed"}));
    CHECK(controller.active_theme_name() == expected.at("settledTheme").get_string());
    controller.dispose();
    REQUIRE(active_root.stop());
}

TEST_CASE("Real theme query sink distinguishes late FIFO and malformed frozen replies",
        "[coding_agent][theme][controller][issue1037][spec]") {
    const auto artifact = tests::read_pi_tui_evidence("product-theme-rebind-protocol.json");
    REQUIRE(artifact);
    const auto scenario = [&](std::string_view name) -> const support::JsonValue& {
        for (const auto& entry : artifact->at("scenarios").get_array()) {
            if (entry.at("name").get_string() == name) return entry;
        }
        REQUIRE(false);
        return *artifact;
    };
    std::string name;
    SECTION("Same root late reply") { name = "same_root_late_query"; }
    SECTION("Pending queries consume unkeyed replies FIFO") { name = "unkeyed_reply_pending_target_fifo"; }
    SECTION("Malformed controls leave completion pending") { name = "malformed_and_unsupported_controls"; }
    const auto& frozen = scenario(name);
    const auto& inputs = frozen.at("inputs");
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", "0;15");
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"system"});
    controller.apply_from_settings();
    CHECK(controller.active_theme_name() == "light");
    if (name == "same_root_late_query") {
        REQUIRE(wait_for_reports(harness, 1));
        {
            std::lock_guard lock(harness.reports_mutex);
            REQUIRE(harness.reports.size() == 1);
            CHECK(harness.reports[0].colors == tui::TerminalColors{});
        }
        CHECK(harness.pump(controller) == 1);
    } else if (name == "unkeyed_reply_pending_target_fifo") {
        // Two pending requests: one DA1 resolves only the front request.
        controller.apply_from_settings();
    } else {
        REQUIRE(harness.terminal.inject_input("\x1b]10;rgb:invalid\x07\x1b]12;rgb:00/00/00\x07\x1b[24;80R"));
        CHECK(harness.report_count() == 0);
    }
    const auto reply = [&](std::string_view target, const std::string& value) {
        REQUIRE(harness.terminal.inject_input("\x1b]" + std::string{target} + ";" + value + "\x07"));
    };
    if (name == "same_root_late_query") reply("10", inputs.at("lateFg").get_string());
    if (name == "unkeyed_reply_pending_target_fifo") reply("10", inputs.at("unkeyedFg").get_string());
    reply("11",
            inputs.at(name == "same_root_late_query"                    ? "lateBg"
                          : name == "unkeyed_reply_pending_target_fifo" ? "unkeyedBg"
                                                                        : "validBg")
                    .get_string());
    if (name != "same_root_late_query") CHECK(harness.report_count() == 0);
    REQUIRE(harness.terminal.inject_input("\x1b[?1;2c"));
    {
        std::lock_guard lock(harness.reports_mutex);
        REQUIRE_FALSE(harness.reports.empty());
        const auto& actual = harness.reports.back().colors;
        REQUIRE(actual.has_value());
        const auto& expected = frozen.at("expected").at("settledColors");
        const auto check_rgb = [](const std::optional<tui::RgbColor>& rgb, const support::JsonValue& value) {
            REQUIRE(rgb.has_value());
            CHECK(rgb->red == value.at("r").get_number());
            CHECK(rgb->green == value.at("g").get_number());
            CHECK(rgb->blue == value.at("b").get_number());
        };
        check_rgb(actual->background, expected.at("background"));
        if (name == "malformed_and_unsupported_controls")
            CHECK_FALSE(actual->foreground.has_value());
        else
            check_rgb(actual->foreground, expected.at("foreground"));
        CHECK_FALSE(actual->palette.has_value());
        if (name != "same_root_late_query") CHECK(harness.reports.size() == 1);
    }
    (void)harness.pump(controller);
    CHECK(controller.active_theme_name() == "dark");
    if (name == "unkeyed_reply_pending_target_fifo") {
        // If the first DA1 consumed both requests, these second replies
        // would be dropped and the appearance would remain dark.
        reply("11", "#eeeeee");
        CHECK(harness.report_count() == 0);
        REQUIRE(harness.terminal.inject_input("\x1b[?1;2c"));
        CHECK(harness.report_count() == 1);
        (void)harness.pump(controller);
        CHECK(controller.active_theme_name() == "light");
    }
}

TEST_CASE("COLORFGBG detection classifies the basic-16 background index",
        "[coding_agent][theme][controller][issue991][spec]") {
    using coding_agent::tui::terminal_theme_from_colorfgbg;
    using coding_agent::tui::TerminalTheme;
    CHECK(terminal_theme_from_colorfgbg("0;15") == TerminalTheme::Light);
    CHECK(terminal_theme_from_colorfgbg("15;0") == TerminalTheme::Dark);

    // The last COLORFGBG field is the background (pi `split(";").at(-1)`).
    CHECK(terminal_theme_from_colorfgbg("0;7;15") == TerminalTheme::Light);

    // pi `detectColorFgBgTheme`: 0..6 and 8 are dark, 7 and 9..15 light.
    CHECK(terminal_theme_from_colorfgbg("0;8") == TerminalTheme::Dark);
    CHECK(terminal_theme_from_colorfgbg("0;7") == TerminalTheme::Light);
    CHECK(terminal_theme_from_colorfgbg("0;9") == TerminalTheme::Light);
    // Anything but a one- or two-digit index in 0..15 is undefined, so the
    // reported scheme and the dark fallback decide instead.
    for (const auto* value : {"", "abc", "0;16", "0;-1", "0;007"}) {
        CHECK_FALSE(terminal_theme_from_colorfgbg(value).has_value());
    }
    // The last field is trimmed like pi's `.at(-1)?.trim()`.
    CHECK(terminal_theme_from_colorfgbg("0; 7") == TerminalTheme::Light);
}

TEST_CASE("resolve_theme_setting reads automatic pairs and rejects malformed slash values",
        "[coding_agent][theme][controller][issue991][spec]") {
    using coding_agent::tui::TerminalTheme;
    using coding_agent::tui::resolve_theme_setting;

    CHECK(resolve_theme_setting(std::optional<std::string>{"dark"}, TerminalTheme::Light) ==
        std::optional<std::string>{"dark"});
    // pi `resolveThemeSetting`: a `light/dark` pair selects by terminal appearance.
    CHECK(resolve_theme_setting(std::optional<std::string>{"light/dark"}, TerminalTheme::Dark) ==
            std::optional<std::string>{"dark"});
    CHECK(resolve_theme_setting(std::optional<std::string>{"light/dark"}, TerminalTheme::Light) ==
            std::optional<std::string>{"light"});
    // pi `parseAutoThemeSetting` trims the pair halves.
    CHECK(resolve_theme_setting(std::optional<std::string>{" light / dark "}, TerminalTheme::Dark) ==
            std::optional<std::string>{"dark"});
    // More than one slash, or an empty half, is a malformed pair and reads as unset.
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{"light/dark/extra"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{"/dark"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(std::optional<std::string>{"light/"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{}, TerminalTheme::Dark).has_value());
}

TEST_CASE("apply_from_settings resolves the setting, queries colors, and syncs only automatic values",
        "[coding_agent][theme][controller][issue991][spec]") {
    ControllerHarness harness;

    {
        // A defined choice applies and stays: pi `applyFromSettings` does not
        // write the setting back, and mode 2031 stays off.
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});
        const auto before = harness.written().size();
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        CHECK(harness.errors.empty());
        CHECK(harness.written().find("\x1b[?2031h") == std::string::npos);
        // The query burst is written even though the choice is explicit.
        CHECK(harness.written().substr(before).starts_with("\x1b]10;?\x07\x1b]11;?\x07"));
    }
    {
        // Unset setting: the COLORFGBG default applies and auto-sync turns on.
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        auto controller = make_controller(harness);
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "light");
        CHECK(harness.written().find("\x1b[?2031h") != std::string::npos);
    }
    {
        // No COLORFGBG: the dark fallback applies, still with auto-sync on.
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness);
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        CHECK(harness.written().find("\x1b[?2031h") != std::string::npos);
    }
    {
        // A pair resolves through the COLORFGBG appearance and stays automatic.
        tests::EnvVarGuard env("COLORFGBG", "15;0");
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"light/dark"});
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        CHECK(harness.written().find("\x1b[?2031h") != std::string::npos);
    }
}

TEST_CASE("terminal color replies drive the pair and outrank the reported scheme",
        "[coding_agent][theme][controller][issue991][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", "0;15");
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"light/dark"});

    controller.apply_from_settings();
    CHECK(controller.active_theme_name() == "light");
    REQUIRE(count_query_bursts(harness.written()) == 1);

    // A live mode-2031 report decides while no background is reported: it
    // outranks COLORFGBG (pi `detectTerminalTheme` order).
    REQUIRE(harness.terminal.inject_input("\x1b[?997;1n"));
    REQUIRE(harness.pump(controller) == 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Dark);
    CHECK(controller.active_theme_name() == "dark");
    // pi `applyTerminalColorSchemeChange` queries the colors again.
    CHECK(count_query_bursts(harness.written()) == 2);

    // The startup query's replies arrive together: the reported light
    // background now outranks the live scheme and selects the light half.
    REQUIRE(harness.terminal.inject_input("\x1b]11;#f0f0f0\x07\x1b[?1;2c"));
    REQUIRE(harness.pump(controller) >= 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Light);
    CHECK(controller.active_theme_name() == "light");

    // Another live report cannot override the reported background.
    REQUIRE(harness.terminal.inject_input("\x1b[?997;1n"));
    REQUIRE(harness.pump(controller) == 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Light);
    CHECK(controller.active_theme_name() == "light");
}

TEST_CASE("a late color reply after the query timeout still applies",
        "[coding_agent][theme][controller][issue991][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness);

    controller.apply_from_settings();
    CHECK(controller.active_theme_name() == "dark");
    // The 100 ms timeout reports nothing: the palette and the appearance stay.
    REQUIRE(wait_for_reports(harness, 1));
    REQUIRE(harness.pump(controller) == 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Dark);
    CHECK(controller.active_theme_name() == "dark");

    // pi `onLateReply`: a reply after the timeout is applied again.
    REQUIRE(harness.terminal.inject_input("\x1b]11;#f0f0f0\x07"));
    REQUIRE(harness.report_count() == 1);
    REQUIRE(harness.pump(controller) == 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Light);
    CHECK(controller.active_theme_name() == "light");
}

TEST_CASE("an explicit theme ignores live scheme reports", "[coding_agent][theme][controller][issue991][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});

    controller.apply_from_settings();
    const auto queries = count_query_bursts(harness.written());
    const auto changed = harness.changed;

    // The listener is registered either way; the host gate drops the report
    // before it can change anything (pi `applyTerminalColorSchemeChange`).
    REQUIRE(harness.terminal.inject_input("\x1b[?997;2n"));
    REQUIRE(harness.report_count() == 1);
    CHECK(harness.pump(controller) == 1);
    CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Dark);
    CHECK(controller.active_theme_name() == "dark");
    CHECK(harness.changed == changed);
    // No re-query and no mode-2031 toggle for a defined choice.
    CHECK(count_query_bursts(harness.written()) == queries);
    CHECK(harness.written().find("\x1b[?2031h") == std::string::npos);
}

TEST_CASE("the scheme listener keeps exactly one registration across rebind and dispose",
        "[coding_agent][theme][controller][issue991][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness);
    controller.apply_from_settings();

    // One registration: each terminal report reaches the host hook once.
    REQUIRE(harness.terminal.inject_input("\x1b[?997;2n"));
    CHECK(harness.report_count() == 1);
    (void)harness.pump(controller);

    // pi `rebindTui` replaces the registration instead of adding one.
    controller.rebind_terminal_color_scheme_listener();
    REQUIRE(harness.terminal.inject_input("\x1b[?997;1n"));
    CHECK(harness.report_count() == 1);
    (void)harness.pump(controller);
    controller.rebind_terminal_color_scheme_listener();
    controller.rebind_terminal_color_scheme_listener();
    REQUIRE(harness.terminal.inject_input("\x1b[?997;2n"));
    CHECK(harness.report_count() == 1);
    (void)harness.pump(controller);

    // pi `dispose`: the listener is detached and mode 2031 is disabled.
    controller.dispose();
    CHECK(harness.written().find("\x1b[?2031l") != std::string::npos);
    REQUIRE(harness.terminal.inject_input("\x1b[?997;1n"));
    CHECK(harness.report_count() == 0);
    CHECK(controller.active_theme_name() == "light");
}

TEST_CASE("shared theme color math classifies terminal appearance like the frozen system theme",
        "[coding_agent][theme][color][issue991][spec]") {
    using coding_agent::tui::theme_color_math::relative_luminance;
    using coding_agent::tui::theme_color_math::terminal_appearance;
    using coding_agent::tui::theme_color_math::TerminalAppearance;
    using coding_agent::tui::theme_color_math::wcag_contrast;
    const tui::RgbColor black{.red = 0, .green = 0, .blue = 0};
    const tui::RgbColor white{.red = 255, .green = 255, .blue = 255};
    const tui::RgbColor light_background{.red = 0xf0, .green = 0xf0, .blue = 0xf0};
    const tui::RgbColor dark_background{.red = 0x10, .green = 0x10, .blue = 0x10};

    // WCAG 2 relative luminance and the 0.04045 sRGB transfer threshold.
    CHECK(relative_luminance(black) == 0.0);
    CHECK(relative_luminance(white) == Catch::Approx(1.0));
    CHECK(relative_luminance(light_background) > relative_luminance(dark_background));
    CHECK(wcag_contrast(black, white) == Catch::Approx(21.0));
    CHECK(wcag_contrast(white, light_background) == Catch::Approx(1.13).epsilon(0.01));

    // A readable foreground sets the appearance; otherwise white-versus-black
    // contrast on the background decides (pi `terminalAppearance`).
    CHECK(terminal_appearance(light_background, black) == TerminalAppearance::Light);
    CHECK(terminal_appearance(dark_background, white) == TerminalAppearance::Dark);
    CHECK(terminal_appearance(light_background, std::nullopt) == TerminalAppearance::Light);
    CHECK(terminal_appearance(dark_background, std::nullopt) == TerminalAppearance::Dark);
    // A foreground below the 4.5 text contrast minimum does not decide.
    CHECK(terminal_appearance(light_background, light_background) == TerminalAppearance::Light);
}

TEST_CASE("init_boot_theme resolves the setting or the env default with silent dark fallback",
        "[coding_agent][theme][controller][issue415][spec]") {
    using coding_agent::tui::init_boot_theme;
    tests::TempWorkspace config;
    config.write("themes/solarized.json", tests::fixture_theme("solarized", "#abcdef"));

    {
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        CHECK(init_boot_theme({}, std::nullopt) == coding_agent::tui::builtin_light_theme());
    }
    {
        tests::EnvVarGuard env("COLORFGBG", "15;0");
        CHECK(init_boot_theme({}, std::nullopt) == coding_agent::tui::builtin_dark_theme());
    }
    // A defined setting wins over the env default.
    CHECK(init_boot_theme({}, std::optional<std::string>{"dark"}) ==
        coding_agent::tui::builtin_dark_theme());
    // A custom-directory theme loads by name.
    const auto custom = init_boot_theme(config.path(), std::optional<std::string>{"solarized"});
    REQUIRE(custom.name == "solarized");
    const coding_agent::tui::RgbThemeColor solarized_accent{.red = 0xab, .green = 0xcd, .blue = 0xef};
    CHECK(accent_of(custom) == solarized_accent);
    // An unknown name falls back to dark silently.
    CHECK(init_boot_theme({}, std::optional<std::string>{"missing"}) ==
        coding_agent::tui::builtin_dark_theme());
}

TEST_CASE("theme controller boots from the settings theme or the env default",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;

    {
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});
        CHECK(controller.active_theme_name() == "dark");
        CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Dark);
        CHECK(harness.errors.empty());
        CHECK(harness.changed == 0);
    }
    {
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        auto controller = make_controller(harness);
        CHECK(controller.active_theme_name() == "light");
        CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
            "\x1b[38;2;90;128;128mx\x1b[39m");
    }
    // A failing settings theme falls back to dark silently at boot.
    {
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"missing"});
        CHECK(controller.active_theme_name() == "dark");
        CHECK(harness.errors.empty());
    }
}

TEST_CASE("theme controller registered themes apply by name and re-register on set_registered_themes",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("solarized.json", tests::fixture_theme("solarized", "#abcdef")),
        .source_path = "solarized.json",
    }};
    auto controller = make_controller(harness, {}, std::move(registered));

    CHECK(controller.set_theme_name("solarized", false));
    CHECK(controller.active_theme_name() == "solarized");
    CHECK(harness.changed == 1);
    CHECK(harness.base_pointer->invalidations == 1);
    CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;171;205;239mx\x1b[39m");

    // pi `setRegisteredThemes` replaces the map (reload re-registration).
    controller.set_registered_themes({});
    CHECK_FALSE(controller.set_theme_name("solarized", false));
    CHECK(controller.active_theme_name() == "dark");
    CHECK(harness.changed == 2);
}

TEST_CASE("apply_theme_name failure falls back to dark with the verbatim message",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});

    const auto failed = controller.set_theme_name("missing", /* show_error */ true);

    CHECK_FALSE(failed);
    CHECK(controller.active_theme_name() == "dark");
    // pi applyThemeName verbatim:
    // `Failed to load theme "<name>": <error>\nFell back to dark theme.`
    REQUIRE(harness.errors.size() == 1);
    CHECK(harness.errors[0] ==
        "Failed to load theme \"missing\": Theme not found: missing\nFell back to dark theme.");
    CHECK(harness.changed == 1);

    // Without show_error the fallback stays silent.
    harness.errors.clear();
    CHECK_FALSE(controller.set_theme_name("missing", false));
    CHECK(harness.errors.empty());
}

TEST_CASE("preview applies in memory without committing or changing the active name",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("solarized.json", tests::fixture_theme("solarized", "#abcdef")),
        .source_path = "solarized.json",
    }};
    auto controller = make_controller(harness, {}, std::move(registered));

    controller.preview("solarized");

    // The palette changed in memory; the active name and the change sink
    // did not (pi preview: `setTheme` + invalidate, no `applyThemeName`).
    CHECK(controller.active_theme_name() == "dark");
    CHECK(harness.changed == 0);
    CHECK(harness.base_pointer->invalidations == 1);
    CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;171;205;239mx\x1b[39m");

    // A slash setting preview resolves to the active theme (pi preview
    // `resolveThemeSetting(...) ?? activeThemeName`).
    const auto before = harness.base_pointer->invalidations;
    controller.preview("light/dark");
    CHECK(harness.base_pointer->invalidations == before + 1);
    CHECK(controller.active_theme_name() == "dark");
}

TEST_CASE("available_theme_names lists builtins custom directory and registered sorted",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;
    tests::TempWorkspace config;
    config.write("themes/solarized.json", tests::fixture_theme("solarized", "#abcdef"));
    config.write("themes/broken.json", "{not json");
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("registered.json", tests::fixture_theme("registered", "#010203")),
        .source_path = "registered.json",
    }};

    auto controller = make_controller(harness, config.path() / "themes", std::move(registered));

    // pi `getAvailableThemes`: builtins + custom directory (invalid files
    // ignored) + registered, deduped and sorted.
    const auto names = controller.available_theme_names();
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "dark");
    CHECK(names[1] == "light");
    CHECK(names[2] == "registered");
    CHECK(names[3] == "solarized");
}

TEST_CASE("discover_themes parses documents and dedupes with pi collision diagnostics",
        "[coding_agent][theme][controller][issue415][spec]") {
    std::vector<coding_agent::LoadedThemeResource> documents{{
                                                                     .path = ".pi/themes/project.json",
                                                                     .json = tests::fixture_theme("shared", "#111111"),
                                                                     .scope = coding_agent::SourceScope::Project,
                                                             },
            {
                    .path = "/home/user/.pike/agent/themes/user.json",
                    .json = tests::fixture_theme("user-theme", "#222222"),
                    .scope = coding_agent::SourceScope::User,
            },
            {
                    .path = "shared.json",
                    .json = tests::fixture_theme("shared", "#333333"),
                    .scope = coding_agent::SourceScope::Temporary,
            },
            {
                    .path = "broken.json",
                    .json = "{not json",
                    .scope = coding_agent::SourceScope::Temporary,
            }};

    auto discovery = coding_agent::tui::discover_themes(std::move(documents));

    // First-wins dedupe in load order (discovered before explicit): the
    // project theme wins and the explicit duplicate loses with pi's
    // collision diagnostic carrying the winner/loser paths.
    REQUIRE(discovery.themes.size() == 2);
    CHECK(discovery.themes[0].theme.name == "shared");
    CHECK(discovery.themes[0].source_path == std::filesystem::path{".pi/themes/project.json"});
    const coding_agent::tui::RgbThemeColor project_accent{.red = 0x11, .green = 0x11, .blue = 0x11};
    CHECK(accent_of(discovery.themes[0].theme) == project_accent);
    CHECK(discovery.themes[1].theme.name == "user-theme");
    CHECK(discovery.themes[1].source_path == std::filesystem::path{"/home/user/.pike/agent/themes/user.json"});

    const auto collision = std::find_if(
        discovery.diagnostics.begin(),
        discovery.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.type == coding_agent::ResourceDiagnosticType::Collision;
        });
    REQUIRE(collision != discovery.diagnostics.end());
    CHECK(collision->message == "name \"shared\" collision");
    REQUIRE(collision->collision.has_value());
    CHECK(collision->collision->resource_type == coding_agent::ResourceCollisionResourceType::Theme);
    CHECK(collision->collision->name == "shared");
    CHECK(collision->collision->winner_path == ".pi/themes/project.json");
    CHECK(collision->collision->loser_path == "shared.json");

    // The broken document is a warning with its path, like pi's
    // `loadThemeFromFile` catch.
    const auto parse_warning = std::find_if(
        discovery.diagnostics.begin(),
        discovery.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.type == coding_agent::ResourceDiagnosticType::Warning;
        });
    REQUIRE(parse_warning != discovery.diagnostics.end());
    CHECK(parse_warning->path == std::optional<std::string>{"broken.json"});
}

TEST_CASE("theme controller preview failure falls back to dark without invalidating",
        "[coding_agent][theme][controller][issue415][spec]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});

    controller.preview("missing");

    // pi `setTheme` failure replaces the palette with dark silently; the
    // preview invalidates only on success.
    CHECK(harness.base_pointer->invalidations == 0);
    CHECK(controller.active_theme_name() == "dark");
}
