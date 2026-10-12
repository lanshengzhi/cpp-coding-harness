#pragma once

// Linux clipboard process seam (pi `clipboard-command.ts` `runClipboardCommand`,
// the `process.env` branches of `clipboard.ts`, and `wsl.ts`): the environment
// facts the clipboard policy branches on are *observed once* at the composition
// boundary and injected from there, and one subprocess contract carries every
// helper invocation. Extraction #993 from the pre-#993 `ClipboardWrite` path,
// which read `getenv` inline, tried a single X11 helper, and waited on children
// without a bound.
//
// Repository-private `cch_coding_agent` implementation header: not part of an
// Owner Interface, not installed, never exported.

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// pi's OSC 52 size bound (`MAX_OSC52_ENCODED_LENGTH`): a longer base64 payload
/// is refused rather than written.
inline constexpr std::size_t kMaxOsc52EncodedLength = 100'000;

/// pi's clipboard helper timeout when the caller names none
/// (`runClipboardCommand` `timeoutMs ?? 3000`).
inline constexpr std::chrono::milliseconds kClipboardCommandTimeout{3000};

/// pi's captured-stdout bound for one helper (`maxBufferBytes ?? 50 MiB`):
/// exceeding it aborts the helper and leaves the command without a result.
inline constexpr std::size_t kClipboardCommandMaxBufferBytes = 50 * 1024 * 1024;

/// The environment facts the Linux clipboard policy branches on. Read once by
/// `clipboard_environment_from_process()` at the composition boundary and
/// injected from there; no policy code below reads the process environment, so
/// tests drive every branch by injection instead of mutating the process.
struct ClipboardEnvironment {
    /// pi `platform() === "linux"`, which gates the whole Linux tool path.
    bool is_linux{true};
    /// pi `TERMUX_VERSION` and the Termux clipboard tools it selects.
    std::string termux_version{};
    /// pi `WAYLAND_DISPLAY` and the Wayland clipboard tools it selects.
    std::string wayland_display{};
    /// pi `DISPLAY` and the X11 clipboard tools it selects.
    std::string display{};
    /// pi `SSH_CONNECTION`, `SSH_CLIENT`, and `MOSH_CONNECTION`
    /// (`isRemoteSession`).
    std::string ssh_connection{};
    std::string ssh_client{};
    std::string mosh_connection{};
    /// pi `WT_SESSION`: Windows Terminal supports OSC 52, so a WSL session
    /// prefers it over the PowerShell round trip.
    std::string wt_session{};
    /// pi `isWSL` from `wsl.ts`.
    bool is_wsl{false};
    /// The helper resolution path, with `PATH` semantics. An empty path
    /// resolves nothing, so every helper becomes a launch failure rather than
    /// an implicit process-environment lookup.
    std::string search_path{};
};

/// pi `isRemoteSession`: the clipboard that matters lives on the client side of
/// the terminal connection, so OSC 52 is the only route that reaches it.
[[nodiscard]] bool clipboard_is_remote_session(const ClipboardEnvironment& environment);

/// pi's headless Linux condition (no display, no Wayland, no Termux): the
/// terminal is the only clipboard route.
[[nodiscard]] bool clipboard_is_headless(const ClipboardEnvironment& environment);

/// Observe the process environment once, exactly as pi reads `process.env`.
/// This is the only clipboard code that reads `getenv`.
[[nodiscard]] ClipboardEnvironment clipboard_environment_from_process();

/// One helper invocation (pi `runClipboardCommand`).
struct ClipboardCommandRequest {
    std::string command{};
    std::vector<std::string> args{};
    /// Writer payload. When unset the helper is a *reader*: its stdout is
    /// captured and bounded. A writer's stdout goes to `/dev/null` because
    /// clipboard writers can daemonize and must not be given an output pipe
    /// pi (or this runner) would wait on.
    std::optional<std::string> input{std::nullopt};
    std::chrono::milliseconds timeout{kClipboardCommandTimeout};
    std::size_t max_buffer_bytes{kClipboardCommandMaxBufferBytes};
    /// See `ClipboardEnvironment::search_path`; the executable is resolved
    /// against it and executed by absolute path.
    std::string search_path{};
};

/// One helper outcome. `exit_code` is unset when the helper could not be
/// launched, timed out, was cancelled, or exceeded `max_buffer_bytes` — pi's
/// `undefined` result. Exit code 0 is success, and an empty captured output is
/// a *successful* empty result.
struct ClipboardCommandResult {
    std::optional<int> exit_code{std::nullopt};
    bool timed_out{false};
    bool cancelled{false};
    bool output_overflow{false};
    std::string output{};
};

/// Run one helper to its terminal outcome, observing `stop_token`. A helper
/// that outlives its timeout, exceeds its output bound, or is cancelled while
/// in flight is SIGKILLed and reaped before this returns, so no child, pipe, or
/// zombie survives the call: writers that daemonize are never waited on through
/// the pipe a grandchild holds (their stdout is `/dev/null`).
[[nodiscard]] boost::asio::awaitable<ClipboardCommandResult> run_clipboard_command(
        ClipboardCommandRequest request, std::stop_token stop_token);

/// The same operation for a caller that cannot suspend (the copy path is
/// synchronous in this repository, pi awaits it): one shared state machine, so
/// both drivers observe identical launch, timeout, output-bound, and reaping
/// semantics. `request` must outlive the call.
[[nodiscard]] ClipboardCommandResult run_clipboard_command_blocking(const ClipboardCommandRequest& request);

/// pi `emitOsc52`'s exact sequence `\x1b]52;c;<base64>\x07`, or `std::nullopt`
/// when the encoded payload exceeds `kMaxOsc52EncodedLength`.
[[nodiscard]] std::optional<std::string> encode_clipboard_osc52(std::string_view text);

/// One unique clipboard temporary-file token (pi `crypto.randomUUID()`), shared
/// by the `pi-clipboard-*` image files and the `pi-wsl-clip-*` interop files.
[[nodiscard]] std::string make_clipboard_unique_token();

} // namespace cch::coding_agent::tui
