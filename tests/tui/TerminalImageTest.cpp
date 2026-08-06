#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include "support/EnvVarGuard.hpp"
#include "support/ImageEnvironmentGuard.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/test/terminal-image.test.ts
// (detectCapabilities env-rule cases, hyperlink, imageFallback with ~/
// shortening and file:// linking) and test/tui-cell-size-input.test.ts
// (cell-size response consumption and bare-escape forwarding).

namespace {

class ImageCapabilitiesGuard final {
public:
    explicit ImageCapabilitiesGuard(cch::tui::DetectedImageCapabilities capabilities) {
        cch::tui::set_image_capabilities(capabilities);
    }
    ~ImageCapabilitiesGuard() {
        cch::tui::reset_image_capabilities_cache();
    }

    ImageCapabilitiesGuard(const ImageCapabilitiesGuard&) = delete;
    ImageCapabilitiesGuard& operator=(const ImageCapabilitiesGuard&) = delete;
};

using cch::tests::ImageEnvironmentGuard;

} // namespace

TEST_CASE("detect_image_capabilities defaults to conservative unknown-terminal", "[tui][image][terminal-image][issue385]") {
    ImageEnvironmentGuard environment;

    const auto caps = cch::tui::detect_image_capabilities();
    CHECK(caps.images == cch::tui::InlineImageProtocol::None);
    CHECK_FALSE(caps.hyperlinks);
}

TEST_CASE("detect_image_capabilities probes tmux OSC 8 forwarding", "[tui][image][terminal-image][issue385]") {
    ImageEnvironmentGuard environment;
    environment.set("TMUX", "/tmp/tmux-1000/default,1234,0");
    environment.set("TERM_PROGRAM", "ghostty");

    const auto forwards = cch::tui::detect_image_capabilities([] { return true; });
    CHECK(forwards.images == cch::tui::InlineImageProtocol::None);
    CHECK(forwards.hyperlinks);

    const auto blocked = cch::tui::detect_image_capabilities([] { return false; });
    CHECK(blocked.images == cch::tui::InlineImageProtocol::None);
    CHECK_FALSE(blocked.hyperlinks);
}

TEST_CASE("detect_image_capabilities probes when TERM starts with tmux", "[tui][image][terminal-image][issue385]") {
    ImageEnvironmentGuard environment;
    environment.set("TERM", "tmux-256color");
    environment.set("TERM_PROGRAM", "iterm.app");

    const auto forwards = cch::tui::detect_image_capabilities([] { return true; });
    CHECK(forwards.images == cch::tui::InlineImageProtocol::None);
    CHECK(forwards.hyperlinks);

    const auto blocked = cch::tui::detect_image_capabilities([] { return false; });
    CHECK_FALSE(blocked.hyperlinks);
}

TEST_CASE("detect_image_capabilities forces hyperlinks off under screen", "[tui][image][terminal-image][issue385]") {
    ImageEnvironmentGuard environment;
    environment.set("TERM", "screen-256color");
    const auto caps = cch::tui::detect_image_capabilities([] { return true; });
    CHECK(caps.images == cch::tui::InlineImageProtocol::None);
    CHECK_FALSE(caps.hyperlinks);
}

TEST_CASE("detect_image_capabilities enables per-emulator protocols", "[tui][image][terminal-image][issue385]") {
    struct Fixture {
        std::string name;
        std::string value;
        cch::tui::InlineImageProtocol expected;
    };
    const std::vector<Fixture> fixtures{
        {.name = "TERM_PROGRAM", .value = "ghostty", .expected = cch::tui::InlineImageProtocol::Kitty},
        {.name = "KITTY_WINDOW_ID", .value = "1", .expected = cch::tui::InlineImageProtocol::Kitty},
        {.name = "WEZTERM_PANE", .value = "0", .expected = cch::tui::InlineImageProtocol::Kitty},
        {.name = "WARP_SESSION_ID", .value = "some-session-id", .expected = cch::tui::InlineImageProtocol::Kitty},
        {.name = "WARP_TERMINAL_SESSION_UUID", .value = "d0e1a2e5-7ca7-44cd-9037-ac7222011161", .expected = cch::tui::InlineImageProtocol::Kitty},
        {.name = "ITERM_SESSION_ID", .value = "pid", .expected = cch::tui::InlineImageProtocol::ITerm2},
        {.name = "WT_SESSION", .value = "session", .expected = cch::tui::InlineImageProtocol::None},
    };
    for (const auto& fixture : fixtures) {
        ImageEnvironmentGuard environment;
        environment.set(fixture.name, fixture.value);
        const auto caps = cch::tui::detect_image_capabilities();
        CHECK(caps.images == fixture.expected);
        CHECK(caps.hyperlinks);
    }
}

