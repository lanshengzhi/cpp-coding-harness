#include <cch/tui/ProcessTerminal.hpp>

#include <cch/tui/StdinBuffer.hpp>
#include <cch/tui/TerminalImage.hpp>

#include "tui/InputDecoder.hpp"
#include "support/UniqueFd.hpp"

#include <cch/support/Error.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <atomic>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
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
constexpr std::string_view kEnterAlternateScreen = "\x1b[?1049h";
constexpr std::string_view kExitAlternateScreen = "\x1b[?1049l";
constexpr std::string_view kDisableAutowrap = "\x1b[?7l";
constexpr std::string_view kEnableAutowrap = "\x1b[?7h";
// pi's resize full-redraw clears screen, homes, and clears scrollback
// (packages/tui/src/terminal.ts clearScreen: `\x1b[2J\x1b[H\x1b[3J`).
constexpr std::string_view kClearScreen = "\x1b[2J\x1b[H\x1b[3J";
constexpr std::string_view kBeginSynchronizedUpdate = "\x1b[?2026h";
constexpr std::string_view kEndSynchronizedUpdate = "\x1b[?2026l";
constexpr std::string_view kKeyboardProtocolPush = "\x1b[>7u";
constexpr std::string_view kKeyboardProtocolQuery = "\x1b[?u\x1b[c";
constexpr std::string_view kKeyboardProtocolPop = "\x1b[<u";
// pi `TuiMainScreen` relative line-flow and scrollback flow:
// No startup DSR query (`\x1b[6n`) is sent during regular TUI startup or rendering.
constexpr std::string_view kModifyOtherKeysEnable = "\x1b[>4;2m";
constexpr std::string_view kModifyOtherKeysDisable = "\x1b[>4;0m";
constexpr std::string_view kProgressActiveSequence = "\x1b]9;4;3\x07";
constexpr std::string_view kProgressClearSequence = "\x1b]9;4;0\x07";
constexpr auto kProgressKeepalive = std::chrono::milliseconds(1000);
/// The readiness timer re-reads TIOCGWINSZ at least this often so resizes are
/// detected even without SIGWINCH delivery.
/// This is a low-rate watchdog timer, not input polling.
constexpr auto kResizeWatchdogInterval = std::chrono::milliseconds(500);
/// Ordered output queue bound: beyond this many queued bytes the terminal
/// reports explicit backpressure (`Busy`) instead of buffering without limit.
/// The bound must exceed one full-frame repaint, or a frame cannot be admitted
/// while the terminal is not draining: the deferred retry re-emits the same
/// frame from its start, so it would stall at the same byte and never converge.
/// Measured startup paint (#727, session-independent): 252-279 KB at 120x40 and
/// 410 KB at 240x60, so 1 MB keeps roughly 2.5x headroom over the largest.
/// debt: a repaint larger than this bound cannot converge through the deferred
/// retry; raise the bound or paint in slices when a measured paint exceeds it.
constexpr std::size_t kOutputQueueMaxBytes = 1024 * 1024;

[[nodiscard]] support::Error process_error(std::string message, std::string_view operation, int error_number) {
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
        return support::make_error(support::ErrorCode::Busy,
                std::move(message),
                std::format("{} temporarily unavailable (errno {})", operation, error_number));
    }
    return support::make_error(support::ErrorCode::Process,
            std::move(message),
            std::format("{} failed (errno {})", operation, error_number));
}

struct WriteAttempt {
    support::ExpectedVoid result;
    std::size_t bytes_written{0};
};

[[nodiscard]] WriteAttempt attempt_write_all(int descriptor, std::string_view output) {
    constexpr std::size_t kMaxWritePollRetries = 5;
    WriteAttempt attempt;
    std::size_t poll_retries = 0;
    while (attempt.bytes_written < output.size()) {
        const auto written =
                ::write(descriptor, output.data() + attempt.bytes_written, output.size() - attempt.bytes_written);
        if (written > 0) {
            attempt.bytes_written += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A persistently unwritable descriptor must report Busy instead of
            // stalling the render call forever, so the retry budget is bounded.
            if (poll_retries < kMaxWritePollRetries) {
                ++poll_retries;
                pollfd pfd{.fd = descriptor, .events = POLLOUT, .revents = 0};
                const int poll_result = ::poll(&pfd, 1, 100);
                if (poll_result > 0 && (pfd.revents & POLLOUT) != 0) continue;
                if (poll_result < 0 && errno == EINTR) continue;
            }
            attempt.result =
                    std::unexpected(process_error("Process Terminal could not write terminal output", "write", EAGAIN));
            return attempt;
        }
        const auto error_number = written < 0 ? errno : EIO;
        attempt.result = std::unexpected(
                process_error("Process Terminal could not write terminal output", "write", error_number));
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

/// Resolve the `PI_TUI_WRITE_LOG` diagnostic target once, at construction (pi
/// `ProcessTerminal.writeLogPath`): empty disables the log, an existing
/// directory receives a timestamped per-process file, and anything else is the
/// literal file path. A path that cannot be inspected is used as-is, the way
/// pi's failed `statSync` probe falls through to the configured value.
[[nodiscard]] std::string resolve_write_log_path() {
    const auto configured = std::string(environment("PI_TUI_WRITE_LOG"));
    if (configured.empty()) return {};
    std::error_code error;
    if (!std::filesystem::is_directory(configured, error) || error) return configured;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    (void)localtime_r(&now, &local);
    const auto timestamp = std::format("{:04d}-{:02d}-{:02d}_{:02d}-{:02d}-{:02d}",
            local.tm_year + 1900,
            local.tm_mon + 1,
            local.tm_mday,
            local.tm_hour,
            local.tm_min,
            local.tm_sec);
    return (std::filesystem::path(configured) / std::format("tui-{}-{}.log", timestamp, ::getpid())).string();
}

[[nodiscard]] bool supports_synchronized_output() {
    // DEC 2026 is a private mode; terminals without support safely ignore it.
    // Follow pi's conservative default so unknown TERM values still avoid frame flicker.
    return true;
}

[[nodiscard]] TerminalColorCapability detect_color_capability() {
    const auto color_terminal = lowercase_environment("COLORTERM");
    const auto terminal = lowercase_environment("TERM");
    const bool has_true_color_hint =
            color_terminal == "truecolor" || color_terminal == "24bit" || terminal.ends_with("-direct");
    if (!environment("TMUX").empty() || terminal.starts_with("tmux") || terminal.starts_with("screen")) {
        return has_true_color_hint ? TerminalColorCapability::TrueColor : TerminalColorCapability::Xterm256;
    }

    const auto program = lowercase_environment("TERM_PROGRAM");
    const auto emulator = lowercase_environment("TERMINAL_EMULATOR");
    const bool known_true_color =
            !environment("KITTY_WINDOW_ID").empty() || program == "kitty" || program == "ghostty" ||
            terminal.find("ghostty") != std::string::npos || !environment("GHOSTTY_RESOURCES_DIR").empty() ||
            !environment("WEZTERM_PANE").empty() || program == "wezterm" || program == "warpterminal" ||
            !environment("WARP_SESSION_ID").empty() || !environment("WARP_TERMINAL_SESSION_UUID").empty() ||
            !environment("ITERM_SESSION_ID").empty() || program == "iterm.app" || !environment("WT_SESSION").empty() ||
            program == "vscode" || program == "alacritty" || emulator == "jetbrains-jediterm";
    return known_true_color || has_true_color_hint ? TerminalColorCapability::TrueColor
                                                   : TerminalColorCapability::Xterm256;
}

struct EnvironmentRgb {
    int red{0};
    int green{0};
    int blue{0};
};

[[nodiscard]] EnvironmentRgb xterm_index_to_rgb(int index) {
    constexpr std::array<EnvironmentRgb, 16> basic{{
            {0, 0, 0},
            {128, 0, 0},
            {0, 128, 0},
            {128, 128, 0},
            {0, 0, 128},
            {128, 0, 128},
            {0, 128, 128},
            {192, 192, 192},
            {128, 128, 128},
            {255, 0, 0},
            {0, 255, 0},
            {255, 255, 0},
            {0, 0, 255},
            {255, 0, 255},
            {0, 255, 255},
            {255, 255, 255},
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
    const auto luminance =
            0.2126 * linear_channel(rgb.red) + 0.7152 * linear_channel(rgb.green) + 0.0722 * linear_channel(rgb.blue);
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

} // namespace
struct InputState {
    StdinBuffer stdin_buffer{StdinBufferOptions{}};
    detail::TerminalStreamDecoder decoder;
    bool needs_input_flush{false};
    /// Escape window, resolved once per session from pi's `resolveEscapeTimeoutMs`.
    std::chrono::milliseconds escape_fragment_timeout{detail::kEscapeFragmentTimeout};
    /// Deadline for the fragment currently held by the decoder (and for the
    /// empty-value flush that follows forwarded input); `max` arms nothing.
    std::chrono::steady_clock::time_point negotiation_deadline{std::chrono::steady_clock::time_point::max()};
};

struct ProcessTerminal::Impl {
    explicit Impl(ProcessTerminalOptions configured_options) : options(configured_options) {}

    ProcessTerminalOptions options;
    /// Resolved once at construction (pi ProcessTerminal.writeLogPath).
    std::string write_log_path;
    std::weak_ptr<Impl> self;
    mutable std::mutex mutex;
    TerminalDimensions dimensions;
    TerminalCapabilities capabilities;
    TerminalModeState modes;
    std::shared_ptr<TerminalInputSink> input_sink;
    std::shared_ptr<TerminalResizeSink> resize_sink;
    std::string startup_input;
    std::optional<boost::asio::posix::stream_descriptor> input_stream;
    std::optional<boost::asio::posix::stream_descriptor> output_stream;
    std::optional<boost::asio::steady_timer> keepalive_timer;
    std::optional<boost::asio::steady_timer> readiness_timer;
    std::array<char, 4096> read_buffer{};
    InputState input_state;
    std::shared_ptr<std::atomic_bool> session_alive;
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
    /// Screen row where buffer row 0 was first written. Regular rendering
    /// flows relative to the inherited shell cursor (#983), so the renderer
    /// never learns the absolute origin and keeps it at 0; a retained
    /// buffer-to-screen base for set_cursor.
    std::size_t scroll_origin{0};
    /// Total native scrolls since the anchor, mirroring VirtualTerminal's
    /// scroll emulation: the renderer writes the full composed buffer with
    /// buffer-relative rows under the main-screen scrollback flow, and rows at
    /// or past the visible viewport bottom advance this count while
    /// `set_cursor` emits pi's CRLF line flow so the real terminal's native
    /// scrollback receives the overflow. The screen row of buffer row b is
    /// `scroll_origin + b - viewport_top`; the first visible buffer row is
    /// `max(0, viewport_top - scroll_origin)`. `set_cursor`
    /// converts buffer rows to screen rows using this mapping before emitting
    /// ANSI sequences.
    std::size_t viewport_top{0};
    termios original_termios{};
    bool has_original_termios{false};
    int original_input_flags{-1};
    bool input_nonblock{false};
    /// Original descriptor flags (O_NONBLOCK is added to the output
    /// descriptor at start and restored on every exit path).
    int original_fd_flags{-1};
    bool output_nonblock{false};
    /// Ordered bounded output queue drained by executor readiness when the
    /// output descriptor is writable. The queue gives explicit backpressure
    /// (`Busy`) instead of blocking the caller indefinitely on a backed-up
    /// terminal.
    std::deque<std::string> output_queue;
    std::size_t output_queued_bytes{0};
    bool output_draining{false};
    bool output_wait_armed{false};
    /// Resize watchdog deadline base, owned by the readiness timer: the event
    /// loop wakes at least this often to re-read TIOCGWINSZ, so resizes are
    /// detected even without SIGWINCH delivery.
    std::chrono::steady_clock::time_point last_resize_check{};
    bool output_only{false};
    int original_output_only_flags{-1};
    bool was_started{false};
};

namespace {

template <typename T> [[nodiscard]] support::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "Process Terminal must be started before terminal operations"));
}

template <typename T> [[nodiscard]] support::ExpectedVoid require_output_admission(const T& impl) {
    if (impl.modes.started || impl.output_only) return {};
    return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "Process Terminal must be started before terminal operations"));
}

