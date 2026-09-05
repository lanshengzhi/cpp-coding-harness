#include <cch/tui/ProcessTerminal.hpp>

#include <cch/tui/TerminalImage.hpp>

#include "tui/InputDecoder.hpp"
#include "support/UniqueFd.hpp"

#include <cch/support/Error.hpp>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <cstdlib>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace cch::tui {
namespace {

// Behavioral baseline: pi 83114817 packages/tui/src/terminal.ts
// (setTitle/setProgress/drainInput, TERMINAL_PROGRESS_* constants, and
// drainInput defaults) and packages/tui/src/terminal-image.ts
// (detectCapabilities env rules, probeTmuxHyperlinks, and the CSI 16 t
// cell-size query that tui.ts queryCellSize sends at startup; the response
// consumption is the terminal seam's sidecar-model counterpart of pi's
// consumeCellSizeResponse).
constexpr std::string_view kBracketedPasteEnable = "\x1b[?2004h";
constexpr std::string_view kBracketedPasteDisable = "\x1b[?2004l";
constexpr std::string_view kCursorShow = "\x1b[?25h";
constexpr std::string_view kCursorHide = "\x1b[?25l";
// pi's resize full-redraw clears screen, homes, and clears scrollback
// (packages/tui/src/terminal.ts clearScreen: `\x1b[2J\x1b[H\x1b[3J`).
constexpr std::string_view kClearScreen = "\x1b[2J\x1b[H\x1b[3J";
constexpr std::string_view kBeginSynchronizedUpdate = "\x1b[?2026h";
constexpr std::string_view kEndSynchronizedUpdate = "\x1b[?2026l";
constexpr std::string_view kKeyboardProtocolPush = "\x1b[>7u";
constexpr std::string_view kKeyboardProtocolQuery = "\x1b[?u\x1b[c";
constexpr std::string_view kKeyboardProtocolPop = "\x1b[<u";
constexpr std::string_view kColorSchemeQuery = "\x1b[?996n";
constexpr std::string_view kBackgroundColorQuery = "\x1b]11;?\x07";
// ADR 0041 anchored absolute flow: the startup DSR cursor-position query
// (`\x1b[6n`) establishes the shell's cursor row as the buffer-to-screen
// origin; the answer is a CPR (`ESC [ rows ; cols R`, 1-based).
constexpr std::string_view kCursorPositionQuery = "\x1b[6n";
constexpr std::string_view kModifyOtherKeysEnable = "\x1b[>4;2m";
constexpr std::string_view kModifyOtherKeysDisable = "\x1b[>4;0m";
constexpr std::string_view kProgressActiveSequence = "\x1b]9;4;3\x07";
constexpr std::string_view kProgressClearSequence = "\x1b]9;4;0;\x07";
constexpr auto kProgressKeepalive = std::chrono::milliseconds(1000);
/// Startup appearance probe window (the color-scheme and OSC 11 background
/// queries run before the delivery worker starts; pi probes synchronously at
/// startup).
constexpr auto kAppearanceProbeTimeout = std::chrono::milliseconds(100);
/// Escape-sequence and negotiation idle flush window: after input leaves a
/// partial protocol/appearance response (or a decoder flush) pending, the
/// delivery worker flushes it once this window passes (was 15 polls at 10 ms
/// in the periodic-polling design; issue #462).
constexpr auto kNegotiationTimeout = std::chrono::milliseconds(150);
/// Startup cursor-position poll bound (ADR 0041): a terminal that does not
/// answer the DSR query within this window falls back to clear-screen + home +
/// scrollback with the origin at row 0.
constexpr auto kCursorPositionTimeout = std::chrono::milliseconds(250);
/// The delivery worker re-reads TIOCGWINSZ at least this often so resizes are
/// detected even without SIGWINCH delivery (it also checks on every wakeup).
/// This is a low-rate watchdog timer, not input polling.
constexpr auto kResizeWatchdogInterval = std::chrono::milliseconds(500);
/// Ordered output queue bound: beyond this many queued bytes the terminal
/// reports explicit backpressure (`Busy`) instead of buffering without limit.
constexpr std::size_t kOutputQueueMaxBytes = 256 * 1024;
/// While stop() is pending, the delivery worker bounds how long it waits for
/// a backed-up output descriptor to drain, so cancellation never hangs on a
/// stuck terminal (undrained queued output is dropped on exit).
constexpr auto kStopDrainTimeout = std::chrono::milliseconds(250);

[[nodiscard]] support::Error process_error(std::string message, std::string_view operation, int error_number) {
    return support::make_error(
        support::ErrorCode::Process,
        std::move(message),
        std::format("{} failed (errno {})", operation, error_number));
}

struct WriteAttempt {
    support::ExpectedVoid result;
    std::size_t bytes_written{0};
};

[[nodiscard]] WriteAttempt attempt_write_all(int descriptor, std::string_view output) {
    WriteAttempt attempt;
    while (attempt.bytes_written < output.size()) {
        const auto written = ::write(
            descriptor,
            output.data() + attempt.bytes_written,
            output.size() - attempt.bytes_written);
        if (written > 0) {
            attempt.bytes_written += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        const auto error_number = written < 0 ? errno : EIO;
        attempt.result = std::unexpected(process_error(
            "Process Terminal could not write terminal output",
            "write",
            error_number));
        return attempt;
    }
    return attempt;
}

[[nodiscard]] support::ExpectedVoid write_all(int descriptor, std::string_view output) {
    return attempt_write_all(descriptor, output).result;
}

[[nodiscard]] std::string lowercase_environment(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    std::string result = value == nullptr ? "" : value;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] std::string_view environment(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::string_view{} : std::string_view{value};
}

[[nodiscard]] bool supports_synchronized_output() {
    const auto terminal = environment("TERM");
    const auto program = environment("TERM_PROGRAM");
    return terminal == "xterm-kitty" || terminal == "alacritty" ||
        terminal == "foot" || terminal == "foot-extra" || terminal == "wezterm" ||
        terminal == "ghostty" || program == "iTerm.app" || program == "WezTerm" ||
        program == "ghostty";
}

[[nodiscard]] TerminalColorCapability detect_color_capability() {
    const auto color_terminal = lowercase_environment("COLORTERM");
    const bool has_true_color_hint = color_terminal == "truecolor" || color_terminal == "24bit";
    const auto terminal = lowercase_environment("TERM");
    if (!environment("TMUX").empty() || terminal.starts_with("tmux") || terminal.starts_with("screen")) {
        return has_true_color_hint ? TerminalColorCapability::TrueColor : TerminalColorCapability::Xterm256;
    }

    const auto program = lowercase_environment("TERM_PROGRAM");
    const auto emulator = lowercase_environment("TERMINAL_EMULATOR");
    const bool known_true_color = !environment("KITTY_WINDOW_ID").empty() || program == "kitty" ||
        program == "ghostty" || terminal.find("ghostty") != std::string::npos ||
        !environment("GHOSTTY_RESOURCES_DIR").empty() || !environment("WEZTERM_PANE").empty() ||
        program == "wezterm" || program == "warpterminal" || !environment("WARP_SESSION_ID").empty() ||
        !environment("WARP_TERMINAL_SESSION_UUID").empty() || !environment("ITERM_SESSION_ID").empty() ||
        program == "iterm.app" || !environment("WT_SESSION").empty() || program == "vscode" ||
        program == "alacritty" || emulator == "jetbrains-jediterm";
    return known_true_color || has_true_color_hint
        ? TerminalColorCapability::TrueColor
        : TerminalColorCapability::Xterm256;
}

struct EnvironmentRgb {
    int red{0};
    int green{0};
    int blue{0};
};

[[nodiscard]] EnvironmentRgb xterm_index_to_rgb(int index) {
    constexpr std::array<EnvironmentRgb, 16> basic{{
        {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
        {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
        {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
        {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    }};
    if (index < 16) return basic[static_cast<std::size_t>(index)];
    if (index < 232) {
        const auto cube = index - 16;
        const auto channel = [](int value) { return value == 0 ? 0 : 55 + value * 40; };
        return {
            .red = channel(cube / 36),
            .green = channel((cube % 36) / 6),
            .blue = channel(cube % 6),
        };
    }
    const auto gray = 8 + (index - 232) * 10;
    return {.red = gray, .green = gray, .blue = gray};
}

[[nodiscard]] double linear_channel(int channel) {
    const auto value = static_cast<double>(channel) / 255.0;
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] TerminalAppearance appearance_for_index(int index) {
    const auto rgb = xterm_index_to_rgb(index);
    const auto luminance = 0.2126 * linear_channel(rgb.red) +
        0.7152 * linear_channel(rgb.green) + 0.0722 * linear_channel(rgb.blue);
    return luminance >= 0.5 ? TerminalAppearance::Light : TerminalAppearance::Dark;
}

[[nodiscard]] TerminalAppearance detect_terminal_appearance() {
    const auto value = environment("COLORFGBG");
    std::optional<int> background;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto separator = value.find(';', start);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        auto part = value.substr(start, end - start);
        const auto first = part.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            part = {};
        } else {
            const auto last = part.find_last_not_of(" \t\r\n");
            part = part.substr(first, last - first + 1);
        }
        if (part.starts_with('+')) part.remove_prefix(1);
        if (!part.empty()) {
            int parsed = 0;
            const auto [pointer, error] = std::from_chars(part.data(), part.data() + part.size(), parsed);
            (void)pointer; // pi baseline parseInt semantics intentionally accept a valid numeric prefix.
            if (error == std::errc{} && parsed >= 0 && parsed <= 255) background = parsed;
        }
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return background ? appearance_for_index(*background) : TerminalAppearance::Unknown;
}

/// Startup probe state shared by the appearance and cursor-position probe
/// phases: one stream decoder accumulates the pre-worker byte stream, so
/// responses split across reads reassemble in the decoder's single fragment
/// buffer and every other byte is preserved verbatim for the input sink.
struct StartupProbe {
    detail::TerminalStreamDecoder decoder;
    std::optional<TerminalAppearance> color_scheme{std::nullopt};
    std::optional<TerminalAppearance> background{std::nullopt};
    /// The reported cursor position (0-based) when a CPR arrived in time.
    std::optional<CursorPosition> position{std::nullopt};
    std::string forwarded_input;
};

void collect_probe_responses(
    StartupProbe& probe,
    const std::vector<detail::TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        if (const auto* position = std::get_if<CursorPosition>(&response)) {
            probe.position = *position;
            continue;
        }
        if (const auto* scheme = std::get_if<detail::ColorSchemeResponse>(&response)) {
            if (scheme->kind == detail::ColorSchemeResponseKind::ColorScheme) {
                probe.color_scheme = scheme->appearance;
            } else {
                probe.background = scheme->appearance;
            }
            continue;
        }
        // Cell-size and keyboard negotiation answers cannot arrive before
        // their queries are sent (both follow the probes below): any such
        // response here is unsolicited and ignored.
    }
}

/// Poll the input descriptor until the timeout passes, feeding every chunk
/// through the probe's decoder (pi's synchronous startup probes). The
/// appearance probe always runs out its full window; the cursor-position
/// probe returns early once the DSR answer arrives (ADR 0041).
void poll_startup_probe(
    StartupProbe& probe,
    int descriptor,
    std::chrono::milliseconds timeout,
    bool stop_at_cursor_position) {
    constexpr std::size_t kProbeReadChunkBytes{4096};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (stop_at_cursor_position && probe.position) return;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return;
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) return;
        if ((item.revents & POLLIN) == 0) return;
        std::array<char, kProbeReadChunkBytes> buffer{};
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (count == 0) return;
        auto decoded = probe.decoder.feed(
            std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        probe.forwarded_input += decoded.forwarded_input;
        collect_probe_responses(probe, decoded.responses);
    }
}

} // namespace

struct ProcessTerminal::Impl {
    explicit Impl(ProcessTerminalOptions configured_options)
        : options(configured_options) {}

    ProcessTerminalOptions options;
    mutable std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    bool stop_in_progress{false};
    mutable std::mutex mutex;
    TerminalDimensions dimensions;
    TerminalCapabilities capabilities;
    TerminalModeState modes;
    std::shared_ptr<TerminalInputSink> input_sink;
    std::shared_ptr<TerminalResizeSink> resize_sink;
    std::string startup_input;
    bool startup_color_scheme_reported{false};
    std::jthread worker;
    std::optional<support::Error> worker_error;
    bool keyboard_protocol_pushed{false};
    bool modify_other_keys_active{false};
    std::size_t synchronized_update_depth{0};
    /// Output staged between begin/end so a render is admitted as one ordered
    /// queue item. This keeps a backed-up terminal from displaying or
    /// partially admitting a frame that cannot be retried atomically.
    std::string synchronized_output;
    struct SynchronizedOutputState {
        CursorPosition cursor;
        std::size_t scroll_origin{0};
        std::size_t viewport_top{0};
        std::uint64_t next_image_handle{1};
        std::size_t margin_top{0};
        std::size_t margin_bottom{0};
        bool margins_active{false};
    };
    std::optional<SynchronizedOutputState> synchronized_output_state;
    bool progress_active{false};
    std::chrono::steady_clock::time_point progress_next_keepalive{};
    std::uint64_t next_image_handle{1};
    bool draining{false};
    std::chrono::steady_clock::time_point drain_last_activity{};
    CursorPosition cursor{};
    std::size_t margin_top{0};
    std::size_t margin_bottom{0};
    bool margins_active{false};
    /// Screen row where buffer row 0 was first written: the shell's cursor row
    /// at startup (0-based), established once by the startup cursor-position
    /// probe (ADR 0041 anchored absolute flow; the DSR timeout fallback clears
    /// the screen and anchors at row 0).
    std::size_t scroll_origin{0};
    /// Total native scrolls since the anchor, mirroring VirtualTerminal's
    /// scroll emulation: the renderer writes the full composed buffer with
    /// buffer-relative rows under the main-screen scrollback flow, and rows at
    /// or past the visible viewport bottom advance this count while
    /// `set_cursor` emits pi's CRLF line flow so the real terminal's native
    /// scrollback receives the overflow. The screen row of buffer row b is
    /// `scroll_origin + b - viewport_top`; the first visible buffer row is
    /// `max(0, viewport_top - scroll_origin)`. `set_cursor`/`place_image`
    /// convert buffer rows to screen rows using this mapping before emitting
    /// ANSI sequences.
    std::size_t viewport_top{0};
    termios original_termios{};
    bool has_original_termios{false};
    /// Wakeup pipe polled by the delivery worker alongside the input
    /// descriptor: stop() and output enqueue write one byte so the worker's
    /// blocking readiness wait returns immediately (issue #462) instead of
    /// periodic polling.
    cch::support::UniqueFd wakeup_read;
    cch::support::UniqueFd wakeup_write;
    /// Original descriptor flags (O_NONBLOCK is added to the output
    /// descriptor at start and restored on every exit path).
    int original_fd_flags{-1};
    bool output_nonblock{false};
    /// Ordered bounded output queue drained by the delivery worker when the
    /// output descriptor is writable. The queue gives explicit backpressure
    /// (`Busy`) instead of blocking the caller indefinitely on a backed-up
    /// terminal.
    std::deque<std::string> output_queue;
    std::size_t output_queued_bytes{0};
    bool output_draining{false};
    /// Resize watchdog deadline base, owned by the worker thread: the worker
    /// wakes at least this often to re-read TIOCGWINSZ, so resizes are
    /// detected even without SIGWINCH delivery.
    std::chrono::steady_clock::time_point last_resize_check{};
};

namespace {

template <typename T>
[[nodiscard]] support::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Process Terminal must be started before terminal operations"));
}

[[nodiscard]] support::Expected<TerminalDimensions> read_dimensions(int descriptor) {
    winsize size{};
    if (::ioctl(descriptor, TIOCGWINSZ, &size) != 0) {
        return std::unexpected(process_error(
            "Process Terminal could not read terminal dimensions",
            "ioctl(TIOCGWINSZ)",
            errno));
    }
    if (size.ws_col == 0 || size.ws_row == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal requires positive terminal dimensions"));
    }
    return TerminalDimensions{
        .columns = size.ws_col,
        .rows = size.ws_row,
    };
}

[[nodiscard]] std::string describe_error(const support::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] support::Error combine_errors(
    support::Error primary,
    const support::Error& secondary,
    std::string message) {
    return support::make_error(
        primary.code,
        std::move(message),
        std::format(
            "primary: {}; secondary: {}",
            describe_error(primary),
            describe_error(secondary)));
}

void retain_error(support::ExpectedVoid& accumulated, support::ExpectedVoid candidate) {
    if (candidate) return;
    if (accumulated) {
        accumulated = std::unexpected(candidate.error());
        return;
    }
    accumulated = std::unexpected(combine_errors(
        std::move(accumulated.error()),
        candidate.error(),
        "Process Terminal restoration encountered multiple failures"));
}

[[nodiscard]] support::Error startup_failure(
    support::Error acquisition_error,
    const support::ExpectedVoid& rollback) {
    if (rollback) return acquisition_error;
    return combine_errors(
        std::move(acquisition_error),
        rollback.error(),
        "Process Terminal startup failed and rollback was incomplete");
}

template <typename T>
void record_worker_error(T& impl, support::Error error) {
    std::lock_guard lock(impl.mutex);
    if (!impl.worker_error) impl.worker_error = std::move(error);
}

template <typename T>
void invoke_input(T& impl, std::string input) {
    std::shared_ptr<TerminalInputSink> sink;
    {
        std::lock_guard lock(impl.mutex);
        if (impl.draining) return;
        sink = impl.input_sink;
    }
    if (!sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        if (auto delivered = (*sink)(std::move(input)); !delivered) {
            record_worker_error(impl, std::move(delivered.error()));
            std::lock_guard lock(impl.mutex);
            if (impl.input_sink == sink) impl.input_sink.reset();
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception&) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal input sink failed",
            "the input callback threw an exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.input_sink == sink) impl.input_sink.reset();
    } catch (...) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal input sink failed",
            "the input callback threw an unknown exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.input_sink == sink) impl.input_sink.reset();
    }
#endif
}

template <typename T>
void deliver_resize_if_changed(T& impl) {
    // The watchdog base advances on every check so the poll timeout stays in
    // the future and the worker never spins on an immediate resize deadline.
    impl.last_resize_check = std::chrono::steady_clock::now();
    winsize size{};
    if (::ioctl(impl.options.output_fd, TIOCGWINSZ, &size) != 0 ||
        size.ws_col == 0 || size.ws_row == 0) {
        return;
    }
    const TerminalDimensions dimensions{
        .columns = size.ws_col,
        .rows = size.ws_row,
    };
    std::shared_ptr<TerminalResizeSink> sink;
    {
        std::lock_guard lock(impl.mutex);
        if (impl.dimensions == dimensions) return;
        impl.dimensions = dimensions;
        sink = impl.resize_sink;
    }
    if (!sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        if (auto delivered = (*sink)(dimensions); !delivered) {
            record_worker_error(impl, std::move(delivered.error()));
            std::lock_guard lock(impl.mutex);
            if (impl.resize_sink == sink) impl.resize_sink.reset();
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception&) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal resize sink failed",
            "the resize callback threw an exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    } catch (...) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal resize sink failed",
            "the resize callback threw an unknown exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    }
#endif
}

template <typename T>
void enable_modify_other_keys(T& impl) {
    if (impl.capabilities.keyboard_protocol == KeyboardProtocol::Kitty ||
        impl.modify_other_keys_active) {
        return;
    }
    if (auto enabled = write_all(impl.options.output_fd, kModifyOtherKeysEnable); !enabled) {
        if (!impl.worker_error) impl.worker_error = enabled.error();
        return;
    }
    impl.modify_other_keys_active = true;
    impl.capabilities.keyboard_protocol = KeyboardProtocol::ModifyOtherKeys;
}

template <typename T>
void apply_keyboard_response(T& impl, const detail::KeyboardProtocolResponse& response) {
    std::lock_guard lock(impl.mutex);
    if (response.kind == detail::KeyboardProtocolResponseKind::DeviceAttributes ||
        response.flags == 0) {
        enable_modify_other_keys(impl);
        return;
    }
    if (impl.modify_other_keys_active) {
        if (auto disabled = write_all(impl.options.output_fd, kModifyOtherKeysDisable); !disabled) {
            if (!impl.worker_error) impl.worker_error = disabled.error();
            return;
        }
        impl.modify_other_keys_active = false;
    }
    impl.capabilities.keyboard_protocol = KeyboardProtocol::Kitty;
}

template <typename T>
void emit_progress_keepalive(T& impl) {
    std::lock_guard lock(impl.mutex);
    if (!impl.progress_active) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < impl.progress_next_keepalive) return;
    // While the ordered queue is non-empty or a synchronized render frame is
    // being staged, a direct keepalive write could interleave ahead of the
    // render bytes and truncate an escape sequence. Skip this beat and retry
    // on the next deadline (the indicator is best-effort and re-emitted).
    if (!impl.output_queue.empty() || impl.output_draining ||
        impl.synchronized_update_depth > 0) {
        impl.progress_next_keepalive = now + kProgressKeepalive;
        return;
    }
    // Non-blocking best-effort write: on any non-delivery outcome, re-arm the
    // keepalive so the next deadline (and poll timeout) stays in the future
    // and the worker never spins on an immediate deadline.
    const auto written = ::write(
        impl.options.output_fd,
        kProgressActiveSequence.data(),
        kProgressActiveSequence.size());
    if (written == static_cast<ssize_t>(kProgressActiveSequence.size())) {
        impl.progress_next_keepalive = now + kProgressKeepalive;
        return;
    }
    if (written < 0 && errno != EINTR && !impl.worker_error) {
        impl.worker_error = process_error(
            "Process Terminal could not write terminal output",
            "write",
            errno);
    }
    impl.progress_next_keepalive = now + kProgressKeepalive;
}

/// Wake the delivery worker's blocking readiness wait (stop, output enqueue,
/// or another worker-relevant state change). Best-effort; a full pipe is
/// drained on the next wakeup and cannot wedge the wait.
template <typename T>
void wake_worker(T& impl) {
    constexpr char kWakeByte = 1;
    (void)::write(impl.wakeup_write.get(), &kWakeByte, 1);
}

/// Whether ordered output is still pending (queued or being drained).
template <typename T>
bool output_pending(const T& impl) {
    std::lock_guard lock(impl.mutex);
    return !impl.output_queue.empty() || impl.output_draining;
}

/// Submit one ordered output chunk through the bounded queue. Caller holds
/// `impl.mutex`. The common fast path writes the whole chunk inline when the
/// queue is empty and the descriptor accepts it; partial writes and
/// backpressure fall into the bounded FIFO the delivery worker drains in
/// order. The bound caps the accumulated backlog: once the queue is non-empty
/// (the terminal is behind), writes that would push it past `kOutputQueueMaxBytes`
/// are rejected with explicit backpressure (`Busy`) instead of blocking the
/// caller. A single in-flight write is always admitted when the queue is
/// empty, so a large one-shot render (e.g. an inline image) is never rejected
/// on a healthy, draining terminal (issue #462).
template <typename T>
[[nodiscard]] support::ExpectedVoid enqueue_output(T& impl, std::string_view bytes) {
    if (bytes.empty()) return {};
    if (impl.synchronized_update_depth > 0) {
        impl.synchronized_output.append(bytes);
        return {};
    }
    if (impl.output_queue.empty() && !impl.output_draining) {
        const auto written = ::write(impl.options.output_fd, bytes.data(), bytes.size());
        if (written > 0 && static_cast<std::size_t>(written) == bytes.size()) return {};
        if (written > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(written));
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return std::unexpected(process_error(
                "Process Terminal could not write terminal output",
                "write",
                errno));
        }
        // EINTR with no bytes, or a partial write: the remainder is queued.
    }
    if (!impl.output_queue.empty() &&
        impl.output_queued_bytes + bytes.size() > kOutputQueueMaxBytes) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Busy,
            "Process Terminal output is backed up",
            std::format(
                "the bounded output queue ({} bytes) cannot admit more output",
                kOutputQueueMaxBytes)));
    }
    impl.output_queue.push_back(std::string(bytes));
    impl.output_queued_bytes += bytes.size();
    wake_worker(impl);
    return {};
}