TEST_CASE("detect_image_capabilities honors TERM_PROGRAM values pi pins", "[tui][image][terminal-image][issue385]") {
    struct Fixture {
        std::string value;
        cch::tui::InlineImageProtocol expected;
        bool hyperlinks;
    };
    const std::vector<Fixture> fixtures{
        {.value = "WarpTerminal", .expected = cch::tui::InlineImageProtocol::Kitty, .hyperlinks = true},
        {.value = "kitty", .expected = cch::tui::InlineImageProtocol::Kitty, .hyperlinks = true},
        {.value = "wezterm", .expected = cch::tui::InlineImageProtocol::Kitty, .hyperlinks = true},
        {.value = "iterm.app", .expected = cch::tui::InlineImageProtocol::ITerm2, .hyperlinks = true},
        {.value = "vscode", .expected = cch::tui::InlineImageProtocol::None, .hyperlinks = true},
        {.value = "alacritty", .expected = cch::tui::InlineImageProtocol::None, .hyperlinks = true},
    };
    for (const auto& fixture : fixtures) {
        ImageEnvironmentGuard environment;
        environment.set("TERM_PROGRAM", fixture.value);
        environment.set("TERM", "xterm-256color");
        const auto caps = cch::tui::detect_image_capabilities();
        CHECK(caps.images == fixture.expected);
        CHECK(caps.hyperlinks == fixture.hyperlinks);
    }
}

TEST_CASE("detect_image_capabilities keeps JetBrains and unknown terminals hyperlink-free", "[tui][image][terminal-image][issue385]") {
    {
        ImageEnvironmentGuard environment;
        environment.set("TERMINAL_EMULATOR", "JetBrains-JediTerm");
        environment.set("TERM", "xterm-256color");
        const auto caps = cch::tui::detect_image_capabilities();
        CHECK(caps.images == cch::tui::InlineImageProtocol::None);
        CHECK_FALSE(caps.hyperlinks);
    }
    {
        ImageEnvironmentGuard environment;
        environment.set("TERM_PROGRAM", "ghostty");
        environment.set("CMUX_WORKSPACE_ID", "workspace");
        const auto caps = cch::tui::detect_image_capabilities();
        CHECK(caps.images == cch::tui::InlineImageProtocol::Kitty);
        CHECK(caps.hyperlinks);
    }
}

TEST_CASE("detect_image_capabilities treats set-but-empty variables as absent", "[tui][image][terminal-image][issue385]") {
    ImageEnvironmentGuard environment;
    environment.set("KITTY_WINDOW_ID", "");
    environment.set("TERM_PROGRAM", "");
    environment.set("TERM", "");
    environment.set("TMUX", "");
    const auto caps = cch::tui::detect_image_capabilities([] { return true; });
    CHECK(caps.images == cch::tui::InlineImageProtocol::None);
    CHECK_FALSE(caps.hyperlinks);
}

TEST_CASE("image capability cache mirrors pi getCapabilities set and reset", "[tui][image][terminal-image][issue385]") {
    cch::tui::reset_image_capabilities_cache();
    const auto detected = cch::tui::get_image_capabilities();
    CHECK(detected == cch::tui::detect_image_capabilities());

    cch::tui::set_image_capabilities({
        .images = cch::tui::InlineImageProtocol::Kitty,
        .hyperlinks = true,
    });
    const auto overridden = cch::tui::get_image_capabilities();
    CHECK(overridden.images == cch::tui::InlineImageProtocol::Kitty);
    CHECK(overridden.hyperlinks);
    cch::tui::reset_image_capabilities_cache();
}

TEST_CASE("hyperlink emits pi-exact OSC 8 sequences", "[tui][image][terminal-image][issue385]") {
    CHECK(cch::tui::hyperlink("click me", "https://example.com") ==
        "\x1b]8;;https://example.com\x1b\\click me\x1b]8;;\x1b\\");
    CHECK(cch::tui::hyperlink("", "https://example.com") ==
        "\x1b]8;;https://example.com\x1b\\\x1b]8;;\x1b\\");
    CHECK(cch::tui::hyperlink("README.md", "file:///home/user/README.md") ==
        "\x1b]8;;file:///home/user/README.md\x1b\\README.md\x1b]8;;\x1b\\");
}