/// Append admitted output to the `PI_TUI_WRITE_LOG` diagnostic file
/// (pi `ProcessTerminal.write`, packages/tui/src/terminal.ts:479-481). Only
/// bytes the terminal admitted are logged, and a logging failure is swallowed:
/// the log is a diagnostic, never a write path.
template <typename T> void append_write_log(const T& impl, std::string_view output) {
    if (impl.write_log_path.empty() || output.empty()) return;
    std::ofstream log(impl.write_log_path, std::ios::binary | std::ios::app);
    if (!log) return;
    log.write(output.data(), static_cast<std::streamsize>(output.size()));
}

[[nodiscard]] support::Expected<TerminalDimensions> read_dimensions(int descriptor) {
    winsize size{};
    if (::ioctl(descriptor, TIOCGWINSZ, &size) != 0) {
        return std::unexpected(
                process_error("Process Terminal could not read terminal dimensions", "ioctl(TIOCGWINSZ)", errno));
    }
    if (size.ws_col == 0 || size.ws_row == 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Process Terminal requires positive terminal dimensions"));
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
        support::Error primary, const support::Error& secondary, std::string message) {
    return support::make_error(primary.code,
            std::move(message),
            std::format("primary: {}; secondary: {}", describe_error(primary), describe_error(secondary)));
}

void retain_error(support::ExpectedVoid& accumulated, support::ExpectedVoid candidate) {
    if (candidate) return;
    if (accumulated) {
        accumulated = std::unexpected(candidate.error());
        return;
    }
    accumulated = std::unexpected(combine_errors(std::move(accumulated.error()),
            candidate.error(),
            "Process Terminal restoration encountered multiple failures"));
}

[[nodiscard]] support::Error startup_failure(support::Error acquisition_error, const support::ExpectedVoid& rollback) {
    if (rollback) return acquisition_error;
    return combine_errors(std::move(acquisition_error),
            rollback.error(),
            "Process Terminal startup failed and rollback was incomplete");
}

template <typename T> void record_worker_error(T& impl, support::Error error) {
    std::lock_guard lock(impl.mutex);
    if (!impl.worker_error) impl.worker_error = std::move(error);
}

template <typename T>
void apply_terminal_responses(T& impl, const std::vector<detail::TerminalResponseVariant>& responses);

template <typename T> void invoke_input(T& impl, std::string input) {
    std::shared_ptr<TerminalInputSink> sink;
    {
        std::lock_guard lock(impl.mutex);
        if (impl.draining) return;
        sink = impl.input_sink;
    }
    if (!sink || !*sink) return;
    if (auto delivered = (*sink)(std::move(input)); !delivered) {
        record_worker_error(impl, std::move(delivered.error()));
        std::lock_guard lock(impl.mutex);
        if (impl.input_sink == sink) impl.input_sink.reset();
    }
}

template <typename T> void process_framed_input(T& impl, std::string sequence) {
    if (sequence.empty()) return;
    auto decoded = impl.input_state.decoder.feed(sequence);
    apply_terminal_responses(impl, decoded.responses);
    if (!decoded.forwarded_input.empty()) {
        invoke_input(impl, std::move(decoded.forwarded_input));
        impl.input_state.needs_input_flush = true;
    }
}

template <typename T> void configure_stdin_buffer(T& impl) {
    impl.input_state.stdin_buffer.clear();
    impl.input_state.stdin_buffer = StdinBuffer({
            .timeout = detail::kSequenceFragmentTimeout,
            .escape_timeout = impl.input_state.escape_fragment_timeout,
    });
    const auto weak_self = impl.self;
    impl.input_state.stdin_buffer.set_data_handler([weak_self](std::string sequence) {
        const auto self = weak_self.lock();
        if (!self) return;
        process_framed_input(*self, std::move(sequence));
    });
    impl.input_state.stdin_buffer.set_paste_handler([weak_self](std::string content) {
        const auto self = weak_self.lock();
        if (!self) return;
        invoke_input(*self, std::string("\x1b[200~") + std::move(content) + "\x1b[201~");
    });
}

/// True while the terminal session is still live, i.e. teardown has not begun.
/// A resize may be dispatched only while the session is alive: this mirrors the
/// input/output drain paths' weak lifetime guard so an in-flight resize is
/// neutralized once stop() clears session_alive and started, before the owning
/// component tree is destroyed (issue #628).
template <typename T> [[nodiscard]] bool session_active(T& impl) {
    return impl.session_alive && impl.session_alive->load() && impl.modes.started;
}

template <typename T> void deliver_resize_if_changed(T& impl) {
    // The watchdog base advances on every check so the poll timeout stays in
    // the future and the worker never spins on an immediate resize deadline.
    {
        std::lock_guard lock(impl.mutex);
        impl.last_resize_check = std::chrono::steady_clock::now();
    }
    winsize size{};
    if (::ioctl(impl.options.output_fd, TIOCGWINSZ, &size) != 0 || size.ws_col == 0 || size.ws_row == 0) {
        return;
    }
    const TerminalDimensions dimensions{
            .columns = size.ws_col,
            .rows = size.ws_row,
    };
    std::shared_ptr<TerminalResizeSink> sink;
    {
        std::lock_guard lock(impl.mutex);
        if (!session_active(impl)) return;
        if (impl.dimensions == dimensions) return;
        impl.dimensions = dimensions;
        sink = impl.resize_sink;
    }
    if (!sink || !*sink) return;
    if (auto delivered = (*sink)(dimensions); !delivered) {
        record_worker_error(impl, std::move(delivered.error()));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    }
}

template <typename T> void enable_modify_other_keys(T& impl) {
    if (impl.capabilities.keyboard_protocol == KeyboardProtocol::Kitty || impl.modify_other_keys_active) {
        return;
    }
    if (auto enabled = write_all(impl.options.output_fd, kModifyOtherKeysEnable); !enabled) {
        if (!impl.worker_error) impl.worker_error = enabled.error();
        return;
    }
    impl.modify_other_keys_active = true;
    impl.capabilities.keyboard_protocol = KeyboardProtocol::ModifyOtherKeys;
}

template <typename T> void apply_keyboard_response(T& impl, const detail::KeyboardProtocolResponse& response) {
    std::lock_guard lock(impl.mutex);
    if (response.kind == detail::KeyboardProtocolResponseKind::DeviceAttributes || response.flags == 0) {
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
template <typename T> void drain_output_locked(T& impl, bool writable);
template <typename T> [[nodiscard]] support::ExpectedVoid enqueue_output(T& impl, std::string_view bytes);

template <typename T> void emit_progress_keepalive(T& impl) {
    std::lock_guard lock(impl.mutex);
    if (!impl.progress_active) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < impl.progress_next_keepalive) return;
    if (!impl.output_queue.empty() || impl.output_draining || impl.synchronized_update_depth > 0) {
        impl.progress_next_keepalive = now + kProgressKeepalive;
        return;
    }
    if (auto queued = enqueue_output(impl, kProgressActiveSequence);
            !queued && queued.error().code != support::ErrorCode::Busy && !impl.worker_error) {
        impl.worker_error = queued.error();
    }
    impl.progress_next_keepalive = now + kProgressKeepalive;
}

/// Arm one executor-owned writable wait for the FIFO. The caller holds the
/// implementation mutex; the completion reacquires it before draining, so
/// enqueue and drain remain strictly ordered.
template <typename T> void arm_output_wait_locked(T& impl) {
    if (!impl.modes.started || impl.output_queue.empty() || impl.output_wait_armed) return;
    boost::asio::posix::stream_descriptor* stream = nullptr;
    if (impl.options.output_fd == impl.options.input_fd) {
        if (impl.input_stream) stream = &*impl.input_stream;
    } else if (impl.output_stream) {
        stream = &*impl.output_stream;
    }
    if (stream == nullptr) return;
    const auto weak_self = impl.self;
    impl.output_wait_armed = true;
    const auto weak_alive = std::weak_ptr<std::atomic_bool>{impl.session_alive};
    stream->async_wait(boost::asio::posix::stream_descriptor::wait_write,
            [weak_self, weak_alive](const boost::system::error_code& error) {
                const auto self = weak_self.lock();
                if (!self) return;
                auto& impl = *self;
                const auto alive = weak_alive.lock();
                if (!alive || !alive->load()) return;
                std::lock_guard lock(impl.mutex);
                impl.output_wait_armed = false;
                if (error) {
                    if (error != boost::asio::error::operation_aborted && !impl.worker_error) {
                        impl.worker_error = support::make_error(support::ErrorCode::Process,
                                "Process Terminal output readiness failed",
                                error.message());
                    }
                    return;
                }
                if (!impl.modes.started) return;
                drain_output_locked(impl, true);
                arm_output_wait_locked(impl);
            });
}

template <typename T> void arm_keepalive_timer_locked(T& impl) {
    if (!impl.keepalive_timer || !impl.modes.started || !impl.progress_active) return;
    const auto weak_self = impl.self;
    impl.keepalive_timer->expires_at(impl.progress_next_keepalive);
    const auto weak_alive = std::weak_ptr<std::atomic_bool>{impl.session_alive};
    impl.keepalive_timer->async_wait([weak_self, weak_alive](const boost::system::error_code& error) {
        const auto self = weak_self.lock();
        if (!self) return;
        auto& impl = *self;
        const auto alive = weak_alive.lock();
        if (!alive || !alive->load()) return;
        if (error == boost::asio::error::operation_aborted) return;
        if (error) {
            std::lock_guard lock(impl.mutex);
            if (!impl.worker_error) {
                impl.worker_error = support::make_error(
                        support::ErrorCode::Process, "Process Terminal keepalive timer failed", error.message());
            }
            return;
        }
        emit_progress_keepalive(impl);
        std::lock_guard lock(impl.mutex);
        arm_keepalive_timer_locked(impl);
    });
}

/// Wake the executor-owned writable readiness wait after output enqueue.
template <typename T> void wake_output_wait(T& impl) { arm_output_wait_locked(impl); }

/// Whether ordered output is still pending (queued or being drained).
template <typename T> bool output_pending(const T& impl) {
    std::lock_guard lock(impl.mutex);
    return !impl.output_queue.empty() || impl.output_draining;
}

/// Submit one ordered output chunk through the bounded queue. Caller holds
/// `impl.mutex`. The common fast path writes the whole chunk inline when the
/// queue is empty and the descriptor accepts it; partial writes and
/// backpressure fall into the bounded FIFO drained by executor readiness.
template <typename T> [[nodiscard]] support::ExpectedVoid enqueue_output(T& impl, std::string_view bytes) {
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
            return std::unexpected(process_error("Process Terminal could not write terminal output", "write", errno));
        }
    }
    if (!impl.output_queue.empty() && impl.output_queued_bytes + bytes.size() > kOutputQueueMaxBytes) {
        return std::unexpected(support::make_error(support::ErrorCode::Busy,
                "Process Terminal output is backed up",
                std::format("the bounded output queue ({} bytes) cannot admit more output", kOutputQueueMaxBytes)));
    }
    impl.output_queue.push_back(std::string(bytes));
    impl.output_queued_bytes += bytes.size();
    wake_output_wait(impl);
    return {};
}