/// Drain the ordered output queue into the output descriptor while it is
/// writable. Non-blocking writes run under `impl.mutex`, so ordering with
/// enqueue_output is exact; a real write failure records the worker error and
/// drops the undeliverable queued bytes.
template <typename T>
void drain_output(T& impl, bool writable) {
    std::lock_guard lock(impl.mutex);
    if (impl.output_queue.empty() || !writable) return;
    impl.output_draining = true;
    while (!impl.output_queue.empty()) {
        auto& front = impl.output_queue.front();
        const auto written = ::write(impl.options.output_fd, front.data(), front.size());
        if (written > 0) {
            impl.output_queued_bytes -= static_cast<std::size_t>(written);
            if (static_cast<std::size_t>(written) == front.size()) {
                impl.output_queue.pop_front();
                continue;
            }
            front.erase(0, static_cast<std::size_t>(written));
            break;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && errno == EAGAIN) break;
        if (written < 0) {
            if (!impl.worker_error) {
                impl.worker_error = process_error(
                    "Process Terminal could not write terminal output",
                    "write",
                    errno);
            }
            impl.output_queued_bytes = 0;
            impl.output_queue.clear();
        }
        break;
    }
    impl.output_draining = false;
}

enum class WorkerEventKind {
    Timeout,
    Input,
    Closed,
    Error,
};

