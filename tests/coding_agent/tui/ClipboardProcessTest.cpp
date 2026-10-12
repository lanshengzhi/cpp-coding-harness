// #993 clipboard process seam: the frozen `runClipboardCommand` contract
// (launch, writer stdin, reader stdout capture, timeout, output bound, one
// observation of the process environment) and the OSC 52 encoding bound.

#include "coding_agent/tui/ClipboardProcess.hpp"

#include "FakeClipboardBin.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cctype>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

using testing::FakeClipboardBin;
using testing::FakeClipboardHelper;

using Clock = std::chrono::steady_clock;

[[nodiscard]] ClipboardCommandRequest command_request(const FakeClipboardBin& bin,
        std::string command,
        std::optional<std::string> input = std::nullopt,
        std::chrono::milliseconds timeout = kClipboardCommandTimeout,
        std::size_t max_buffer_bytes = kClipboardCommandMaxBufferBytes) {
    return ClipboardCommandRequest{
            .command = std::move(command),
            .args = {},
            .input = std::move(input),
            .timeout = timeout,
            .max_buffer_bytes = max_buffer_bytes,
            .search_path = bin.search_path(),
    };
}

[[nodiscard]] std::chrono::milliseconds elapsed_since(const Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

/// Run the asynchronous driver to completion on a throwaway loop.
TEST_CASE("Clipboard helper defaults pin pi's timeout and captured-output bound",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    CHECK(kClipboardCommandTimeout == std::chrono::milliseconds{3000});
    CHECK(kClipboardCommandMaxBufferBytes == 50u * 1024u * 1024u);
    CHECK(kMaxOsc52EncodedLength == 100000u);
}

TEST_CASE("Clipboard helper capture reports success, empty success, failure, and absence distinctly",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    bin.install({.name = "termux-clipboard-get"});
    bin.install({.name = "wl-paste", .output = "wayland text"});
    bin.install({.name = "xclip", .output = "x11 text\n"});
    bin.install({.name = "xsel", .exit_code = 1});

    SECTION("exit zero with output is a successful read") {
        const auto result = run_clipboard_command_blocking(command_request(bin, "wl-paste"));
        CHECK(result.exit_code == 0);
        CHECK(result.output == "wayland text");
        CHECK_FALSE(result.timed_out);
        CHECK_FALSE(result.cancelled);
        CHECK_FALSE(result.output_overflow);
    }

    SECTION("exit zero with empty output is a successful *empty* read, not a failure") {
        const auto result = run_clipboard_command_blocking(command_request(bin, "termux-clipboard-get"));
        CHECK(result.exit_code == 0);
        CHECK(result.output.empty());
    }

    SECTION("a non-zero exit is a failure that keeps its captured output") {
        const auto result = run_clipboard_command_blocking(command_request(bin, "xsel"));
        CHECK(result.exit_code == 1);
    }

    SECTION("a helper that cannot be launched has no result at all") {
        const auto result = run_clipboard_command_blocking(command_request(bin, "no-such-helper"));
        CHECK_FALSE(result.exit_code.has_value());
        CHECK(result.output.empty());
        CHECK_FALSE(result.timed_out);
        CHECK_FALSE(result.cancelled);
    }

    SECTION("the process PATH is never consulted for a helper") {
        // Same command name, resolution path empty: the helper exists only in
        // the injected bin, so the launch fails instead of falling back to a
        // real `xclip` on the developer's machine.
        auto request = command_request(bin, "wl-paste");
        request.search_path.clear();
        const auto result = run_clipboard_command_blocking(request);
        CHECK_FALSE(result.exit_code.has_value());
        CHECK(result.output.empty());
    }
}

TEST_CASE("Clipboard writer receives the payload on stdin and is never waited on through its stdout",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    bin.install({.name = "wl-copy", .reads_stdin = true});

    const auto result = run_clipboard_command_blocking(
            command_request(bin, "wl-copy", std::string{"payload with\nnewline"}));
    CHECK(result.exit_code == 0);
    REQUIRE(bin.invocations().size() == 1);
    CHECK(bin.invocations()[0] == "wl-copy payload with\nnewline");
}

TEST_CASE("Clipboard helpers that daemonize do not hold the caller through an inherited pipe",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    // A background holder inherits the helper's stdio, so a reader would still
    // be waiting on the pipe long after the launched helper exited. The writer
    // path must not be.
    bin.install_script("wl-copy", "(sleep 30 &)\n", /*reads_stdin=*/true);

    const auto start = Clock::now();
    const auto result = run_clipboard_command_blocking(command_request(bin, "wl-copy", std::string{"text"}));
    CHECK(result.exit_code == 0);
    CHECK(elapsed_since(start) < std::chrono::seconds{2});
}

TEST_CASE("Clipboard readers wait for stdio close exactly like pi's close event",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    // The launched helper exits immediately but a background grandchild keeps
    // the capture pipe open: pi resolves on the pipe's `close`, so the read is
    // not complete yet and the timeout aborts it without a result.
    bin.install_script("xclip", "(sleep 30 &)\nprintf 'partial'\n");

    const auto start = Clock::now();
    const auto result = run_clipboard_command_blocking(
            command_request(bin, "xclip", std::nullopt, std::chrono::milliseconds{200}));
    CHECK(result.timed_out);
    CHECK_FALSE(result.exit_code.has_value());
    CHECK(result.output.empty());
    CHECK(elapsed_since(start) < std::chrono::seconds{2});
}

TEST_CASE("Clipboard helper timeout kills and reaps the helper instead of blocking the caller",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    bin.install({.name = "xclip", .delay_seconds = 30});

    const auto start = Clock::now();
    const auto result = run_clipboard_command_blocking(
            command_request(bin, "xclip", std::nullopt, std::chrono::milliseconds{100}));
    CHECK(result.timed_out);
    CHECK_FALSE(result.exit_code.has_value());
    CHECK(result.output.empty());
    CHECK(elapsed_since(start) < std::chrono::seconds{2});
    CHECK(bin.invocations().size() == 1);
}

TEST_CASE("Clipboard helper output bound aborts an oversized read",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    bin.install({.name = "xclip", .output = std::string(4096, 'a')});

    const auto result = run_clipboard_command_blocking(
            command_request(bin, "xclip", std::nullopt, kClipboardCommandTimeout, 1024));
    CHECK(result.output_overflow);
    CHECK_FALSE(result.exit_code.has_value());
    CHECK(result.output.empty());
    CHECK_FALSE(result.timed_out);
}

TEST_CASE("Clipboard read observes cancellation while the helper is in flight",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    bin.install({.name = "xclip", .delay_seconds = 30});

    boost::asio::io_context io;
    std::stop_source cancellation;
    std::optional<ClipboardCommandResult> outcome;
    const auto start = Clock::now();
    boost::asio::co_spawn(
            io,
            [&]() -> boost::asio::awaitable<void> {
                outcome.emplace(co_await run_clipboard_command(
                        command_request(bin, "xclip", std::nullopt, std::chrono::seconds{30}),
                        cancellation.get_token()));
            },
            boost::asio::detached);
    boost::asio::steady_timer stop_timer{io.get_executor()};
    stop_timer.expires_after(std::chrono::milliseconds{100});
    stop_timer.async_wait([&](const boost::system::error_code&) { cancellation.request_stop(); });

    tests::drain_ready(io);

    REQUIRE(outcome.has_value());
    CHECK(outcome->cancelled);
    CHECK_FALSE(outcome->exit_code.has_value());
    CHECK_FALSE(outcome->timed_out);
    CHECK(elapsed_since(start) < std::chrono::seconds{2});
    // The helper really started, so the cancellation killed a live child.
    CHECK(bin.invocations().size() == 1);
}

TEST_CASE("Clipboard environment observation reads the process environment once",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    FakeClipboardBin bin;
    tests::EnvVarGuard termux{"TERMUX_VERSION", std::string{"0.118.3"}};
    tests::EnvVarGuard wayland{"WAYLAND_DISPLAY", std::string{"wayland-0"}};
    tests::EnvVarGuard display{"DISPLAY", std::string{":1"}};
    tests::EnvVarGuard ssh_connection{"SSH_CONNECTION", std::string{"10.0.0.1 5555 10.0.0.2 22"}};
    tests::EnvVarGuard ssh_client{"SSH_CLIENT", std::string{"10.0.0.1 5555 22"}};
    tests::EnvVarGuard mosh{"MOSH_CONNECTION", std::string{"10.0.0.2 60000 10.0.0.1 60001"}};
    tests::EnvVarGuard wt_session{"WT_SESSION", std::string{"terminal-session"}};
    tests::EnvVarGuard wsl_distro{"WSL_DISTRO_NAME", std::string{"Ubuntu"}};
    tests::EnvVarGuard path{"PATH", bin.search_path()};

    const auto environment = clipboard_environment_from_process();

    CHECK(environment.termux_version == "0.118.3");
    CHECK(environment.wayland_display == "wayland-0");
    CHECK(environment.display == ":1");
    CHECK(environment.ssh_connection == "10.0.0.1 5555 10.0.0.2 22");
    CHECK(environment.ssh_client == "10.0.0.1 5555 22");
    CHECK(environment.mosh_connection == "10.0.0.2 60000 10.0.0.1 60001");
    CHECK(environment.wt_session == "terminal-session");
    CHECK(environment.search_path == bin.search_path());
    // `WSL_DISTRO_NAME` is one of pi's WSL markers; the kernel probe may also
    // report WSL, so only the marked case is pinned here.
    CHECK(environment.is_wsl);
#if defined(__linux__)
    CHECK(environment.is_linux);
#endif
}

TEST_CASE("Clipboard environment markers absent leave every injected fact empty",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    tests::EnvVarGuard termux{"TERMUX_VERSION", std::nullopt};
    tests::EnvVarGuard wayland{"WAYLAND_DISPLAY", std::nullopt};
    tests::EnvVarGuard display{"DISPLAY", std::nullopt};
    tests::EnvVarGuard ssh_connection{"SSH_CONNECTION", std::nullopt};
    tests::EnvVarGuard ssh_client{"SSH_CLIENT", std::nullopt};
    tests::EnvVarGuard mosh{"MOSH_CONNECTION", std::nullopt};
    tests::EnvVarGuard wt_session{"WT_SESSION", std::nullopt};

    const auto environment = clipboard_environment_from_process();

    CHECK(environment.termux_version.empty());
    CHECK(environment.wayland_display.empty());
    CHECK(environment.display.empty());
    CHECK(environment.ssh_connection.empty());
    CHECK(environment.ssh_client.empty());
    CHECK(environment.mosh_connection.empty());
    CHECK(environment.wt_session.empty());
    CHECK_FALSE(clipboard_is_remote_session(environment));
    CHECK(clipboard_is_headless(environment));
}

TEST_CASE("clipboard_is_headless and clipboard_is_remote_session follow pi's conditions",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardEnvironment environment{};
    environment.is_linux = true;
    CHECK(clipboard_is_headless(environment));

    environment.display = ":0";
    CHECK_FALSE(clipboard_is_headless(environment));
    environment.display.clear();

    environment.wayland_display = "wayland-0";
    CHECK_FALSE(clipboard_is_headless(environment));
    environment.wayland_display.clear();

    environment.termux_version = "0.118.3";
    CHECK_FALSE(clipboard_is_headless(environment));
    environment.termux_version.clear();

    environment.is_linux = false;
    CHECK_FALSE(clipboard_is_headless(environment));

    ClipboardEnvironment remote{};
    CHECK_FALSE(clipboard_is_remote_session(remote));
    remote.ssh_connection = "host 1 client 2";
    CHECK(clipboard_is_remote_session(remote));
    remote.ssh_connection.clear();
    remote.ssh_client = "client 2 22";
    CHECK(clipboard_is_remote_session(remote));
    remote.ssh_client.clear();
    remote.mosh_connection = "client 2 server 3";
    CHECK(clipboard_is_remote_session(remote));
}

TEST_CASE("OSC 52 encoding pins pi's sequence, its UTF-8 bytes, and its size bound",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    const auto plain = encode_clipboard_osc52("hi");
    REQUIRE(plain.has_value());
    CHECK(*plain == std::string{"\x1b]52;c;aGk=\x07"});
    CHECK(plain->front() == '\x1b');
    CHECK(plain->back() == '\x07');

    // `café` is five UTF-8 bytes: the payload is encoded from its bytes, never
    // from a code-point count.
    const auto utf8 = encode_clipboard_osc52("caf\xc3\xa9");
    REQUIRE(utf8.has_value());
    CHECK(*utf8 == std::string{"\x1b]52;c;Y2Fmw6k=\x07"});

    // 75 000 payload bytes base64-encode to exactly the 100 000 character bound,
    // so the largest allowed payload is written in full...
    const auto largest = encode_clipboard_osc52(std::string(75000, 'a'));
    REQUIRE(largest.has_value());
    CHECK(largest->size() == kMaxOsc52EncodedLength + 8);

    // ...and one byte more is refused without emitting anything.
    CHECK_FALSE(encode_clipboard_osc52(std::string(75001, 'a')).has_value());
    CHECK_FALSE(encode_clipboard_osc52(std::string(90000, 'a')).has_value());
}

TEST_CASE("Clipboard temporary tokens are unique UUID-shaped names",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    const auto first = make_clipboard_unique_token();
    const auto second = make_clipboard_unique_token();

    CHECK(first.size() == 36);
    CHECK(first != second);
    for (std::size_t index = 0; index < first.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator) {
            CHECK(first[index] == '-');
        } else {
            CHECK(std::isxdigit(static_cast<unsigned char>(first[index])) != 0);
        }
    }
    CHECK(first.find('/') == std::string::npos);
}

} // namespace
} // namespace cch::coding_agent::tui