template <typename T> [[nodiscard]] support::ExpectedVoid flush_synchronized_update_locked(T& impl) {
    if (impl.synchronized_update_depth == 0) return {};
    impl.synchronized_output.append(kEndSynchronizedUpdate);
    impl.synchronized_update_depth = 0;
    auto frame = std::move(impl.synchronized_output);
    impl.synchronized_output.clear();
    auto ended = enqueue_output(impl, frame);
    if (!ended && impl.synchronized_output_state) {
        impl.cursor = impl.synchronized_output_state->cursor;
        impl.scroll_origin = impl.synchronized_output_state->scroll_origin;
        impl.viewport_top = impl.synchronized_output_state->viewport_top;
        impl.next_image_handle = impl.synchronized_output_state->next_image_handle;
        impl.margin_top = impl.synchronized_output_state->margin_top;
        impl.margin_bottom = impl.synchronized_output_state->margin_bottom;
        impl.margins_active = impl.synchronized_output_state->margins_active;
    }
    impl.synchronized_output_state.reset();
    return ended;
}
/// Drain the ordered output queue into the output descriptor while it is
/// writable. The mutex serializes queue admission and partial writes.
template <typename T> void drain_output_locked(T& impl, bool writable) {
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
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (written < 0) {
            if (!impl.worker_error) {
                impl.worker_error = process_error("Process Terminal could not write terminal output", "write", errno);
            }
            impl.output_queued_bytes = 0;
            impl.output_queue.clear();
        }
        break;
    }
    impl.output_draining = false;
}

template <typename T> void drain_output(T& impl, bool writable) {
    std::lock_guard lock(impl.mutex);
    drain_output_locked(impl, writable);
}

/// The downstream cell-size consumer, reached only when the host runs a
/// cell-size reply through its raw-input listener stage (#952; pi
/// terminal-image.ts consumeCellSizeResponse at the frozen listener stage, pi
/// tui.ts onLateReply). Refining the reported pixels is a
/// presentation-affecting capability update, so the resize sink runs with
/// unchanged dimensions.
template <typename T> void apply_cell_size_response(T& impl, const CellPixelDimensions& pixels) {
    if (pixels.width == 0 || pixels.height == 0) return;
    std::shared_ptr<TerminalResizeSink> sink;
    TerminalDimensions dimensions;
    {
        std::lock_guard lock(impl.mutex);
        // Same lifetime guard as deliver_resize_if_changed (issue #628): never
        // dispatch a resize once teardown has begun.
        if (!session_active(impl)) return;
        if (impl.capabilities.cell_pixels && *impl.capabilities.cell_pixels == pixels) {
            return;
        }
        impl.capabilities.cell_pixels = pixels;
        sink = impl.resize_sink;
        dimensions = impl.dimensions;
    }
    if (!sink || !*sink) return;
    if (auto delivered = (*sink)(dimensions); !delivered) {
        record_worker_error(impl, std::move(delivered.error()));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    }
}