struct WorkerEvent {
    WorkerEventKind kind{WorkerEventKind::Timeout};
    std::string input;
    std::optional<support::Error> error{std::nullopt};
};

/// What one blocking readiness wait observed. The worker drains the wakeup
/// pipe itself, so callers never observe wakeup bytes directly.
struct WorkerWake {
    /// poll() returned 0: at least one armed deadline (escape-sequence idle
    /// flush, progress keepalive, or the resize watchdog) passed.
    bool timed_out{false};
    /// The output descriptor was reported writable, so queued output can be
    /// drained in order.
    bool output_writable{false};
    /// EOF on the input descriptor (the terminal side closed).
    bool input_closed{false};
    /// Bytes read from the input descriptor (empty when none were available).
    std::string input;
    std::optional<support::Error> error{std::nullopt};
};

struct WorkerInputState {
    detail::TerminalStreamDecoder decoder;
    bool color_scheme_reported{false};
    bool needs_input_flush{false};
    /// Readiness-driven deadline: armed (now + kNegotiationTimeout) whenever
    /// input leaves a decoder fragment or forwarded bytes that Tui must flush.
    std::chrono::steady_clock::time_point negotiation_deadline{
        std::chrono::steady_clock::time_point::max()};
};

[[nodiscard]] bool deadline_fired(std::chrono::steady_clock::time_point deadline) {
    return deadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= deadline;
}

