// #993 clipboard copy policy: pi `copyToClipboard`'s frozen routes — the Linux
// helper order, the WSL Windows round trip, the OSC 52 terminal route and its
// size bound — driven through injected environments and fake helpers. Desktop
// Linux with a display must *fail* when its helpers fail instead of emitting
// OSC 52 (#9618).

#include "coding_agent/tui/ClipboardWrite.hpp"

#include "FakeClipboardBin.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using testing::FakeClipboardBin;

/// Records every OSC 52 sequence a copy writes to the terminal it owns.
class Osc52Recorder final {
public:
    [[nodiscard]] ClipboardWriteOptions options(const ClipboardEnvironment& environment) {
        ClipboardWriteOptions options{.environment = environment};
        options.write_osc52 = [this](std::string_view sequence) { sequences.emplace_back(sequence); };
        return options;
    }

    std::vector<std::string> sequences{};
};

/// The terminal that owns the copy: records every OSC 52 sequence a copy writes
/// instead of putting bytes on a real terminal.
class CopyTerminal final {
public:
    [[nodiscard]] ClipboardCopyResult copy(std::string_view text, ClipboardEnvironment environment) {
        auto options = recorder_.options(environment);
        return write_clipboard_text(text, options);
    }

    [[nodiscard]] const std::vector<std::string>& sequences() const { return recorder_.sequences; }

private:
    Osc52Recorder recorder_{};
};