template <typename T>
void apply_terminal_responses(T& impl, const std::vector<detail::TerminalResponseVariant>& responses) {
    for (const auto& response : responses) {
        // A cell-size reply is deliberately not applied here (#952): the reply
        // is forwarded verbatim so the host's raw-input listener stage observes
        // it first, and Terminal::apply_cell_pixel_dimensions is the downstream
        // consumer. Keyboard-negotiation answers keep this stage, where frozen
        // pi consumes them; color and scheme answers belong to the TUI root
        // (#990).
        if (const auto* keyboard = std::get_if<detail::KeyboardProtocolResponse>(&response)) {
            apply_keyboard_response(impl, *keyboard);
            continue;
        }
        if (std::get_if<CursorPosition>(&response) != nullptr) {
            // A CPR arriving on the worker answers no query any more (#983):
            // startup no longer probes the cursor position, so consume and
            // ignore the unsolicited report.
            continue;
        }
    }
}
/// pi picks the fragment window from what the decoder still holds
/// (`this.buffer === ESC ? this.escapeTimeoutMs : this.timeoutMs`,
/// stdin-buffer.ts), so a chunk that leaves nothing held resolves immediately
/// and arms no deadline.
template <typename T> void arm_fragment_deadline(T& impl) {
    auto deadline = std::chrono::steady_clock::time_point::max();
    const auto& decoder = impl.input_state.decoder;
    const auto& stdin_buffer = impl.input_state.stdin_buffer;
    if (stdin_buffer.holds_fragment()) {
        deadline = std::min(deadline, stdin_buffer.deadline());
    }
    if (decoder.holds_lone_escape()) {
        deadline = std::min(deadline, std::chrono::steady_clock::now() + impl.input_state.escape_fragment_timeout);
    } else if (decoder.holds_fragment() || impl.input_state.needs_input_flush) {
        deadline = std::min(deadline, std::chrono::steady_clock::now() + detail::kSequenceFragmentTimeout);
    }
    impl.input_state.negotiation_deadline = deadline;
}

template <typename T> void process_input_chunk(T& impl, std::string_view chunk) {
    if (chunk.empty()) return;
    {
        std::lock_guard lock(impl.mutex);
        if (impl.draining) {
            impl.drain_last_activity = std::chrono::steady_clock::now();
            return;
        }
    }
    impl.input_state.stdin_buffer.process(chunk);
    arm_fragment_deadline(impl);
}

/// Re-assert the started-session invariant on the input descriptor.
/// `O_NONBLOCK` belongs to the open file description, which this process shares
/// with every child that inherits the descriptor: tmux, script, and the herdr
/// client all clear it on their own standard input. The descriptor has to stay
/// non-blocking for as long as the terminal is started, because the runtime
/// loop performs its next read from the read completion handler: Asio's
/// speculative read has no fresh readiness event behind it, so a blocking
/// descriptor parks the loop inside that read (and the mutex it holds) until a
/// key press arrives. One `F_GETFL` per read is cheap enough to make the
/// invariant self-healing instead of trusting every child that shares the
/// description.
template <typename T> void ensure_input_nonblocking(T& impl) noexcept {
    if (!impl.modes.started || impl.options.input_fd < 0) return;
    const int flags = ::fcntl(impl.options.input_fd, F_GETFL);
    if (flags == -1 || (flags & O_NONBLOCK) != 0) return;
    (void)::fcntl(impl.options.input_fd, F_SETFL, flags | O_NONBLOCK);
}

template <typename T> [[nodiscard]] support::ExpectedVoid restore_terminal_modes(T& impl);

template <typename T> void handle_pty_close(T& impl) {
    {
        std::lock_guard lock(impl.mutex);
        if (!impl.modes.started) return;
        if (impl.session_alive) {
            impl.session_alive->store(false);
        }
        if (impl.input_stream) {
            boost::system::error_code error;
            impl.input_stream->cancel(error);
            impl.input_stream->release();
            impl.input_stream.reset();
        }
        if (impl.output_stream) {
            boost::system::error_code error;
            impl.output_stream->cancel(error);
            impl.output_stream->release();
            impl.output_stream.reset();
        }
        if (impl.keepalive_timer) {
            impl.keepalive_timer->cancel();
            impl.keepalive_timer.reset();
        }
        if (impl.readiness_timer) {
            impl.readiness_timer->cancel();
            impl.readiness_timer.reset();
        }
        impl.output_wait_armed = false;
        impl.input_sink.reset();
        impl.resize_sink.reset();
        impl.modes.started = false;
        impl.modes.raw_input = false;
    }
    (void)restore_terminal_modes(impl);
}