/// Readiness-driven wait (issue #462): block on the input descriptor, the
/// wakeup pipe, and (while output is pending) the output descriptor, with a
/// timeout only when a timer deadline (escape idle flush, keepalive, resize
/// watchdog) is armed. Periodic input polling is replaced by this blocking
/// wait; stop() and output enqueue write the wakeup pipe so the wait returns
/// immediately. While stopping, the wait is capped so a backed-up output
/// descriptor cannot hang the worker join.
template <typename T>
[[nodiscard]] WorkerWake wait_for_worker_events(
    T& impl,
    const WorkerInputState& state,
    bool stopping) {
    WorkerWake wake;
    bool output_pending = false;
    std::optional<std::chrono::steady_clock::time_point> earliest;
    {
        std::lock_guard lock(impl.mutex);
        output_pending = !impl.output_queue.empty() || impl.output_draining;
        if (impl.progress_active) {
            if (!earliest || impl.progress_next_keepalive < *earliest) {
                earliest = impl.progress_next_keepalive;
            }
        }
        const auto resize_watchdog = impl.last_resize_check + kResizeWatchdogInterval;
        if (!earliest || resize_watchdog < *earliest) earliest = resize_watchdog;
    }
    if (state.negotiation_deadline != std::chrono::steady_clock::time_point::max()) {
        if (!earliest || state.negotiation_deadline < *earliest) earliest = state.negotiation_deadline;
    }

    std::array<pollfd, 3> items{};
    std::size_t count = 0;
    items[count++] = pollfd{.fd = impl.options.input_fd, .events = POLLIN, .revents = 0};
    items[count++] = pollfd{.fd = impl.wakeup_read.get(), .events = POLLIN, .revents = 0};
    if (output_pending) {
        items[count++] = pollfd{.fd = impl.options.output_fd, .events = POLLOUT, .revents = 0};
    }

    int timeout_ms = -1;
    if (earliest) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *earliest - std::chrono::steady_clock::now());
        timeout_ms = std::max<int>(0, static_cast<int>(remaining.count()));
    }
    if (stopping &&
        (timeout_ms < 0 || timeout_ms > kStopDrainTimeout.count())) {
        timeout_ms = static_cast<int>(kStopDrainTimeout.count());
    }

    const auto ready = ::poll(items.data(), static_cast<nfds_t>(count), timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) return wake; // signal interrupted the wait; retry.
        wake.error = process_error("Process Terminal input wait failed", "poll", errno);
        return wake;
    }
    if (ready == 0) {
        wake.timed_out = true;
        return wake;
    }

    if ((items[1].revents & POLLIN) != 0) {
        std::array<char, 64> buffer{};
        while (::read(impl.wakeup_read.get(), buffer.data(), buffer.size()) > 0) {}
    }
    if (count > 2 && (items[2].revents & POLLOUT) != 0) {
        wake.output_writable = true;
    }
    if ((items[0].revents & POLLIN) != 0) {
        std::array<char, 4096> buffer{};
        const auto read_count = ::read(impl.options.input_fd, buffer.data(), buffer.size());
        if (read_count > 0) {
            wake.input.assign(buffer.data(), static_cast<std::size_t>(read_count));
        } else if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // O_NONBLOCK is shared between input and output when they are the
            // same descriptor; another wake consumed the readable data.
        } else if (read_count < 0 && errno != EINTR) {
            wake.error = process_error("Process Terminal input read failed", "read", errno);
        } else if (read_count == 0) {
            wake.input_closed = true;
        }
    } else if ((items[0].revents & (POLLHUP | POLLERR)) != 0) {
        // The terminal side went away without readable data (hangup): treat
        // it as EOF so the worker stops cleanly instead of spinning.
        wake.input_closed = true;
    }
    return wake;
}