[[nodiscard]] ClipboardEnvironment linux_environment(const FakeClipboardBin& bin) {
    ClipboardEnvironment environment{};
    environment.is_linux = true;
    environment.search_path = bin.search_path();
    return environment;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

/// Every helper installed with a non-zero exit so the environment's failure
/// message is the only outcome a copy can report.
void install_failing_helpers(const FakeClipboardBin& bin) {
    bin.install({.name = "termux-clipboard-set", .exit_code = 1, .reads_stdin = true});
    bin.install({.name = "wl-copy", .exit_code = 1, .reads_stdin = true});
    bin.install({.name = "xclip", .exit_code = 1, .reads_stdin = true});
    bin.install({.name = "xsel", .exit_code = 1, .reads_stdin = true});
}

TEST_CASE("Clipboard copy reports each environment's own failure message and never invents an OSC 52 route",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    install_failing_helpers(bin);
    CopyTerminal terminal;

    SECTION("Termux") {
        auto environment = linux_environment(bin);
        environment.termux_version = "0.118.3";
        const auto result = terminal.copy("text", environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: install the Termux:API app and `termux-api` package");
        CHECK(terminal.sequences().empty());
        CHECK(bin.invocations() == std::vector<std::string>{"termux-clipboard-set text"});
    }

    SECTION("Wayland") {
        auto environment = linux_environment(bin);
        environment.wayland_display = "wayland-0";
        const auto result = terminal.copy("text", environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: install `wl-clipboard` (`wl-copy`) or check Wayland access");
        CHECK(terminal.sequences().empty());
        CHECK(bin.invocations() == std::vector<std::string>{"wl-copy text"});
    }

    SECTION("X11: both helpers are tried and the failure is reported, not hidden behind OSC 52") {
        auto environment = linux_environment(bin);
        environment.display = ":0";
        const auto result = terminal.copy("text", environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: install `xclip` or `xsel`, or check X11 access");
        CHECK(terminal.sequences().empty());
        CHECK(bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard text", "xsel --clipboard --input text"});
    }

    SECTION("no route at all") {
        ClipboardEnvironment environment{};
        environment.is_linux = false;
        const auto result = terminal.copy("text", environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable");
        CHECK(terminal.sequences().empty());
        CHECK(bin.invocations().empty());
    }
}

TEST_CASE("Clipboard copy tries pi's Linux helpers in order and stops at the first success",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    CopyTerminal terminal;

    SECTION("every route present: a failing Termux helper hands over to Wayland") {
        bin.install({.name = "termux-clipboard-set", .exit_code = 1, .reads_stdin = true});
        bin.install({.name = "wl-copy", .reads_stdin = true});
        bin.install({.name = "xclip", .exit_code = 1, .reads_stdin = true});
        auto environment = linux_environment(bin);
        environment.termux_version = "0.118.3";
        environment.wayland_display = "wayland-0";
        environment.display = ":0";

        const auto result = terminal.copy("payload", environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        CHECK(terminal.sequences().empty());
        CHECK(bin.invocations() == std::vector<std::string>{"termux-clipboard-set payload", "wl-copy payload"});
    }

    SECTION("X11 order: xclip is tried first and the payload reaches the winning xsel") {
        bin.install({.name = "xclip", .exit_code = 1, .reads_stdin = true});
        bin.install({.name = "xsel", .reads_stdin = true});
        auto environment = linux_environment(bin);
        environment.display = ":0";

        const auto result = terminal.copy("payload", environment);
        CHECK(result.copied);
        CHECK(bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard payload", "xsel --clipboard --input payload"});
    }

    SECTION("X11 order: a succeeding xclip keeps the selection, so xsel is never run") {
        bin.install({.name = "xclip", .reads_stdin = true});
        bin.install({.name = "xsel", .reads_stdin = true});
        auto environment = linux_environment(bin);
        environment.display = ":0";

        const auto result = terminal.copy("payload", environment);
        CHECK(result.copied);
        CHECK(bin.invocations() == std::vector<std::string>{"xclip -selection clipboard payload"});
    }

    SECTION("the helper payload is the exact text, not a shell-escaped variant") {
        bin.install({.name = "wl-copy", .reads_stdin = true});
        auto environment = linux_environment(bin);
        environment.wayland_display = "wayland-0";

        const auto result = terminal.copy("quote ' and $HOME and \\", environment);
        CHECK(result.copied);
        CHECK(bin.invocations() == std::vector<std::string>{"wl-copy quote ' and $HOME and \\"});
    }
}

TEST_CASE("Clipboard copy without a display route writes the OSC 52 sequence through the terminal",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    CopyTerminal terminal;

    SECTION("a headless Linux session has no helper route left, so the terminal is the clipboard") {
        const auto result = terminal.copy("hello", linux_environment(bin));
        CHECK(result.copied);
        CHECK(result.error.empty());
        CHECK(bin.invocations().empty());
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(terminal.sequences()[0] == std::string{"\x1b]52;c;aGVsbG8=\x07"});
    }

    SECTION("a headless session with no terminal route reports unavailable, not a size failure") {
        ClipboardEnvironment environment{};
        environment.is_linux = true;
        ClipboardWriteOptions options{.environment = environment};
        const auto result = write_clipboard_text("hello", options);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable");
        CHECK(terminal.sequences().empty());
    }

    SECTION("a headless session writes the largest allowed payload in full and refuses one byte more") {
        const auto largest = terminal.copy(std::string(75000, 'a'), linux_environment(bin));
        CHECK(largest.copied);
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(terminal.sequences()[0].size() == kMaxOsc52EncodedLength + 8);
    }

    SECTION("an oversized payload reports pi's size failure and writes nothing") {
        const auto result = terminal.copy(std::string(75001, 'a'), linux_environment(bin));
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: text exceeds the OSC 52 size limit");
        CHECK(terminal.sequences().empty());
    }
}

TEST_CASE("Clipboard copy in a remote session reaches the client clipboard through OSC 52",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    install_failing_helpers(bin);
    CopyTerminal terminal;

    SECTION("a remote session with failing helpers still copies through OSC 52") {
        auto environment = linux_environment(bin);
        environment.display = ":0";
        environment.ssh_connection = "10.0.0.1 5555 10.0.0.2 22";

        const auto result = terminal.copy("hello", environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        CHECK(bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard hello", "xsel --clipboard --input hello"});
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(terminal.sequences()[0] == std::string{"\x1b]52;c;aGVsbG8=\x07"});
    }

    SECTION("a remote session also emits OSC 52 when a local helper succeeded") {
        bin.install({.name = "xsel", .reads_stdin = true});
        auto environment = linux_environment(bin);
        environment.display = ":0";
        environment.mosh_connection = "10.0.0.1 60000 10.0.0.2 60001";

        const auto result = terminal.copy("hello", environment);
        CHECK(result.copied);
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard hello", "xsel --clipboard --input hello"});
    }

    SECTION("a non-Linux remote session never runs a Linux helper") {
        ClipboardEnvironment environment{};
        environment.is_linux = false;
        environment.ssh_client = "10.0.0.1 5555 22";

        const auto result = terminal.copy("hello", environment);
        CHECK(result.copied);
        CHECK(bin.invocations().empty());
        REQUIRE(terminal.sequences().size() == 1);
    }

    SECTION("a remote session reports the size failure when its OSC 52 payload is too large") {
        auto environment = linux_environment(bin);
        const auto result = terminal.copy(std::string(75001, 'a'), environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: text exceeds the OSC 52 size limit");
        CHECK(terminal.sequences().empty());
    }
}

TEST_CASE("WSL copy prefers OSC 52 under Windows Terminal and otherwise round-trips through PowerShell",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    const std::string text = "caf\xc3\xa9 clipboard";
    const std::string windows_path = "/mnt/c/Users/O'Brien/pi-wsl-clip.txt";
    // pi doubles every `'` before embedding the converted path in the script.
    const std::string expected_windows_path = "/mnt/c/Users/O''Brien/pi-wsl-clip.txt";

    FakeClipboardBin bin;
    install_failing_helpers(bin);
    const auto saved_copy = bin.directory().parent_path() / "windows-clipboard-copy.txt";
    const auto recorded_mode = bin.directory().parent_path() / "windows-clipboard-mode.txt";
    // The `wslpath` fake records the temporary file's mode and path (so the test
    // can prove pi's 0600 UTF-8 file and its cleanup) and copies the bytes to the
    // test's own file before that temporary file is unlinked.
    bin.install_script("wslpath",
            std::format("cp \"$2\" {}\nprintf '%s %s' \"$(stat -c %a \"$2\")\" \"$2\" > {}\nprintf '%s' {}\n",
                    FakeClipboardBin::shell_quote(saved_copy.string()),
                    FakeClipboardBin::shell_quote(recorded_mode.string()),
                    FakeClipboardBin::shell_quote(windows_path)));
    bin.install({.name = "powershell.exe"});
    CopyTerminal terminal;

    SECTION("Windows Terminal writes OSC 52 and skips the Windows round trip") {
        auto environment = linux_environment(bin);
        environment.display = ":0";
        environment.is_wsl = true;
        environment.wt_session = "terminal-session";

        const auto result = terminal.copy(text, environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard " + text, "xsel --clipboard --input " + text});
    }

    SECTION("without Windows Terminal PowerShell reads the UTF-8 temporary file") {
        auto environment = linux_environment(bin);
        environment.display = ":0";
        environment.is_wsl = true;

        const auto result = terminal.copy(text, environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        CHECK(terminal.sequences().empty());
        REQUIRE(bin.invocations().size() == 4);
        CHECK(bin.invocations()[0] == "xclip -selection clipboard " + text);
        CHECK(bin.invocations()[1] == "xsel --clipboard --input " + text);
        CHECK(bin.invocations()[2].starts_with("wslpath -w "));
        CHECK(bin.invocations()[3] == "powershell.exe -NoProfile -Command " +
                                              std::format("Set-Clipboard -Value ([System.IO.File]::ReadAllText('{}', "
                                                          "[System.Text.Encoding]::UTF8))",
                                                      expected_windows_path));
        // pi writes the text as UTF-8 into a 0600 temporary file and always
        // removes it, whichever way the round trip ended.
        CHECK(read_file(saved_copy) == text);
        const auto recorded = read_file(recorded_mode);
        const auto separator = recorded.find(' ');
        REQUIRE(separator != std::string::npos);
        CHECK(recorded.substr(0, separator) == "600");
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path{recorded.substr(separator + 1)}));
    }

    SECTION("a failed Windows round trip on a desktop WSL session reports the display failure") {
        bin.install({.name = "powershell.exe", .exit_code = 1});
        auto environment = linux_environment(bin);
        environment.display = ":0";
        environment.is_wsl = true;

        const auto result = terminal.copy(text, environment);
        CHECK_FALSE(result.copied);
        CHECK(result.error == "Clipboard unavailable: install `xclip` or `xsel`, or check X11 access");
        CHECK(terminal.sequences().empty());
        // The temporary file is removed even when PowerShell fails.
        const auto recorded = read_file(recorded_mode);
        const auto separator = recorded.find(' ');
        REQUIRE(separator != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path{recorded.substr(separator + 1)}));
    }

    SECTION("a failed Windows round trip without WSLg falls back to OSC 52, the only remaining route") {
        bin.install({.name = "powershell.exe", .exit_code = 1});
        auto environment = linux_environment(bin);
        environment.is_wsl = true;

        const auto result = terminal.copy(text, environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        REQUIRE(terminal.sequences().size() == 1);
        CHECK(terminal.sequences()[0].starts_with("\x1b]52;c;"));
    }

    SECTION("WSL without WSLg copies through the Windows clipboard") {
        auto environment = linux_environment(bin);
        environment.is_wsl = true;
        const auto result = terminal.copy(text, environment);
        CHECK(result.copied);
        CHECK(result.error.empty());
        CHECK(terminal.sequences().empty());
        // No helper route exists without a display, so only the interop pair runs.
        REQUIRE(bin.invocations().size() == 2);
        CHECK(bin.invocations()[0].starts_with("wslpath -w "));
        CHECK(bin.invocations()[0].find("pi-wsl-clip-") != std::string::npos);
        CHECK(bin.invocations()[1].starts_with("powershell.exe -NoProfile -Command "));
    }
}

} // namespace
} // namespace cch::coding_agent::tui