template <typename T> void poll_nonblocking(T& impl) {
    {
        std::lock_guard lock(impl.mutex);
        if (!impl.modes.started) return;
        ensure_input_nonblocking(impl);
    }
    deliver_resize_if_changed(impl);
    drain_output(impl, true);

    std::array<char, 4096> buffer{};
    while (true) {
        const auto read_count = ::read(impl.options.input_fd, buffer.data(), buffer.size());
        if (read_count > 0) {
            process_input_chunk(impl, std::string_view(buffer.data(), static_cast<std::size_t>(read_count)));
        } else if (read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (read_count < 0 && errno == EINTR) {
            continue;
        } else if (read_count == 0 || (read_count < 0 && errno == EIO)) {
            handle_pty_close(impl);
            break;
        } else {
            break;
        }
    }
}
template <typename T> void arm_readiness_timer_locked(T& impl);

template <typename T> void start_async_read(T& impl) {
    if (!impl.input_stream || !impl.modes.started) return;
    // The re-arm below is the read Asio performs speculatively from this
    // completion handler; the descriptor must be non-blocking before it runs.
    ensure_input_nonblocking(impl);
    const auto weak_self = impl.self;
    const auto weak_alive = std::weak_ptr<std::atomic_bool>{impl.session_alive};
    impl.input_stream->async_read_some(boost::asio::buffer(impl.read_buffer),
            [weak_self, weak_alive](const boost::system::error_code& ec, std::size_t bytes_read) {
                const auto self = weak_self.lock();
                if (!self) return;
                auto& impl = *self;
                const auto alive = weak_alive.lock();
                if (!alive || !alive->load()) return;
                if (ec) {
                    if (ec == boost::asio::error::operation_aborted) {
                        return;
                    }
                    if (ec == boost::asio::error::eof || ec.value() == EIO) {
                        handle_pty_close(impl);
                        return;
                    }
                    record_worker_error(impl,
                            support::make_error(
                                    support::ErrorCode::Process, "Process Terminal input read failed", ec.message()));
                    return;
                }
                {
                    std::lock_guard lock(impl.mutex);
                    if (!impl.modes.started) return;
                }
                deliver_resize_if_changed(impl);
                drain_output(impl, true);
                if (bytes_read > 0) {
                    process_input_chunk(impl, std::string_view(impl.read_buffer.data(), bytes_read));
                }
                std::lock_guard lock(impl.mutex);
                if (!impl.modes.started) return;
                arm_readiness_timer_locked(impl);
                start_async_read(impl);
            });
}
template <typename T> void arm_readiness_timer_locked(T& impl) {
    if (!impl.readiness_timer || !impl.modes.started) return;
    auto deadline = impl.last_resize_check + kResizeWatchdogInterval;
    if (impl.input_state.negotiation_deadline < deadline) {
        deadline = impl.input_state.negotiation_deadline;
    }
    impl.readiness_timer->expires_at(deadline);
    const auto weak_self = impl.self;
    const auto weak_alive = std::weak_ptr<std::atomic_bool>{impl.session_alive};
    impl.readiness_timer->async_wait([weak_self, weak_alive](const boost::system::error_code& error) {
        const auto self = weak_self.lock();
        if (!self) return;
        auto& impl = *self;
        const auto alive = weak_alive.lock();
        if (!alive || !alive->load()) return;
        if (error == boost::asio::error::operation_aborted) return;
        if (error) {
            std::lock_guard lock(impl.mutex);
            if (!impl.worker_error) {
                impl.worker_error = support::make_error(
                        support::ErrorCode::Process, "Process Terminal readiness timer failed", error.message());
            }
            return;
        }

        deliver_resize_if_changed(impl);
        std::vector<detail::TerminalResponseVariant> responses;
        std::string forwarded_input;
        bool needs_input_flush = false;
        {
            std::lock_guard lock(impl.mutex);
            const auto now = std::chrono::steady_clock::now();
            if (impl.input_state.negotiation_deadline != std::chrono::steady_clock::time_point::max() &&
                    now >= impl.input_state.negotiation_deadline) {
                if (impl.input_state.stdin_buffer.holds_fragment() && now >= impl.input_state.stdin_buffer.deadline()) {
                    (void)impl.input_state.stdin_buffer.flush();
                }
                auto decoded = impl.input_state.decoder.flush();
                responses = std::move(decoded.responses);
                forwarded_input = std::move(decoded.forwarded_input);
                needs_input_flush = impl.input_state.needs_input_flush;
                impl.input_state.needs_input_flush = false;
                impl.input_state.negotiation_deadline = std::chrono::steady_clock::time_point::max();
            }
        }
        apply_terminal_responses(impl, responses);
        if (!forwarded_input.empty()) invoke_input(impl, std::move(forwarded_input));
        if (needs_input_flush) invoke_input(impl, {});
        std::lock_guard lock(impl.mutex);
        arm_readiness_timer_locked(impl);
    });
}

[[nodiscard]] std::optional<boost::asio::any_io_executor> normalized_executor(const std::any& value) {
    if (const auto* executor = std::any_cast<boost::asio::any_io_executor>(&value)) {
        return *executor;
    }
    if (const auto* executor = std::any_cast<boost::asio::io_context::executor_type>(&value)) {
        return boost::asio::any_io_executor{*executor};
    }
    return std::nullopt;
}

/// Allocate and configure all readiness objects before the first asynchronous
/// operation is initiated. Throwable allocation failures in this phase remain
/// recoverable by ProcessTerminal::start().
template <typename T> void prepare_async_io(T& impl) {
    if (!impl.modes.started) return;
    const auto executor = normalized_executor(impl.options.executor);
    if (!executor) return;
    if (!impl.input_stream) {
        impl.input_stream.emplace(*executor);
        boost::system::error_code error;
        impl.input_stream->assign(impl.options.input_fd, error);
        if (error) {
            impl.input_stream->release();
            impl.input_stream.reset();
            if (!impl.worker_error) {
                impl.worker_error = support::make_error(
                        support::ErrorCode::Process, "Process Terminal input readiness setup failed", error.message());
            }
            return;
        }
    }
    if (impl.options.output_fd != impl.options.input_fd && !impl.output_stream) {
        impl.output_stream.emplace(*executor);
        boost::system::error_code error;
        impl.output_stream->assign(impl.options.output_fd, error);
        if (error) {
            impl.output_stream->release();
            impl.output_stream.reset();
            if (!impl.worker_error) {
                impl.worker_error = support::make_error(
                        support::ErrorCode::Process, "Process Terminal output readiness setup failed", error.message());
            }
        }
    }
    if (!impl.keepalive_timer) impl.keepalive_timer.emplace(*executor);
    if (!impl.readiness_timer) impl.readiness_timer.emplace(*executor);
}

/// Initiate readiness operations only after prepare_async_io() has completed.
/// No exception boundary belongs around this function: once the first async
/// operation is initiated, startup has crossed into the post-initiation phase.
template <typename T> void start_async_io(T& impl) noexcept {
    if (!impl.modes.started) return;
    if (impl.input_stream) start_async_read(impl);
    arm_output_wait_locked(impl);
    arm_keepalive_timer_locked(impl);
    arm_readiness_timer_locked(impl);
}

template <typename T> [[nodiscard]] support::ExpectedVoid restore_terminal_modes(T& impl) {
    support::ExpectedVoid first_error;

    // Restore the output descriptor to its original flags first so the
    // restoration writes below run on a blocking descriptor (the worker has
    // already drained the ordered output queue before stop() restores).
    if (impl.output_nonblock && impl.original_fd_flags >= 0) {
        if (::fcntl(impl.options.output_fd, F_SETFL, impl.original_fd_flags) != 0) {
            retain_error(first_error,
                    std::unexpected(process_error(
                            "Process Terminal could not restore output descriptor flags", "fcntl(F_SETFL)", errno)));
        } else {
            impl.output_nonblock = false;
        }
    }
    if (impl.input_nonblock && impl.original_input_flags >= 0) {
        if (::fcntl(impl.options.input_fd, F_SETFL, impl.original_input_flags) != 0) {
            retain_error(first_error,
                    std::unexpected(process_error(
                            "Process Terminal could not restore input descriptor flags", "fcntl(F_SETFL)", errno)));
        } else {
            impl.input_nonblock = false;
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
    if (impl.modes.alternate_screen) {
        auto restored = write_all(impl.options.output_fd, kExitAlternateScreen);
        if (restored) impl.modes.alternate_screen = false;
        retain_error(first_error, std::move(restored));
    }
    if (!impl.modes.autowrap) {
        auto enabled = write_all(impl.options.output_fd, kEnableAutowrap);
        if (enabled) impl.modes.autowrap = true;
        retain_error(first_error, std::move(enabled));
    }
    if (!impl.modes.cursor_visible) {
        auto shown = write_all(impl.options.output_fd, kCursorShow);
        if (shown) impl.modes.cursor_visible = true;
        retain_error(first_error, std::move(shown));
    }
    if (impl.modes.bracketed_paste) {
        auto disabled = write_all(impl.options.output_fd, kBracketedPasteDisable);
        if (disabled) impl.modes.bracketed_paste = false;
        retain_error(first_error, std::move(disabled));
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
        if (::tcsetattr(impl.options.input_fd, TCSADRAIN, &impl.original_termios) == 0) {
            impl.has_original_termios = false;
            impl.modes.raw_input = false;
        } else {
            if (errno == EIO) {
                impl.has_original_termios = false;
                impl.modes.raw_input = false;
            }
            retain_error(first_error,
                    std::unexpected(
                            process_error("Process Terminal could not restore input mode", "tcsetattr", errno)));
        }
    }

    return first_error;
}

template <typename T> void release_unstarted_async_io(T& impl) {
    // prepare_async_io() may have assigned the non-owning descriptors before
    // a later readiness object allocation failed. Release those handles before
    // destroying the Asio wrappers so rollback never closes caller-owned fds.
    if (impl.input_stream) {
        impl.input_stream->release();
        impl.input_stream.reset();
    }
    if (impl.output_stream) {
        impl.output_stream->release();
        impl.output_stream.reset();
    }
    impl.keepalive_timer.reset();
    impl.readiness_timer.reset();
    impl.output_wait_armed = false;
}

template <typename T> void abandon_startup(T& impl) {
    impl.modes.started = false;
    if (impl.session_alive) {
        impl.session_alive->store(false);
        impl.session_alive.reset();
    }
    impl.input_sink.reset();
    impl.resize_sink.reset();
    release_unstarted_async_io(impl);
    impl.input_state.decoder.reset();
    impl.input_state.needs_input_flush = false;
    impl.input_state.negotiation_deadline = std::chrono::steady_clock::time_point::max();
    impl.startup_input.clear();
    impl.output_queue.clear();
    impl.output_queued_bytes = 0;
    impl.output_draining = false;
    impl.synchronized_output.clear();
    impl.synchronized_output_state.reset();
    impl.worker_error.reset();
}

template <typename T>
[[nodiscard]] support::ExpectedVoid leave_output_only_locked(T& impl, bool emit_paste_off = false) {
    if (!impl.output_only) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Process Terminal is not in output-only mode"));
    }
    impl.output_only = false;
    support::ExpectedVoid result;

    if (impl.synchronized_update_depth > 0) {
        impl.synchronized_output.append(kEndSynchronizedUpdate);
        impl.synchronized_update_depth = 0;
        auto frame = std::move(impl.synchronized_output);
        impl.synchronized_output.clear();
        auto frame_res = enqueue_output(impl, frame);
        if (!frame_res && impl.synchronized_output_state) {
            impl.cursor = impl.synchronized_output_state->cursor;
            impl.scroll_origin = impl.synchronized_output_state->scroll_origin;
            impl.viewport_top = impl.synchronized_output_state->viewport_top;
            impl.next_image_handle = impl.synchronized_output_state->next_image_handle;
            impl.margin_top = impl.synchronized_output_state->margin_top;
            impl.margin_bottom = impl.synchronized_output_state->margin_bottom;
            impl.margins_active = impl.synchronized_output_state->margins_active;
        }
        impl.synchronized_output_state.reset();
        retain_error(result, std::move(frame_res));
    }

    if (impl.margins_active) {
        auto reset_margins = enqueue_output(impl, "\x1b[r");
        if (reset_margins) {
            impl.margins_active = false;
            impl.margin_top = 0;
            impl.margin_bottom = 0;
        }
        retain_error(result, std::move(reset_margins));
    }

    if (!impl.modes.autowrap) {
        auto enable_autowrap = enqueue_output(impl, kEnableAutowrap);
        if (enable_autowrap) {
            impl.modes.autowrap = true;
        }
        retain_error(result, std::move(enable_autowrap));
    }

    if (!impl.modes.cursor_visible) {
        auto show_cursor = enqueue_output(impl, kCursorShow);
        if (show_cursor) {
            impl.modes.cursor_visible = true;
        }
        retain_error(result, std::move(show_cursor));
    }

    if (emit_paste_off) {
        retain_error(result, enqueue_output(impl, kBracketedPasteDisable));
    }

    drain_output_locked(impl, true);

    if (impl.worker_error) {
        retain_error(result, std::unexpected(std::move(*impl.worker_error)));
        impl.worker_error.reset();
    } else if (!impl.output_queue.empty()) {
        retain_error(result,
                std::unexpected(support::make_error(support::ErrorCode::Busy,
                        "Process Terminal output is backed up",
                        std::format("the output queue ({} bytes remaining) could not be drained",
                                impl.output_queued_bytes))));
    }
    impl.output_queue.clear();
    impl.output_queued_bytes = 0;
    impl.output_draining = false;

    if (impl.original_output_only_flags >= 0) {
        if (::fcntl(impl.options.output_fd, F_SETFL, impl.original_output_only_flags) != 0) {
            retain_error(result,
                    std::unexpected(process_error(
                            "Process Terminal could not restore output descriptor flags", "fcntl(F_SETFL)", errno)));
        } else {
            impl.original_output_only_flags = -1;
            impl.output_nonblock = false;
        }
    }
    return result;
}

} // namespace

ProcessTerminal::ProcessTerminal(ProcessTerminalOptions options) : impl_(std::make_shared<Impl>(options)) {
    // pi `getCapabilities()` (packages/tui terminal-image.ts) is synchronous
    // and feeds `initTheme` ahead of TUI construction, so the env-derived
    // color depth and appearance must already be observable here: the
    // interactive engine builds its ThemeController from `capabilities()`
    // before `start()`. The live scheme and OSC color queries belong to the
    // TUI root (#990); `start()` detects the probe-backed capabilities
    // (inline images, hyperlinks, synchronized output) only.
    impl_->capabilities.color = detect_color_capability();
    impl_->capabilities.appearance = detect_terminal_appearance();
    impl_->write_log_path = resolve_write_log_path();
    impl_->self = impl_;
}

ProcessTerminal::~ProcessTerminal() { (void)stop(); }

support::ExpectedVoid ProcessTerminal::enter_output_only() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->modes.started) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Process Terminal is already started"));
    }
    if (!impl_->was_started) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Process Terminal must complete start/stop lifetime before output-only presentation"));
    }
    if (impl_->has_original_termios || impl_->input_nonblock || impl_->output_nonblock ||
            impl_->original_output_only_flags >= 0 || impl_->modes.raw_input || impl_->modes.alternate_screen ||
            impl_->modes.bracketed_paste || !impl_->modes.cursor_visible || !impl_->modes.autowrap ||
            impl_->keyboard_protocol_pushed || impl_->modify_other_keys_active ||
            impl_->synchronized_update_depth > 0 || impl_->progress_active || impl_->margins_active) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Process Terminal cannot enter output-only mode while terminal modes or input state remain "
                "unrestored"));
    }
    if (impl_->output_only) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Process Terminal is already in output-only mode"));
    }

    const auto flags = ::fcntl(impl_->options.output_fd, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(process_error("Process Terminal could not get output flags", "fcntl(F_GETFL)", errno));
    }
    if ((flags & O_NONBLOCK) == 0) {
        if (::fcntl(impl_->options.output_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            return std::unexpected(
                    process_error("Process Terminal could not set nonblocking output", "fcntl(F_SETFL)", errno));
        }
        impl_->original_output_only_flags = flags;
        impl_->output_nonblock = true;
    }
    impl_->output_only = true;
    return {};
}