template <typename T>
void apply_cell_size_response(
    T& impl,
    const detail::CellSizeResponse& response) {
    if (response.height_px == 0 || response.width_px == 0) return;
    std::shared_ptr<TerminalResizeSink> sink;
    TerminalDimensions dimensions;
    {
        std::lock_guard lock(impl.mutex);
        const CellPixelDimensions updated{
            .width = response.width_px,
            .height = response.height_px,
        };
        if (impl.capabilities.cell_pixels && *impl.capabilities.cell_pixels == updated) {
            return;
        }
        impl.capabilities.cell_pixels = updated;
        sink = impl.resize_sink;
        dimensions = impl.dimensions;
    }
    if (!sink || !*sink) return;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        if (auto delivered = (*sink)(dimensions); !delivered) {
            record_worker_error(impl, std::move(delivered.error()));
            std::lock_guard lock(impl.mutex);
            if (impl.resize_sink == sink) impl.resize_sink.reset();
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception&) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal resize sink failed",
            "the resize callback threw an exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    } catch (...) {
        record_worker_error(impl, support::make_error(
            support::ErrorCode::Unknown,
            "Process Terminal resize sink failed",
            "the resize callback threw an unknown exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    }
#endif
}

template <typename T>
void apply_terminal_responses(
    T& impl,
    WorkerInputState& state,
    const std::vector<detail::TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        if (const auto* cell_size = std::get_if<detail::CellSizeResponse>(&response)) {
            apply_cell_size_response(impl, *cell_size);
            continue;
        }
        if (const auto* keyboard = std::get_if<detail::KeyboardProtocolResponse>(&response)) {
            apply_keyboard_response(impl, *keyboard);
            continue;
        }
        if (std::get_if<CursorPosition>(&response) != nullptr) {
            // A CPR arriving on the worker is a late or duplicate answer to
            // the startup DSR query. The anchor is established once at
            // startup, so consume and ignore it (ADR 0041).
            continue;
        }
        const auto* appearance = std::get_if<detail::ColorSchemeResponse>(&response);
        if (appearance == nullptr) continue;
        if (appearance->kind == detail::ColorSchemeResponseKind::ColorScheme) {
            std::lock_guard lock(impl.mutex);
            impl.capabilities.appearance = appearance->appearance;
            state.color_scheme_reported = true;
        } else if (!state.color_scheme_reported) {
            std::lock_guard lock(impl.mutex);
            impl.capabilities.appearance = appearance->appearance;
        }
    }
}

template <typename T>
[[nodiscard]] bool handle_worker_event(
    T& impl,
    WorkerInputState& state,
    WorkerEvent event) {
    if (event.kind == WorkerEventKind::Closed) return false;
    if (event.kind == WorkerEventKind::Error) {
        record_worker_error(impl, std::move(*event.error));
        return false;
    }
    if (event.kind == WorkerEventKind::Input) {
        if (impl.draining) {
            std::lock_guard lock(impl.mutex);
            impl.drain_last_activity = std::chrono::steady_clock::now();
            return true;
        }
        state.negotiation_deadline = std::chrono::steady_clock::now() + kNegotiationTimeout;
        auto decoded = state.decoder.feed(event.input);
        apply_terminal_responses(impl, state, decoded.responses);
        if (!decoded.forwarded_input.empty()) {
            invoke_input(impl, std::move(decoded.forwarded_input));
            state.needs_input_flush = true;
        }
        return true;
    }

    // A timeout wake ends the single decoder fragment window. Appearance
    // fragments are dropped by flush(); all other fragments are forwarded
    // verbatim and Tui is then asked to flush its event view.
    if (deadline_fired(state.negotiation_deadline)) {
        auto decoded = state.decoder.flush();
        apply_terminal_responses(impl, state, decoded.responses);
        if (!decoded.forwarded_input.empty()) {
            invoke_input(impl, std::move(decoded.forwarded_input));
            state.needs_input_flush = true;
        }
        if (state.needs_input_flush) {
            invoke_input(impl, {});
            state.needs_input_flush = false;
        }
        state.negotiation_deadline = std::chrono::steady_clock::time_point::max();
    }
    return true;
}

template <typename T>
void run_terminal_worker(T& impl, std::stop_token stop_token) {
    WorkerInputState input_state;
    std::string startup_input;
    {
        std::lock_guard lock(impl.mutex);
        startup_input = std::move(impl.startup_input);
        input_state.color_scheme_reported = impl.startup_color_scheme_reported;
    }
    if (!startup_input.empty()) {
        auto decoded = input_state.decoder.feed(startup_input);
        apply_terminal_responses(impl, input_state, decoded.responses);
        if (!decoded.forwarded_input.empty()) {
            invoke_input(impl, std::move(decoded.forwarded_input));
            input_state.needs_input_flush = true;
            input_state.negotiation_deadline =
                std::chrono::steady_clock::now() + kNegotiationTimeout;
        }
    }
    auto stop_drain_deadline = std::chrono::steady_clock::time_point::max();
    while (true) {
        const bool stopping = stop_token.stop_requested();
        if (stopping &&
            stop_drain_deadline == std::chrono::steady_clock::time_point::max()) {
            stop_drain_deadline = std::chrono::steady_clock::now() + kStopDrainTimeout;
        }
        auto wake = wait_for_worker_events(impl, input_state, stopping);
        if (wake.error) {
            record_worker_error(impl, std::move(*wake.error));
            break;
        }
        if (wake.input_closed) break;
        if (!wake.input.empty()) {
            if (!handle_worker_event(
                    impl,
                    input_state,
                    WorkerEvent{
                        .kind = WorkerEventKind::Input,
                        .input = std::move(wake.input),
                        .error = std::nullopt,
                    })) {
                break;
            }
        } else if (wake.timed_out) {
            if (!handle_worker_event(
                    impl,
                    input_state,
                    WorkerEvent{
                        .kind = WorkerEventKind::Timeout,
                        .input = {},
                        .error = std::nullopt,
                    })) {
                break;
            }
        }
        if (!stopping) {
            deliver_resize_if_changed(impl);
            emit_progress_keepalive(impl);
        }
        drain_output(impl, wake.output_writable);
        if (stopping) {
            // Cancellation is prompt: the wakeup pipe wakes the blocking wait
            // immediately. The worker then keeps draining ordered output until
            // the queue is empty or the bounded cap elapses, so a healthy
            // terminal receives its complete final bytes before stop()
            // restores termios and descriptor state; a stuck output
            // descriptor cannot hang the join beyond the cap.
            if (!output_pending(impl)) break;
            if (std::chrono::steady_clock::now() >= stop_drain_deadline) break;
            continue;
        }
    }
}

template <typename T>
[[nodiscard]] support::ExpectedVoid restore_terminal_modes(T& impl) {
    support::ExpectedVoid first_error;

    // Restore the output descriptor to its original flags first so the
    // restoration writes below run on a blocking descriptor (the worker has
    // already drained the ordered output queue before stop() restores).
    if (impl.output_nonblock && impl.original_fd_flags >= 0) {
        if (::fcntl(impl.options.output_fd, F_SETFL, impl.original_fd_flags) != 0) {
            retain_error(
                first_error,
                std::unexpected(process_error(
                    "Process Terminal could not restore output descriptor flags",
                    "fcntl(F_SETFL)",
                    errno)));
        } else {
            impl.output_nonblock = false;
        }
    }

    if (impl.progress_active) {
        auto cleared = write_all(impl.options.output_fd, kProgressClearSequence);
        if (cleared) impl.progress_active = false;
        retain_error(first_error, std::move(cleared));
    }
    if (impl.synchronized_update_depth > 0) {
        auto ended = write_all(impl.options.output_fd, kEndSynchronizedUpdate);
        if (ended) impl.synchronized_update_depth = 0;
        retain_error(first_error, std::move(ended));
    }
    if (!impl.modes.cursor_visible) {
        auto shown = write_all(impl.options.output_fd, kCursorShow);
        if (shown) impl.modes.cursor_visible = true;
        retain_error(first_error, std::move(shown));
    }
    if (impl.modify_other_keys_active) {
        auto disabled = write_all(impl.options.output_fd, kModifyOtherKeysDisable);
        if (disabled) impl.modify_other_keys_active = false;
        retain_error(first_error, std::move(disabled));
    }
    if (impl.keyboard_protocol_pushed) {
        auto popped = write_all(impl.options.output_fd, kKeyboardProtocolPop);
        if (popped) impl.keyboard_protocol_pushed = false;
        retain_error(first_error, std::move(popped));
    }
    if (!impl.modify_other_keys_active && !impl.keyboard_protocol_pushed) {
        impl.capabilities.keyboard_protocol = KeyboardProtocol::Legacy;
    }
    if (impl.modes.bracketed_paste) {
        auto disabled = write_all(impl.options.output_fd, kBracketedPasteDisable);
        if (disabled) impl.modes.bracketed_paste = false;
        retain_error(first_error, std::move(disabled));
    }
    if (impl.margins_active) {
        auto reset_margins = write_all(impl.options.output_fd, "\x1b[r");
        if (reset_margins) {
            impl.margins_active = false;
            impl.margin_top = 0;
            impl.margin_bottom = 0;
        }
        retain_error(first_error, std::move(reset_margins));
    }
    if (impl.has_original_termios) {
        if (::tcsetattr(impl.options.input_fd, TCSAFLUSH, &impl.original_termios) == 0) {
            impl.has_original_termios = false;
            impl.modes.raw_input = false;
        } else {
            retain_error(
                first_error,
                std::unexpected(process_error(
                    "Process Terminal could not restore input mode",
                    "tcsetattr",
                    errno)));
        }
    }

    return first_error;
}

} // namespace