TEST_CASE("image_fallback shortens home-prefixed absolute paths without hyperlinks", "[tui][image][terminal-image][issue385]") {
    ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::None, .hyperlinks = false});
    cch::tests::EnvVarGuard home("HOME", std::string{"/tmp/home"});

    const auto result = cch::tui::image_fallback(
        "image/png",
        cch::tui::ImagePixelSize{.width = 1280, .height = 720},
        std::string_view{"/tmp/home/.pi/agent/shot.png"});
    CHECK(result == "[Image: ~/.pi/agent/shot.png [image/png] 1280x720]");
    CHECK(result.find("\x1b]8;") == std::string::npos);
}

TEST_CASE("image_fallback wraps shortened absolute paths in OSC 8 file links", "[tui][image][terminal-image][issue385]") {
    ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::None, .hyperlinks = true});
    cch::tests::EnvVarGuard home("HOME", std::string{"/tmp/home"});

    const auto result = cch::tui::image_fallback(
        "image/png",
        cch::tui::ImagePixelSize{.width = 10, .height = 10},
        std::string_view{"/tmp/home/.pi/agent/shot.png"});
    CHECK(result.find("\x1b]8;;file:///tmp/home/.pi/agent/shot.png\x1b\\") != std::string::npos);
    // Visible text must use ~/... not the expanded home path.
    CHECK(cch::tui::visible_width(result) ==
        cch::tui::visible_width("[Image: ~/.pi/agent/shot.png [image/png] 10x10]"));
}

TEST_CASE("image_fallback percent-encodes file URLs like node pathToFileURL", "[tui][image][terminal-image][issue385]") {
    ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::None, .hyperlinks = true});

    const auto spaced = cch::tui::image_fallback(
        "image/png",
        std::nullopt,
        std::string_view{"/tmp/my file.png"});
    CHECK(spaced.find("\x1b]8;;file:///tmp/my%20file.png\x1b\\") != std::string::npos);

    const auto reserved = cch::tui::image_fallback(
        "image/png",
        std::nullopt,
        std::string_view{"/tmp/a:b?c#d%~[x]"});
    CHECK(reserved.find("\x1b]8;;file:///tmp/a:b%3Fc%23d%25%7E%5Bx%5D\x1b\\") != std::string::npos);

    const auto utf8 = cch::tui::image_fallback(
        "image/png",
        std::nullopt,
        std::string_view{"/tmp/\xc3\xbcmlaut.png"});
    CHECK(utf8.find("\x1b]8;;file:///tmp/%C3%BCmlaut.png\x1b\\") != std::string::npos);
}

TEST_CASE("image_fallback leaves bare basenames unchanged and unlinked", "[tui][image][terminal-image][issue385]") {
    ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::None, .hyperlinks = true});

    const auto result = cch::tui::image_fallback(
        "image/png",
        cch::tui::ImagePixelSize{.width = 1, .height = 1},
        std::string_view{"clankolas.png"});
    CHECK(result == "[Image: clankolas.png [image/png] 1x1]");
    CHECK(result.find("\x1b]8;") == std::string::npos);
}

TEST_CASE("image_fallback omits filename and dimension segments when absent", "[tui][image][terminal-image][issue385]") {
    ImageCapabilitiesGuard capabilities({.images = cch::tui::InlineImageProtocol::None, .hyperlinks = false});

    CHECK(cch::tui::image_fallback(
              "image/png",
              cch::tui::ImagePixelSize{.width = 8, .height = 6}) == "[Image: [image/png] 8x6]");
    CHECK(cch::tui::image_fallback(
              "image/png",
              std::nullopt,
              std::string_view{"clankolas.png"}) == "[Image: clankolas.png [image/png]]");
}

TEST_CASE("cell-size input parser consumes complete responses and forwards the rest", "[tui][image][terminal-image][issue385]") {
    auto result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20;10t");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input.empty());
    REQUIRE(result.responses.size() == 1);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10}));

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20;10tq");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "q");
    REQUIRE(result.responses.size() == 1);

    result = cch::tui::detail::consume_cell_size_input({}, "user\x1b[6;30;40t");
    CHECK(result.forwarded_input == "user");
    REQUIRE(result.responses.size() == 1);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 30, .width_px = 40}));

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20;10t\x1b[6;30;40t");
    REQUIRE(result.responses.size() == 2);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10}));
    CHECK(result.responses[1] == (cch::tui::detail::CellSizeResponse{.height_px = 30, .width_px = 40}));
}

TEST_CASE("cell-size input parser buffers split and partial responses", "[tui][image][terminal-image][issue385]") {
    auto result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;");
    REQUIRE(result.responses.empty());
    CHECK(result.pending == "\x1b[6;");
    CHECK(result.forwarded_input.empty());

    result = cch::tui::detail::consume_cell_size_input(std::move(result.pending), "20;10t");
    CHECK(result.pending.empty());
    REQUIRE(result.responses.size() == 1);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10}));

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20;10");
    REQUIRE(result.responses.empty());
    CHECK(result.pending == "\x1b[6;20;10");

    result = cch::tui::detail::consume_cell_size_input(std::move(result.pending), "t");
    REQUIRE(result.responses.size() == 1);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 20, .width_px = 10}));
}