support::ExpectedVoid ProcessTerminal::leave_output_only() {
    std::lock_guard lock(impl_->mutex);
    return leave_output_only_locked(*impl_, false);
}

const std::string& ProcessTerminal::write_log_path() const noexcept { return impl_->write_log_path; }
support::ExpectedVoid ProcessTerminal::start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (impl_->modes.started) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Process Terminal is already started"));
    }
    if (impl_->output_only) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Process Terminal cannot start while in output-only mode"));
    }
    if (impl_->has_original_termios || impl_->modes.alternate_screen || impl_->modes.bracketed_paste ||
            !impl_->modes.cursor_visible || !impl_->modes.autowrap || impl_->input_nonblock || impl_->output_nonblock ||
            impl_->original_output_only_flags >= 0 || impl_->keyboard_protocol_pushed ||
            impl_->modify_other_keys_active || impl_->synchronized_update_depth > 0 || impl_->progress_active ||
            impl_->margins_active) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Process, "Process Terminal has unrestored state from a previous stop"));
    }
    if (::isatty(impl_->options.input_fd) != 1 || ::isatty(impl_->options.output_fd) != 1) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Process Terminal requires TTY input and output descriptors",
                std::format("input fd {} and output fd {} must both refer to terminals",
                        impl_->options.input_fd,
                        impl_->options.output_fd)));
    }

    termios original{};
    if (::tcgetattr(impl_->options.input_fd, &original) != 0) {
        return std::unexpected(process_error("Process Terminal could not snapshot input mode", "tcgetattr", errno));
    }
    auto dimensions = read_dimensions(impl_->options.output_fd);
    if (!dimensions) return std::unexpected(dimensions.error());
    impl_->startup_input.clear();
    const auto detected_images = detect_image_capabilities();
    impl_->capabilities = TerminalCapabilities{
            .synchronized_output = supports_synchronized_output(),
            .inline_images = detected_images.images,
            .hyperlinks = detected_images.hyperlinks,
            .color = detect_color_capability(),
            .appearance = detect_terminal_appearance(),
    };
    // One startup-failure path: roll back acquired terminal state and combine
    // the acquisition error with any incomplete rollback (issue #462). The
    // same path is used after `started` is set, before asynchronous initiation.
    const auto fail_startup = [this, &lock](support::Error acquisition_error) -> support::ExpectedVoid {
        if (!lock.owns_lock()) lock.lock();
        abandon_startup(*impl_);
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(startup_failure(std::move(acquisition_error), rollback));
    };

    std::shared_ptr<TerminalInputSink> owned_input_sink;
    std::shared_ptr<TerminalResizeSink> owned_resize_sink;
    owned_input_sink = std::make_shared<TerminalInputSink>(std::move(input_sink));
    owned_resize_sink = std::make_shared<TerminalResizeSink>(std::move(resize_sink));

    auto raw = original;
    ::cfmakeraw(&raw);
    if (::tcsetattr(impl_->options.input_fd, TCSAFLUSH, &raw) != 0) {
        return std::unexpected(process_error("Process Terminal could not acquire raw input mode", "tcsetattr", errno));
    }
    impl_->original_termios = original;
    impl_->has_original_termios = true;
    impl_->modes.raw_input = true;

    auto paste_attempt = attempt_write_all(impl_->options.output_fd, kBracketedPasteEnable);
    if (!paste_attempt.result) {
        return fail_startup(paste_attempt.result.error());
    }
    impl_->modes.bracketed_paste = true;

    impl_->scroll_origin = 0;
    impl_->viewport_top = 0;
    impl_->cursor = {};

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
        auto cell_size_query = attempt_write_all(impl_->options.output_fd, kCellSizeQuery);
        if (!cell_size_query.result) {
            return fail_startup(cell_size_query.result.error());
        }
    }
    const int original_input_flags = ::fcntl(impl_->options.input_fd, F_GETFL);
    if (original_input_flags < 0) {
        return fail_startup(
                process_error("Process Terminal could not read input descriptor flags", "fcntl(F_GETFL)", errno));
    }

    const int original_flags = ::fcntl(impl_->options.output_fd, F_GETFL);
    if (original_flags < 0) {
        return fail_startup(
                process_error("Process Terminal could not read output descriptor flags", "fcntl(F_GETFL)", errno));
    }
    impl_->original_fd_flags = original_flags;
    if (::fcntl(impl_->options.output_fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        return fail_startup(
                process_error("Process Terminal could not set non-blocking output", "fcntl(F_SETFL)", errno));
    }
    impl_->output_nonblock = true;

    impl_->original_input_flags = original_input_flags;
    if (::fcntl(impl_->options.input_fd, F_SETFL, original_input_flags | O_NONBLOCK) != 0) {
        return fail_startup(
                process_error("Process Terminal could not set non-blocking input", "fcntl(F_SETFL)", errno));
    }
    impl_->input_nonblock = true;

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
    impl_->was_started = true;
    impl_->session_alive = std::make_shared<std::atomic_bool>(true);

    auto startup_input = std::move(impl_->startup_input);
    impl_->startup_input.clear();
    lock.unlock();
    // A restart must not inherit the previous session's decoder fragment,
    // negotiation deadline, or flush flag (#613).
    impl_->input_state = InputState{};
    // The started TUI root owns color and scheme reporting (#990): this edge
    // forwards OSC color replies and `CSI ? 997 n` reports verbatim so the
    // TUI's own decoder consumes them before typed input dispatch, while
    // keyboard negotiation still resolves at this stage.
    impl_->input_state.decoder.set_demux_appearance(false);
    impl_->input_state.escape_fragment_timeout =
            detail::resolve_escape_fragment_timeout(environment("PI_TUI_ESC_TIMEOUT"),
                    !environment("SSH_CONNECTION").empty() || !environment("SSH_TTY").empty());
    configure_stdin_buffer(*impl_);
    if (!startup_input.empty()) {
        impl_->input_state.stdin_buffer.process(startup_input);
        arm_fragment_deadline(*impl_);
    }
    lock.lock();
    prepare_async_io(*impl_);
    // The preparation phase above has no outstanding asynchronous operations.
    // From this call onward initiation is in progress; exceptions intentionally
    // follow the post-initiation contract rather than rolling state back.
    start_async_io(*impl_);
    return {};
}

support::ExpectedVoid ProcessTerminal::stop() { return stop(TerminalStopOptions{}); }