ProcessTerminal::ProcessTerminal(ProcessTerminalOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

ProcessTerminal::~ProcessTerminal() {
    (void)stop();
}

support::ExpectedVoid ProcessTerminal::start(
    TerminalInputSink input_sink,
    TerminalResizeSink resize_sink) {
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
    impl_->lifecycle_cv.wait(lifecycle_lock, [this] { return !impl_->stop_in_progress; });
    std::lock_guard lock(impl_->mutex);
    if (impl_->modes.started) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal is already started"));
    }
    if (impl_->worker.joinable()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal delivery worker is still stopping"));
    }
    if (impl_->has_original_termios || impl_->modes.bracketed_paste ||
        !impl_->modes.cursor_visible || impl_->keyboard_protocol_pushed ||
        impl_->modify_other_keys_active || impl_->synchronized_update_depth > 0 ||
        impl_->progress_active) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "Process Terminal has unrestored state from a previous stop"));
    }
    if (::isatty(impl_->options.input_fd) != 1 || ::isatty(impl_->options.output_fd) != 1) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal requires TTY input and output descriptors",
            std::format(
                "input fd {} and output fd {} must both refer to terminals",
                impl_->options.input_fd,
                impl_->options.output_fd)));
    }

    termios original{};
    if (::tcgetattr(impl_->options.input_fd, &original) != 0) {
        return std::unexpected(process_error(
            "Process Terminal could not snapshot input mode",
            "tcgetattr",
            errno));
    }
    auto dimensions = read_dimensions(impl_->options.output_fd);
    if (!dimensions) return std::unexpected(dimensions.error());
    impl_->startup_input.clear();
    impl_->startup_color_scheme_reported = false;
    const auto environment_appearance = detect_terminal_appearance();
    const auto detected_images = detect_image_capabilities();
    impl_->capabilities = TerminalCapabilities{
        .synchronized_output = supports_synchronized_output(),
        .inline_images = detected_images.images,
        .hyperlinks = detected_images.hyperlinks,
        .color = detect_color_capability(),
        .appearance = environment_appearance,
    };
    auto owned_input_sink = std::make_shared<TerminalInputSink>(std::move(input_sink));
    auto owned_resize_sink = std::make_shared<TerminalResizeSink>(std::move(resize_sink));
    // One startup-failure path: roll back acquired terminal state and combine
    // the acquisition error with any incomplete rollback (issue #462).
    const auto fail_startup = [this](support::Error acquisition_error) -> support::ExpectedVoid {
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(
            startup_failure(std::move(acquisition_error), rollback));
    };

    auto raw = original;
    ::cfmakeraw(&raw);
    if (::tcsetattr(impl_->options.input_fd, TCSAFLUSH, &raw) != 0) {
        return std::unexpected(process_error(
            "Process Terminal could not acquire raw input mode",
            "tcsetattr",
            errno));
    }
    impl_->original_termios = original;
    impl_->has_original_termios = true;
    impl_->modes.raw_input = true;

    auto paste_attempt = attempt_write_all(impl_->options.output_fd, kBracketedPasteEnable);
    if (!paste_attempt.result) {
        return fail_startup(paste_attempt.result.error());
    }
    impl_->modes.bracketed_paste = true;

    auto appearance_query = attempt_write_all(
        impl_->options.output_fd,
        std::string(kColorSchemeQuery) + std::string(kBackgroundColorQuery));
    if (!appearance_query.result) {
        return fail_startup(appearance_query.result.error());
    }
    StartupProbe probe;
    poll_startup_probe(
        probe,
        impl_->options.input_fd,
        kAppearanceProbeTimeout,
        false);
    impl_->capabilities.appearance = probe.color_scheme.value_or(
        probe.background.value_or(environment_appearance));

    // ADR 0041 anchored absolute flow (issue #476): query the cursor position
    // once (DSR `\x1b[6n`) through the same blocking pre-worker write path and
    // poll for the CPR response. The reported row anchors the buffer-to-screen
    // origin (`scroll_origin`), so the first frame lands at the shell's cursor
    // row instead of a hard-coded screen row 0, and the tracked cursor becomes
    // the real hardware position for later relative math.
    auto cursor_query = attempt_write_all(impl_->options.output_fd, kCursorPositionQuery);
    if (!cursor_query.result) {
        return fail_startup(cursor_query.result.error());
    }
    poll_startup_probe(
        probe,
        impl_->options.input_fd,
        kCursorPositionTimeout,
        true);
    impl_->capabilities.appearance = probe.color_scheme.value_or(
        probe.background.value_or(environment_appearance));
    if (probe.position) {
        impl_->scroll_origin = probe.position->row;
        impl_->viewport_top = 0;
        impl_->cursor = *probe.position;
    } else {
        // DSR timeout fallback (ADR 0041): clear screen, home, and clear
        // scrollback, then anchor at row 0 — deterministic on terminals that
        // do not answer DSR, at the cost of losing on-screen shell content.
        auto cleared = attempt_write_all(impl_->options.output_fd, kClearScreen);
        if (!cleared.result) {
            return fail_startup(cleared.result.error());
        }
        impl_->scroll_origin = 0;
        impl_->viewport_top = 0;
        impl_->cursor = {};
    }
    auto flushed_probe = probe.decoder.flush();
    probe.forwarded_input += std::move(flushed_probe.forwarded_input);
    impl_->startup_color_scheme_reported = probe.color_scheme.has_value();
    impl_->startup_input = std::move(probe.forwarded_input);

    auto keyboard_push = attempt_write_all(impl_->options.output_fd, kKeyboardProtocolPush);
    if (!keyboard_push.result) {
        return fail_startup(keyboard_push.result.error());
    }
    impl_->keyboard_protocol_pushed = true;

    auto keyboard_query = attempt_write_all(impl_->options.output_fd, kKeyboardProtocolQuery);
    if (!keyboard_query.result) {
        return fail_startup(keyboard_query.result.error());
    }

    if (detected_images.images != InlineImageProtocol::None) {
        // Query cell size in pixels (CSI 16 t) exactly as pi's TUI does at
        // startup when images are supported; the response updates
        // capabilities().cell_pixels and triggers a re-render.
        auto cell_size_query = attempt_write_all(impl_->options.output_fd, detail::kCellSizeQuery);
        if (!cell_size_query.result) {
            return fail_startup(cell_size_query.result.error());
        }
    }

    // Readiness-driven wakeup (issue #462): a pipe the delivery worker polls
    // alongside the input descriptor. stop() and output enqueue write one byte
    // so the blocking readiness wait returns immediately; a fresh pipe per
    // start session avoids stale wakeup bytes across restarts.
    {
        int wakeup_pipe[2]{-1, -1};
        if (::pipe(wakeup_pipe) != 0) {
            return fail_startup(process_error(
                "Process Terminal could not create its wakeup pipe",
                "pipe",
                errno));
        }
        // Both ends run non-blocking: the worker's drain loop must not block
        // when the pipe is empty after consuming the wake bytes, and a full
        // pipe must never block a wake_worker() write. UniqueFd owns the raw
        // descriptors from here on, so every failure path cleans them up.
        cch::support::UniqueFd read_end(wakeup_pipe[0]);
        cch::support::UniqueFd write_end(wakeup_pipe[1]);
        const int read_flags = ::fcntl(read_end.get(), F_GETFL);
        const int write_flags = ::fcntl(write_end.get(), F_GETFL);
        if (read_flags < 0 || write_flags < 0 ||
            ::fcntl(read_end.get(), F_SETFL, read_flags | O_NONBLOCK) != 0 ||
            ::fcntl(write_end.get(), F_SETFL, write_flags | O_NONBLOCK) != 0) {
            return fail_startup(process_error(
                "Process Terminal could not configure its wakeup pipe",
                "fcntl(F_SETFL)",
                errno));
        }
        impl_->wakeup_read = std::move(read_end);
        impl_->wakeup_write = std::move(write_end);
    }
    const int original_flags = ::fcntl(impl_->options.output_fd, F_GETFL);
    if (original_flags < 0) {
        return fail_startup(process_error(
            "Process Terminal could not read output descriptor flags",
            "fcntl(F_GETFL)",
            errno));
    }
    impl_->original_fd_flags = original_flags;
    // The output descriptor runs non-blocking so render-path writes never
    // block the caller; partial writes and backpressure fall into the bounded
    // ordered queue the delivery worker drains (issue #462). O_NONBLOCK is
    // shared when input and output are the same descriptor, so the input read
    // path tolerates EAGAIN.
    if (::fcntl(impl_->options.output_fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return fail_startup(process_error(
            "Process Terminal could not set non-blocking output",
            "fcntl(F_SETFL)",
            errno));
    }
    impl_->output_nonblock = true;
    impl_->output_queue.clear();
    impl_->output_queued_bytes = 0;
    impl_->output_draining = false;
    impl_->synchronized_output.clear();
    impl_->synchronized_output_state.reset();
    impl_->synchronized_update_depth = 0;
    impl_->last_resize_check = std::chrono::steady_clock::now();

    impl_->input_sink = std::move(owned_input_sink);
    impl_->resize_sink = std::move(owned_resize_sink);
    impl_->dimensions = *dimensions;
    impl_->modes.started = true;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        // Impl outlives this borrowed pointer: destruction cannot occur from a sink,
        // and destructor-driven stop joins the delivery worker.
        auto* worker_impl = impl_.get();
        impl_->worker = std::jthread([worker_impl](std::stop_token stop_token) {
            run_terminal_worker(*worker_impl, stop_token);
        });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::system_error& error) {
        impl_->modes.started = false;
        impl_->input_sink.reset();
        impl_->resize_sink.reset();
        return fail_startup(support::make_error(
            support::ErrorCode::Process,
            "Process Terminal could not start input and resize delivery",
            std::format("thread creation failed (code {})", error.code().value())));
    } catch (const std::exception& error) {
        // Any other throw after O_NONBLOCK was set (e.g. bad_alloc from thread
        // creation) must still restore termios and descriptor state (issue
        // #462 criterion 4: restoration after exceptions).
        impl_->modes.started = false;
        impl_->input_sink.reset();
        impl_->resize_sink.reset();
        return fail_startup(support::make_error(
            support::ErrorCode::Process,
            "Process Terminal could not start input and resize delivery",
            std::format("startup failed ({})", error.what())));
    } catch (...) {
        impl_->modes.started = false;
        impl_->input_sink.reset();
        impl_->resize_sink.reset();
        return fail_startup(support::make_error(
            support::ErrorCode::Process,
            "Process Terminal could not start input and resize delivery",
            "startup failed with an unknown exception"));
    }