TEST_CASE("cell-size input parser never swallows bare escapes or invalid data", "[tui][image][terminal-image][issue385]") {
    auto result = cch::tui::detail::consume_cell_size_input({}, "\x1b");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "\x1b");
    CHECK(result.responses.empty());

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "\x1b[");
    CHECK(result.responses.empty());

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20a;10t");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "\x1b[6;20a;10t");
    CHECK(result.responses.empty());

    result = cch::tui::detail::consume_cell_size_input({}, "\x1b[6;20;10q");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "\x1b[6;20;10q");
    CHECK(result.responses.empty());

    // An invalid response still allows a later valid one in the same stream.
    result = cch::tui::detail::consume_cell_size_input({}, "x\x1b[6;bad\x1b[6;11;22t");
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input == "x\x1b[6;bad");
    REQUIRE(result.responses.size() == 1);
    CHECK(result.responses[0] == (cch::tui::detail::CellSizeResponse{.height_px = 11, .width_px = 22}));
}

TEST_CASE("cell-size input parser bounds buffered fragments", "[tui][image][terminal-image][issue385]") {
    auto result = cch::tui::detail::consume_cell_size_input(
        {},
        std::string("\x1b[6;") + std::string(70, '1'));
    CHECK(result.pending.empty());
    CHECK(result.forwarded_input.size() == 74);
    CHECK(result.responses.empty());
}

TEST_CASE("VirtualTerminal consumes cell-size responses protocol-aware", "[tui][image][terminal-image][issue385]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 80,
        .rows = 24,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
        },
    });
    std::vector<std::string> inputs;
    std::vector<cch::tui::TerminalDimensions> notifications;
    REQUIRE(terminal.start(
        [&](std::string input) { inputs.push_back(std::move(input)); },
        [&](cch::tui::TerminalDimensions dimensions) { notifications.push_back(dimensions); }));

    // Pi's 9x18 default applies until a CSI 16 t response arrives, and the
    // startup query is recorded.
    CHECK(terminal.capabilities().cell_pixels == cch::tui::CellPixelDimensions{});
    bool query_recorded = false;
    for (const auto& line : terminal.output()) {
        if (line == "\x1b[16t") query_recorded = true;
    }
    CHECK(query_recorded);

    // Complete response: consumed, applied, and not forwarded.
    REQUIRE(terminal.inject_input("\x1b[6;20;10t"));
    CHECK(terminal.capabilities().cell_pixels ==
        (cch::tui::CellPixelDimensions{.width = 10, .height = 20}));
    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0] == (cch::tui::TerminalDimensions{.columns = 80, .rows = 24}));
    CHECK(inputs.empty());

    // Later user input still forwards.
    REQUIRE(terminal.inject_input("q"));
    REQUIRE(inputs.size() == 1);
    CHECK(inputs[0] == "q");

    // A bare escape is forwarded immediately (pi's tui-cell-size-input pin).
    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(inputs.size() == 2);
    CHECK(inputs[1] == "\x1b");

    // Split responses are buffered across injections.
    REQUIRE(terminal.inject_input("\x1b[6;30;"));
    REQUIRE(terminal.inject_input("40t"));
    CHECK(terminal.capabilities().cell_pixels ==
        (cch::tui::CellPixelDimensions{.width = 40, .height = 30}));

    // Invalid sequences pass through untouched.
    REQUIRE(terminal.inject_input("\x1b[6;5a;5t"));
    REQUIRE(inputs.size() == 3);
    CHECK(inputs[2] == "\x1b[6;5a;5t");

    // Zero dimensions are consumed but not applied (pi consumeCellSizeResponse).
    REQUIRE(terminal.inject_input("\x1b[6;0;0t"));
    CHECK(terminal.capabilities().cell_pixels ==
        (cch::tui::CellPixelDimensions{.width = 40, .height = 30}));
    CHECK(inputs.size() == 3);

    // Notifications fire only for applied changes.
    REQUIRE(notifications.size() == 2);
    REQUIRE(terminal.stop());
}

TEST_CASE("VirtualTerminal emits the cell-size query only for image-capable terminals", "[tui][image][terminal-image][issue385]") {
    cch::tui::VirtualTerminal plain({.columns = 80, .rows = 24});
    REQUIRE(plain.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    CHECK(plain.output().empty());
    REQUIRE(plain.stop());
}