support::ExpectedVoid ProcessTerminal::stop(TerminalStopOptions options) {
    std::lock_guard lock(impl_->mutex);
    if (options.final_alternate_restore) {
        if (!impl_->modes.started) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "Process Terminal must be started for final alternate restore stop"));
        }
        if (!impl_->modes.alternate_screen) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "Process Terminal is not in alternate screen for final alternate restore stop"));
        }
    }

    if (!impl_->modes.started && !impl_->output_only) {
        return {};
    }

    if (impl_->output_only) {
        const bool emit_paste_off = impl_->was_started;
        return leave_output_only_locked(*impl_, emit_paste_off);
    }

    if (impl_->session_alive) {
        impl_->session_alive->store(false);
    }

    if (impl_->input_stream) {
        boost::system::error_code error;
        impl_->input_stream->cancel(error);
        impl_->input_stream->release();
        impl_->input_stream.reset();
    }
    if (impl_->output_stream) {
        boost::system::error_code error;
        impl_->output_stream->cancel(error);
        impl_->output_stream->release();
        impl_->output_stream.reset();
    }
    if (impl_->keepalive_timer) {
        impl_->keepalive_timer->cancel();
        impl_->keepalive_timer.reset();
    }
    if (impl_->readiness_timer) {
        impl_->readiness_timer->cancel();
        impl_->readiness_timer.reset();
    }
    impl_->output_wait_armed = false;

    impl_->input_sink.reset();
    impl_->resize_sink.reset();
    impl_->modes.started = false;

    support::ExpectedVoid result;
    if (impl_->worker_error) {
        retain_error(result, std::unexpected(std::move(*impl_->worker_error)));
        impl_->worker_error.reset();
    }

    if (options.final_alternate_restore) {
        // Atomic stop branch:

        // 0. Pending synchronized update: flush open frame into output queue
        // so nested/partial frame payloads are not dropped, and depth is reset.
        if (impl_->synchronized_update_depth > 0) {
            retain_error(result, flush_synchronized_update_locked(*impl_));
        }

        // 1. Paste off
        bool bracketed_paste_enqueued = false;
        if (impl_->modes.bracketed_paste) {
            if (auto res = enqueue_output(*impl_, kBracketedPasteDisable); res) {
                bracketed_paste_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }

        // 2. Keyboard disables
        bool modify_other_keys_enqueued = false;
        if (impl_->modify_other_keys_active) {
            if (auto res = enqueue_output(*impl_, kModifyOtherKeysDisable); res) {
                modify_other_keys_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }
        bool keyboard_protocol_enqueued = false;
        if (impl_->keyboard_protocol_pushed) {
            if (auto res = enqueue_output(*impl_, kKeyboardProtocolPop); res) {
                keyboard_protocol_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }

        // 3. Input raw restore (before final sync altExit) using TCSANOW.
        // TCSANOW ensures we do not block waiting for saturated output buffers to drain.
        if (impl_->has_original_termios) {
            if (::tcsetattr(impl_->options.input_fd, TCSANOW, &impl_->original_termios) == 0) {
                impl_->has_original_termios = false;
                impl_->modes.raw_input = false;
            } else {
                if (errno == EIO) {
                    impl_->has_original_termios = false;
                    impl_->modes.raw_input = false;
                }
                retain_error(result,
                        std::unexpected(
                                process_error("Process Terminal could not restore input mode", "tcsetattr", errno)));
            }
        }

        // 4. Autowrap restore if disabled directly via API
        bool autowrap_enqueued = false;
        if (!impl_->modes.autowrap) {
            if (auto res = enqueue_output(*impl_, kEnableAutowrap); res) {
                autowrap_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }

        // 5. Final SYNC altExit / show
        constexpr std::string_view kFinalAlternateRestoreFrame = "\x1b[?2026h\x1b[?1049l\x1b[?25h\x1b[?2026l";
        bool alt_restore_enqueued = false;
        if (auto res = enqueue_output(*impl_, kFinalAlternateRestoreFrame); res) {
            alt_restore_enqueued = true;
        } else {
            retain_error(result, std::move(res));
        }

        bool progress_enqueued = false;
        if (impl_->progress_active) {
            if (auto res = enqueue_output(*impl_, kProgressClearSequence); res) {
                progress_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }
        bool margins_enqueued = false;
        if (impl_->margins_active) {
            if (auto res = enqueue_output(*impl_, "\x1b[r"); res) {
                margins_enqueued = true;
            } else {
                retain_error(result, std::move(res));
            }
        }

        // 6. Drain output BEFORE restoring descriptor flags.
        // Output descriptor has remained nonblocking throughout all enqueues and drain.
        drain_output_locked(*impl_, true);

        // Check for fatal worker_error during drain (e.g. fatal EIO on closed master)
        const bool drain_fatal = impl_->worker_error.has_value();
        if (drain_fatal) {
            retain_error(result, std::unexpected(std::move(*impl_->worker_error)));
            impl_->worker_error.reset();
        }

        const bool queue_backed_up = !impl_->output_queue.empty();
        if (queue_backed_up) {
            retain_error(result,
                    std::unexpected(support::make_error(support::ErrorCode::Busy,
                            "Process Terminal output is backed up",
                            std::format("the output queue ({} bytes remaining) could not be drained",
                                    impl_->output_queued_bytes))));
        }

        const bool drain_clean = !drain_fatal && !queue_backed_up;
        if (drain_clean) {
            if (bracketed_paste_enqueued) impl_->modes.bracketed_paste = false;
            if (modify_other_keys_enqueued) impl_->modify_other_keys_active = false;
            if (keyboard_protocol_enqueued) {
                impl_->keyboard_protocol_pushed = false;
                impl_->capabilities.keyboard_protocol = KeyboardProtocol::Legacy;
            }
            if (autowrap_enqueued) impl_->modes.autowrap = true;
            if (alt_restore_enqueued) {
                impl_->modes.alternate_screen = false;
                impl_->modes.cursor_visible = true;
            }
            if (progress_enqueued) impl_->progress_active = false;
            if (margins_enqueued) {
                impl_->margins_active = false;
                impl_->margin_top = 0;
                impl_->margin_bottom = 0;
            }
        }

        // 7. Restore descriptor flags AFTER last write and drain.
        // Restoring output_fd and input_fd flags here ensures that sharing the OFD
        // between input_fd and output_fd (e.g. same slave fd or dup) never restored
        // blocking mode during writes or TCSADRAIN.
        if (impl_->output_nonblock && impl_->original_fd_flags >= 0) {
            if (::fcntl(impl_->options.output_fd, F_SETFL, impl_->original_fd_flags) != 0) {
                retain_error(result,
                        std::unexpected(process_error("Process Terminal could not restore output descriptor flags",
                                "fcntl(F_SETFL)",
                                errno)));
            } else {
                impl_->output_nonblock = false;
            }
        }
        if (impl_->input_nonblock && impl_->original_input_flags >= 0) {
            if (::fcntl(impl_->options.input_fd, F_SETFL, impl_->original_input_flags) != 0) {
                retain_error(result,
                        std::unexpected(process_error(
                                "Process Terminal could not restore input descriptor flags", "fcntl(F_SETFL)", errno)));
            } else {
                impl_->input_nonblock = false;
            }
        }

        impl_->output_queue.clear();
        impl_->output_queued_bytes = 0;
        impl_->output_draining = false;
        impl_->synchronized_output.clear();
        impl_->synchronized_output_state.reset();
        return result;
    }

    // Ordinary default stop:
    if (impl_->synchronized_update_depth > 0) {
        retain_error(result, flush_synchronized_update_locked(*impl_));
    }

    drain_output_locked(*impl_, true);
    if (impl_->worker_error) {
        retain_error(result, std::unexpected(std::move(*impl_->worker_error)));
        impl_->worker_error.reset();
    }
    impl_->output_queue.clear();
    impl_->output_queued_bytes = 0;
    impl_->output_draining = false;
    impl_->synchronized_output.clear();
    impl_->synchronized_output_state.reset();

    retain_error(result, restore_terminal_modes(*impl_));
    return result;
}
TerminalDimensions ProcessTerminal::dimensions() const {
    bool started = false;
    {
        std::lock_guard lock(impl_->mutex);
        started = impl_->modes.started;
    }
    if (started) deliver_resize_if_changed(*impl_);
    std::lock_guard lock(impl_->mutex);
    return impl_->dimensions;
}

TerminalCapabilities ProcessTerminal::capabilities() const {
    bool should_poll = false;
    {
        std::lock_guard lock(impl_->mutex);
        should_poll = !impl_->options.executor.has_value() && impl_->modes.started;
    }
    if (should_poll) poll_nonblocking(*impl_);
    std::lock_guard lock(impl_->mutex);
    return impl_->capabilities;
}

TerminalModeState ProcessTerminal::modes() const {
    bool should_poll = false;
    {
        std::lock_guard lock(impl_->mutex);
        should_poll = !impl_->options.executor.has_value() && impl_->modes.started;
    }
    if (should_poll) poll_nonblocking(*impl_);
    std::lock_guard lock(impl_->mutex);
    return impl_->modes;
}

support::ExpectedVoid ProcessTerminal::clear_screen() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    // pi's resize full-redraw clears scrollback too (`\x1b[3J`): the native
    // scroll history, the mirrored scroll count, and the anchor all reset
    // (the screen is cleared and homed, so buffer row 0 lands on screen row 0).
    // A refused clear leaves all of that alone; the caller retries the frame.
    auto result = enqueue_output(*impl_, kClearScreen);
    if (result) {
        impl_->viewport_top = 0;
        impl_->scroll_origin = 0;
    }
    return result;
}

support::ExpectedVoid ProcessTerminal::write(std::string_view output) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    auto result = enqueue_output(*impl_, output);
    if (result) append_write_log(*impl_, output);
    return result;
}

support::ExpectedVoid ProcessTerminal::set_cursor(CursorPosition position) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    if (position.column > impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Process Terminal cursor position is outside its dimensions"));
    }
    // Cursor and scroll accounting describe admitted output only: a bounded
    // queue refusal (Busy) leaves no bytes on the terminal, so the state moves
    // exactly when its escape sequence is admitted. Otherwise a refused write
    // would leave the terminal's scroll anchor ahead of its physical content
    // and every later row would land one line off (#732).
    const auto commit_cursor = [&](std::size_t row, std::size_t column, std::size_t scroll) {
        impl_->viewport_top += scroll;
        impl_->cursor.row = row;
        impl_->cursor.column = column;
    };
    // `row` is a buffer row under the main-screen scrollback flow: the
    // renderer writes the full composed buffer in increasing row order, and
    // rows at or past the visible viewport bottom must advance the terminal's
    // native scrollback so the addressed line becomes the bottom visible line.
    // Absolute CUP beyond the bottom clamps without scrolling on real
    // terminals, so mirror pi's append (`"\r\n".repeat(scroll)`) by moving to
    // the bottom and emitting CRLF line flow: the real terminal scrolls its
    // scroll history natively. Relative flow keeps the buffer origin at 0, so
    // the screen row of buffer row b is `b - viewport_top`, and the emulated
    // scroll count stays in sync with the renderer's own tracked viewport.
    const auto first_visible_row =
            impl_->viewport_top > impl_->scroll_origin ? impl_->viewport_top - impl_->scroll_origin : std::size_t{0};
    if (position.row < first_visible_row) {
        // Rows above the first visible buffer row are in the terminal's
        // scrollback; as before the anchor, they clamp to screen row 0 (or margin_top).
        const auto row = impl_->margins_active ? impl_->margin_top : 0;
        auto result = enqueue_output(*impl_, std::format("\x1b[{};{}H", row + 1, position.column + 1));
        if (result) commit_cursor(row, position.column, 0);
        return result;
    }
    const auto screen_row = impl_->scroll_origin + position.row - impl_->viewport_top;
    if (impl_->margins_active) {
        const auto viewport_height = impl_->margin_bottom - impl_->margin_top + 1;
        if (screen_row >= viewport_height) {
            const auto scroll = screen_row - (viewport_height - 1);
            std::string sequence;
            sequence += std::format("\x1b[{};1H", impl_->margin_bottom + 1);
            sequence.append(scroll, '\n');
            sequence += std::format("\x1b[{};{}H", impl_->margin_bottom + 1, position.column + 1);
            auto result = enqueue_output(*impl_, sequence);
            if (result) commit_cursor(impl_->margin_bottom, position.column, scroll);
            return result;
        }
        const auto row = impl_->margin_top + screen_row;
        auto result = enqueue_output(*impl_, std::format("\x1b[{};{}H", row + 1, position.column + 1));
        if (result) commit_cursor(row, position.column, 0);
        return result;
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
        auto result = enqueue_output(*impl_, sequence);
        if (result) commit_cursor(bottom, position.column, scroll);
        return result;
    }
    auto result = enqueue_output(*impl_, std::format("\x1b[{};{}H", screen_row + 1, position.column + 1));
    if (result) commit_cursor(screen_row, position.column, 0);
    return result;
}

support::ExpectedVoid ProcessTerminal::set_scroll_margins(std::size_t top_row, std::size_t bottom_row) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    // An equal top and bottom is a valid one-row scroll region (#611).
    if (bottom_row >= impl_->dimensions.rows || top_row > bottom_row) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Process Terminal scroll margins are invalid"));
    }
    // The active region follows its admitted escape sequence, like the cursor
    // state in `set_cursor`: a refused margin change must not leave the
    // renderer addressing rows in a region the terminal does not have (#732).
    auto sequence = std::format("\x1b[{};{}r", top_row + 1, bottom_row + 1);
    sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, impl_->cursor.column + 1);
    auto result = enqueue_output(*impl_, sequence);
    if (result) {
        impl_->margin_top = top_row;
        impl_->margin_bottom = bottom_row;
        impl_->margins_active = true;
    }
    return result;
}