#endif
    return {};
}

support::ExpectedVoid ProcessTerminal::stop() {
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
    const auto called_from_worker = impl_->worker.joinable() &&
        impl_->worker.get_id() == std::this_thread::get_id();
    if (impl_->stop_in_progress) {
        if (called_from_worker) return {};
        impl_->lifecycle_cv.wait(lifecycle_lock, [this] { return !impl_->stop_in_progress; });
        return {};
    }
    impl_->stop_in_progress = true;
    lifecycle_lock.unlock();

    {
        std::lock_guard lock(impl_->mutex);
        impl_->input_sink.reset();
        impl_->resize_sink.reset();
        impl_->modes.started = false;
        if (impl_->worker.joinable()) impl_->worker.request_stop();
        // Wake the worker's blocking readiness wait so cancellation is prompt
        // even while the worker is parked with no armed deadlines.
        wake_worker(*impl_);
    }
    if (impl_->worker.joinable() && !called_from_worker) impl_->worker.join();

    support::ExpectedVoid result;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->worker_error) {
            result = std::unexpected(std::move(*impl_->worker_error));
            impl_->worker_error.reset();
        }
        // The worker drained ordered output before exiting; anything still
        // queued (e.g. after an input error or EOF) cannot be delivered and is
        // dropped before restoration writes replace it.
        impl_->output_queue.clear();
        impl_->output_queued_bytes = 0;
        impl_->output_draining = false;
        impl_->synchronized_output.clear();
        impl_->synchronized_output_state.reset();
        retain_error(result, restore_terminal_modes(*impl_));
    }

    lifecycle_lock.lock();
    impl_->stop_in_progress = false;
    lifecycle_lock.unlock();
    impl_->lifecycle_cv.notify_all();
    return result;
}

TerminalDimensions ProcessTerminal::dimensions() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->dimensions;
}

TerminalCapabilities ProcessTerminal::capabilities() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->capabilities;
}

TerminalModeState ProcessTerminal::modes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->modes;
}

support::ExpectedVoid ProcessTerminal::clear_screen() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    // pi's resize full-redraw clears scrollback too (`\x1b[3J`): the native
    // scroll history, the mirrored scroll count, and the anchor all reset
    // (the screen is cleared and homed, so buffer row 0 lands on screen row 0).
    impl_->viewport_top = 0;
    impl_->scroll_origin = 0;
    return enqueue_output(*impl_, kClearScreen);
}

support::ExpectedVoid ProcessTerminal::write(std::string_view output) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    return enqueue_output(*impl_, output);
}

support::ExpectedVoid ProcessTerminal::set_cursor(CursorPosition position) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (position.column > impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal cursor position is outside its dimensions"));
    }
    // `row` is a buffer row under the main-screen scrollback flow: the
    // renderer writes the full composed buffer in increasing row order, and
    // rows at or past the visible viewport bottom must advance the terminal's
    // native scrollback so the addressed line becomes the bottom visible line.
    // Absolute CUP beyond the bottom clamps without scrolling on real
    // terminals, so mirror pi's append (`"\r\n".repeat(scroll)`) by moving to
    // the bottom and emitting CRLF line flow: the real terminal scrolls its
    // scroll history natively. Under the anchored absolute flow (ADR 0041) the
    // screen row of buffer row b is `scroll_origin + b - viewport_top`, and
    // the emulated scroll count stays in sync with the renderer's own tracked
    // viewport.
    const auto first_visible_row = impl_->viewport_top > impl_->scroll_origin
        ? impl_->viewport_top - impl_->scroll_origin
        : std::size_t{0};
    if (position.row < first_visible_row) {
        // Rows above the first visible buffer row are in the terminal's
        // scrollback; as before the anchor, they clamp to screen row 0 (or margin_top).
        impl_->cursor.row = impl_->margins_active ? impl_->margin_top : 0;
        impl_->cursor.column = position.column;
        return enqueue_output(
            *impl_,
            std::format("\x1b[{};{}H", impl_->cursor.row + 1, position.column + 1));
    }
    const auto screen_row = impl_->scroll_origin + position.row - impl_->viewport_top;
    if (impl_->margins_active) {
        const auto viewport_height = impl_->margin_bottom - impl_->margin_top + 1;
        if (screen_row >= viewport_height) {
            const auto scroll = screen_row - (viewport_height - 1);
            std::string sequence;
            sequence += std::format("\x1b[{};1H", impl_->margin_bottom + 1);
            sequence.append(scroll, '\n');
            impl_->viewport_top += scroll;
            impl_->cursor.row = impl_->margin_bottom;
            impl_->cursor.column = position.column;
            sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, position.column + 1);
            return enqueue_output(*impl_, sequence);
        }
        impl_->cursor.row = impl_->margin_top + screen_row;
        impl_->cursor.column = position.column;
        return enqueue_output(
            *impl_,
            std::format("\x1b[{};{}H", impl_->cursor.row + 1, position.column + 1));
    }
    if (screen_row >= impl_->dimensions.rows) {
        const auto scroll = screen_row - (impl_->dimensions.rows - 1);
        const auto bottom = impl_->dimensions.rows - 1;
        std::string sequence;
        if (impl_->cursor.row < bottom) {
            sequence += std::format("\x1b[{}B", bottom - impl_->cursor.row);
        }
        sequence.append(scroll, '\r');
        sequence.append(scroll, '\n');
        impl_->viewport_top += scroll;
        impl_->cursor.row = bottom;
        impl_->cursor.column = position.column;
        return enqueue_output(*impl_, sequence);
    }
    impl_->cursor.row = screen_row;
    impl_->cursor.column = position.column;
    return enqueue_output(
        *impl_,
        std::format("\x1b[{};{}H", impl_->cursor.row + 1, position.column + 1));
}

support::ExpectedVoid ProcessTerminal::set_scroll_margins(std::size_t top_row, std::size_t bottom_row) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (bottom_row >= impl_->dimensions.rows || top_row >= bottom_row) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal scroll margins are invalid"));
    }
    impl_->margin_top = top_row;
    impl_->margin_bottom = bottom_row;
    impl_->margins_active = true;
    auto sequence = std::format("\x1b[{};{}r", top_row + 1, bottom_row + 1);
    sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, impl_->cursor.column + 1);
    return enqueue_output(*impl_, sequence);
}

support::ExpectedVoid ProcessTerminal::reset_scroll_margins() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (!impl_->margins_active) return {};
    impl_->margins_active = false;
    impl_->margin_top = 0;
    impl_->margin_bottom = 0;
    auto sequence = std::format("\x1b[1;{}r", impl_->dimensions.rows);
    sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, impl_->cursor.column + 1);
    return enqueue_output(*impl_, sequence);
}

support::ExpectedVoid ProcessTerminal::set_dock_cursor(std::size_t dock_row, std::size_t column) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    const auto dock_start = impl_->margins_active ? (impl_->margin_bottom + 1) : 0;
    const auto screen_row = dock_start + dock_row;
    if (screen_row >= impl_->dimensions.rows || column > impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal dock cursor position is outside its dimensions"));
    }
    impl_->cursor.row = screen_row;
    impl_->cursor.column = column;
    return enqueue_output(
        *impl_,
        std::format("\x1b[{};{}H", screen_row + 1, column + 1));
}
support::ExpectedVoid ProcessTerminal::set_cursor_visible(bool visible) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.cursor_visible == visible) return {};
    auto result = enqueue_output(*impl_, visible ? kCursorShow : kCursorHide);
    if (result) impl_->modes.cursor_visible = visible;
    return result;
}