support::ExpectedVoid ProcessTerminal::reset_scroll_margins() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    if (!impl_->margins_active) return {};
    auto sequence = std::format("\x1b[1;{}r", impl_->dimensions.rows);
    sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, impl_->cursor.column + 1);
    auto result = enqueue_output(*impl_, sequence);
    if (result) {
        impl_->margins_active = false;
        impl_->margin_top = 0;
        impl_->margin_bottom = 0;
    }
    return result;
}

support::ExpectedVoid ProcessTerminal::set_cursor_visible(bool visible) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.cursor_visible == visible) return {};
    auto result = enqueue_output(*impl_, visible ? kCursorShow : kCursorHide);
    if (result) impl_->modes.cursor_visible = visible;
    return result;
}

support::ExpectedVoid ProcessTerminal::set_alternate_screen(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.alternate_screen == enabled) return {};
    auto result = enqueue_output(*impl_, enabled ? kEnterAlternateScreen : kExitAlternateScreen);
    if (result) impl_->modes.alternate_screen = enabled;
    return result;
}

support::ExpectedVoid ProcessTerminal::set_autowrap(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.autowrap == enabled) return {};
    auto result = enqueue_output(*impl_, enabled ? kEnableAutowrap : kDisableAutowrap);
    if (result) impl_->modes.autowrap = enabled;
    return result;
}

support::ExpectedVoid ProcessTerminal::begin_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
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
    if (auto started = require_output_admission(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth == 0) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "Process Terminal synchronized update is not active"));
    }
    if (impl_->synchronized_update_depth > 1) {
        --impl_->synchronized_update_depth;
        return {};
    }

    return flush_synchronized_update_locked(*impl_);
}

support::ExpectedVoid ProcessTerminal::apply_cell_pixel_dimensions(CellPixelDimensions pixels) {
    // Reached on the host's own input path (the TUI calls it after its raw-input
    // listener stage), never from this worker while the input sink runs, so the
    // mutex below is free; apply_cell_size_response copies the sink out and
    // invokes it unlocked.
    apply_cell_size_response(*impl_, pixels);
    return {};
}

support::ExpectedVoid ProcessTerminal::set_title(std::string_view title) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    return enqueue_output(*impl_, std::format("\x1b]0;{}\x07", title));
}

support::ExpectedVoid ProcessTerminal::set_progress(bool active) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    auto result = enqueue_output(*impl_, active ? kProgressActiveSequence : kProgressClearSequence);
    if (!result) return result;
    if (active) {
        if (!impl_->progress_active) {
            impl_->progress_active = true;
            impl_->progress_next_keepalive = std::chrono::steady_clock::now() + kProgressKeepalive;
            arm_keepalive_timer_locked(*impl_);
        }
    } else {
        impl_->progress_active = false;
        if (impl_->keepalive_timer) {
            impl_->keepalive_timer->cancel();
        }
    }
    return {};
}

support::ExpectedVoid ProcessTerminal::drain_input(
        std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) {
    std::unique_lock lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    // Asserted under the lock, before the drain releases it: the loop below
    // reads until EAGAIN and assumes nobody cleared the flag on the shared
    // description while it runs.
    ensure_input_nonblocking(*impl_);
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

    drain_output_locked(*impl_, true);

    impl_->draining = true;
    impl_->drain_last_activity = std::chrono::steady_clock::now();
    auto last_activity = impl_->drain_last_activity;
    lock.unlock();

    const auto deadline = last_activity + max_ms;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto idle_elapsed = now - last_activity;
        if (idle_elapsed >= idle_ms) break;
        const auto remaining = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(idle_ms - idle_elapsed),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));

        pollfd pfd{.fd = impl_->options.input_fd, .events = POLLIN, .revents = 0};
        const int timeout_ms = std::max(1, static_cast<int>(remaining.count()));
        const int poll_result = ::poll(&pfd, 1, timeout_ms);
        if (poll_result > 0 && (pfd.revents & POLLIN) != 0) {
            bool read_any = false;
            while (true) {
                const auto read_count = ::read(impl_->options.input_fd, buffer.data(), buffer.size());
                if (read_count > 0) {
                    read_any = true;
                    continue;
                }
                break;
            }
            if (read_any) {
                last_activity = std::chrono::steady_clock::now();
                std::lock_guard guard(impl_->mutex);
                impl_->drain_last_activity = last_activity;
            }
        } else if (poll_result == 0) {
            std::lock_guard guard(impl_->mutex);
            if (impl_->drain_last_activity > last_activity) {
                last_activity = impl_->drain_last_activity;
            } else {
                break;
            }
        } else if (poll_result < 0 && errno != EINTR) {
            break;
        }
    }

    lock.lock();
    impl_->draining = false;
    return {};
}

namespace detail::testing {

bool input_holds_fragment(const ProcessTerminal& terminal) noexcept {
    std::lock_guard lock(terminal.impl_->mutex);
    return terminal.impl_->input_state.stdin_buffer.holds_fragment() ||
           terminal.impl_->input_state.decoder.holds_fragment();
}

} // namespace detail::testing

} // namespace cch::tui