support::Expected<TerminalImageHandle> ProcessTerminal::place_image(const TerminalImage& image) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    const auto handle = image.preferred_handle
        ? *image.preferred_handle
        : TerminalImageHandle{.value = impl_->next_image_handle};
    auto encoded = detail::encode_terminal_image(
        impl_->capabilities.inline_images,
        image,
        handle);
    if (!encoded) return std::unexpected(encoded.error());
    if (image.region.column >= impl_->dimensions.columns ||
        image.region.columns > impl_->dimensions.columns - image.region.column) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image region is outside terminal dimensions"));
    }
    // Image regions are buffer-absolute under the main-screen scrollback flow.
    // A region whose buffer row has scrolled into the terminal's native
    // scrollback (above the first visible buffer row under the anchored
    // mapping, ADR 0041) cannot be addressed by the real terminal and is
    // already held by its scroll history; skip the placement but keep
    // the handle allocation so the renderer's active-image tracking stays
    // consistent (fork-B image-follows-content, ADR 0037).
    const auto first_visible_row = impl_->viewport_top > impl_->scroll_origin
        ? impl_->viewport_top - impl_->scroll_origin
        : std::size_t{0};
    if (image.region.row < first_visible_row) {
        if (!image.preferred_handle) ++impl_->next_image_handle;
        return handle;
    }
    const auto screen_row = impl_->scroll_origin + image.region.row - impl_->viewport_top;
    if (screen_row >= impl_->dimensions.rows ||
        image.region.rows > impl_->dimensions.rows - screen_row) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image region is outside terminal dimensions"));
    }
    // Both protocols anchor the image's top-left at the cursor, so position
    // the cursor at the region origin before emitting the sequence.
    if (auto positioned = enqueue_output(
            *impl_,
            std::format("\x1b[{};{}H", screen_row + 1, image.region.column + 1));
        !positioned) {
        return std::unexpected(positioned.error());
    }
    if (auto written = enqueue_output(*impl_, *encoded); !written) {
        return std::unexpected(written.error());
    }
    if (!image.preferred_handle) ++impl_->next_image_handle;
    return handle;
}

support::ExpectedVoid ProcessTerminal::remove_image(
    TerminalImageHandle handle,
    const CellRegion& region) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (region.column >= impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image removal region is outside terminal dimensions"));
    }
    // Buffer-absolute removal region (fork-B): regions that scrolled into the
    // native scrollback (above the first visible buffer row under the anchored
    // mapping, ADR 0041) are already out of the visible screen, so there is
    // nothing to blank in place; skip them.
    const auto first_visible_row = impl_->viewport_top > impl_->scroll_origin
        ? impl_->viewport_top - impl_->scroll_origin
        : std::size_t{0};
    if (region.row >= first_visible_row) {
        const auto screen_row = impl_->scroll_origin + region.row - impl_->viewport_top;
        const auto columns = std::min(region.columns, impl_->dimensions.columns - region.column);
        if (screen_row >= impl_->dimensions.rows) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "Inline image removal region is outside terminal dimensions"));
        }
        const auto rows = std::min(region.rows, impl_->dimensions.rows - screen_row);
        auto removal = detail::encode_terminal_image_removal(
            impl_->capabilities.inline_images,
            handle);
        if (!removal) return std::unexpected(removal.error());
        if (auto written = enqueue_output(*impl_, *removal); !written) {
            return std::unexpected(written.error());
        }
        // iTerm2 has no addressable deletion: blank the owned cells. Kitty's
        // delete restores the blanked cells the TUI placed the image over, and
        // the blanking is idempotent with it (matching the VirtualTerminal
        // recorded removal semantics).
        if (auto cleared = enqueue_output(
                *impl_,
                std::format(
                    "\x1b[{};{}H{}",
                    screen_row + 1,
                    region.column + 1,
                    std::string(columns * rows, ' ')));
            !cleared) {
            return std::unexpected(cleared.error());
        }
    }
    return {};
}

support::ExpectedVoid ProcessTerminal::begin_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth > 0) {
        ++impl_->synchronized_update_depth;
        return {};
    }
    impl_->synchronized_output_state = ProcessTerminal::Impl::SynchronizedOutputState{
        .cursor = impl_->cursor,
        .scroll_origin = impl_->scroll_origin,
        .viewport_top = impl_->viewport_top,
        .next_image_handle = impl_->next_image_handle,
        .margin_top = impl_->margin_top,
        .margin_bottom = impl_->margin_bottom,
        .margins_active = impl_->margins_active,
    };
    impl_->synchronized_output.clear();
    impl_->synchronized_output.append(kBeginSynchronizedUpdate);
    impl_->synchronized_update_depth = 1;
    return {};
}

support::ExpectedVoid ProcessTerminal::end_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Process Terminal synchronized update is not active"));
    }
    if (impl_->synchronized_update_depth > 1) {
        --impl_->synchronized_update_depth;
        return {};
    }

    impl_->synchronized_output.append(kEndSynchronizedUpdate);
    impl_->synchronized_update_depth = 0;
    auto frame = std::move(impl_->synchronized_output);
    impl_->synchronized_output.clear();
    auto ended = enqueue_output(*impl_, frame);
    if (!ended && impl_->synchronized_output_state) {
        impl_->cursor = impl_->synchronized_output_state->cursor;
        impl_->scroll_origin = impl_->synchronized_output_state->scroll_origin;
        impl_->viewport_top = impl_->synchronized_output_state->viewport_top;
        impl_->next_image_handle = impl_->synchronized_output_state->next_image_handle;
        impl_->margin_top = impl_->synchronized_output_state->margin_top;
        impl_->margin_bottom = impl_->synchronized_output_state->margin_bottom;
        impl_->margins_active = impl_->synchronized_output_state->margins_active;
    }
    impl_->synchronized_output_state.reset();
    return ended;
}

support::ExpectedVoid ProcessTerminal::set_title(std::string_view title) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    return enqueue_output(*impl_, std::format("\x1b]0;{}\x07", title));
}

support::ExpectedVoid ProcessTerminal::set_progress(bool active) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    auto result = enqueue_output(
        *impl_,
        active ? kProgressActiveSequence : kProgressClearSequence);
    if (!result) return result;
    if (active) {
        if (!impl_->progress_active) {
            impl_->progress_active = true;
            impl_->progress_next_keepalive = std::chrono::steady_clock::now() + kProgressKeepalive;
        }
    } else {
        impl_->progress_active = false;
    }
    return {};
}

support::ExpectedVoid ProcessTerminal::drain_input(
    std::chrono::milliseconds max_ms,
    std::chrono::milliseconds idle_ms) {
    std::unique_lock lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    // Disable the keyboard protocols first so late key releases cannot
    // generate new escape sequences while the buffer drains (pi drainInput).
    // The disables go through the ordered bounded queue so they cannot jump
    // ahead of render output still being flushed.
    if (impl_->keyboard_protocol_pushed) {
        if (auto popped = enqueue_output(*impl_, kKeyboardProtocolPop); !popped) {
            return popped;
        }
        impl_->keyboard_protocol_pushed = false;
        impl_->capabilities.keyboard_protocol = KeyboardProtocol::Legacy;
    }
    if (impl_->modify_other_keys_active) {
        if (auto disabled = enqueue_output(*impl_, kModifyOtherKeysDisable); !disabled) {
            return disabled;
        }
        impl_->modify_other_keys_active = false;
    }

    // The delivery worker stays the sole reader: while draining it discards
    // everything it reads and records each read in drain_last_activity. This
    // call waits until input has been idle for idle_ms (or max_ms elapses);
    // it never reads or holds a lock across a sleep, so it cannot race the
    // worker's poll-to-read window or contend with its periodic locks.
    impl_->draining = true;
    impl_->drain_last_activity = std::chrono::steady_clock::now();
    auto last_activity = impl_->drain_last_activity;
    lock.unlock();
    // The protocol disables must reach the terminal before the drain completes
    // (late key releases must not leak); wait, bounded by the drain window,
    // for the delivery worker to flush the ordered queue.
    const auto output_deadline = last_activity + max_ms;
    while (true) {
        {
            std::lock_guard guard(impl_->mutex);
            if (impl_->output_queue.empty() && !impl_->output_draining) break;
        }
        if (std::chrono::steady_clock::now() >= output_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto deadline = last_activity + max_ms;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto idle_elapsed = now - last_activity;
        if (idle_elapsed >= idle_ms) break;
        const auto remaining = std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(idle_ms - idle_elapsed),
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        std::this_thread::sleep_for(remaining);
        std::lock_guard guard(impl_->mutex);
        last_activity = impl_->drain_last_activity;
    }

    lock.lock();
    impl_->draining = false;
    return {};
}

} // namespace cch::tui
