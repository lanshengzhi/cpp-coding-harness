#include <cch/tui/ProcessTerminal.hpp>

#include <cch/tui/TerminalImage.hpp>

#include "KeyboardProtocol.hpp"
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

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

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
constexpr std::string_view kModifyOtherKeysEnable = "\x1b[>4;2m";
constexpr std::string_view kModifyOtherKeysDisable = "\x1b[>4;0m";
constexpr std::string_view kProgressActiveSequence = "\x1b]9;4;3\x07";
constexpr std::string_view kProgressClearSequence = "\x1b]9;4;0;\x07";
constexpr auto kProgressKeepalive = std::chrono::milliseconds(1000);
/// Escape-sequence and negotiation idle flush window: after input leaves a
/// partial protocol/appearance response (or a decoder flush) pending, the
/// delivery worker flushes it once this window passes (was 15 polls at 10 ms
/// in the periodic-polling design; issue #462).
constexpr auto kNegotiationTimeout = std::chrono::milliseconds(150);
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

#if defined(__linux__) || defined(__APPLE__)
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

[[nodiscard]] TerminalAppearance appearance_for_rgb(const EnvironmentRgb& rgb) {
    const auto luminance = 0.2126 * linear_channel(rgb.red) +
        0.7152 * linear_channel(rgb.green) + 0.0722 * linear_channel(rgb.blue);
    return luminance >= 0.5 ? TerminalAppearance::Light : TerminalAppearance::Dark;
}

[[nodiscard]] std::optional<int> parse_osc_hex_channel(std::string_view channel) {
    if (channel.empty() || channel.size() > 8) return std::nullopt;
    std::uint64_t value = 0;
    std::uint64_t maximum = 0;
    for (const auto character : channel) {
        int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = character - '0';
        } else if (character >= 'a' && character <= 'f') {
            digit = 10 + character - 'a';
        } else if (character >= 'A' && character <= 'F') {
            digit = 10 + character - 'A';
        } else {
            return std::nullopt;
        }
        value = value * 16 + static_cast<std::uint64_t>(digit);
        maximum = maximum * 16 + 15;
    }
    if (maximum == 0) return std::nullopt;
    return static_cast<int>(std::round(static_cast<double>(value) / static_cast<double>(maximum) * 255.0));
}

[[nodiscard]] std::optional<EnvironmentRgb> parse_osc_background(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.starts_with('#')) {
        value.remove_prefix(1);
        if (value.size() != 6 && value.size() != 12) return std::nullopt;
        const auto channel_width = value.size() / 3;
        const auto red = parse_osc_hex_channel(value.substr(0, channel_width));
        const auto green = parse_osc_hex_channel(value.substr(channel_width, channel_width));
        const auto blue = parse_osc_hex_channel(value.substr(channel_width * 2, channel_width));
        if (!red || !green || !blue) return std::nullopt;
        return EnvironmentRgb{.red = *red, .green = *green, .blue = *blue};
    }

    auto lower = std::string(value.substr(0, std::min<std::size_t>(5, value.size())));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lower.starts_with("rgba:")) {
        value.remove_prefix(5);
    } else if (lower.starts_with("rgb:")) {
        value.remove_prefix(4);
    } else {
        return std::nullopt;
    }
    const auto first_separator = value.find('/');
    if (first_separator == std::string_view::npos) return std::nullopt;
    const auto second_separator = value.find('/', first_separator + 1);
    if (second_separator == std::string_view::npos) return std::nullopt;
    const auto third_separator = value.find('/', second_separator + 1);
    const auto red = parse_osc_hex_channel(value.substr(0, first_separator));
    const auto green = parse_osc_hex_channel(value.substr(
        first_separator + 1,
        second_separator - first_separator - 1));
    const auto blue = parse_osc_hex_channel(value.substr(
        second_separator + 1,
        third_separator == std::string_view::npos
            ? std::string_view::npos
            : third_separator - second_separator - 1));
    if (!red || !green || !blue) return std::nullopt;
    return EnvironmentRgb{.red = *red, .green = *green, .blue = *blue};
}

constexpr std::size_t kAppearanceResponseMaxBytes{4096};
constexpr std::string_view kColorSchemeResponsePrefix{"\x1b[?997;"};
constexpr std::string_view kOscBackgroundResponsePrefix{"\x1b]11;"};

enum class AppearanceDiscardKind {
    None,
    ColorScheme,
    OscBackground,
};

struct AppearanceInputState {
    std::string pending;
    AppearanceDiscardKind discard{AppearanceDiscardKind::None};
    std::optional<TerminalAppearance> color_scheme{std::nullopt};
    std::optional<TerminalAppearance> background{std::nullopt};
};

[[nodiscard]] bool partial_response_prefix(std::string_view input) {
    return kColorSchemeResponsePrefix.starts_with(input) ||
        kOscBackgroundResponsePrefix.starts_with(input);
}

[[nodiscard]] std::optional<std::size_t> osc_terminator(std::string_view input) {
    const auto bell = input.find('\x07');
    const auto string_terminator = input.find("\x1b\\");
    if (bell == std::string_view::npos && string_terminator == std::string_view::npos) {
        return std::nullopt;
    }
    if (bell == std::string_view::npos) return string_terminator;
    if (string_terminator == std::string_view::npos) return bell;
    return std::min(bell, string_terminator);
}

[[nodiscard]] std::string consume_appearance_input(
    AppearanceInputState& state,
    std::string_view input) {
    state.pending.append(input);
    std::string forwarded;
    for (std::size_t index = 0; index < state.pending.size();) {
        auto remaining = std::string_view(state.pending).substr(index);
        if (state.discard == AppearanceDiscardKind::ColorScheme) {
            const auto end = remaining.find('n');
            if (end == std::string_view::npos) {
                state.pending.clear();
                return forwarded;
            }
            state.discard = AppearanceDiscardKind::None;
            index += end + 1;
            continue;
        }
        if (state.discard == AppearanceDiscardKind::OscBackground) {
            const auto end = osc_terminator(remaining);
            if (!end) {
                state.pending.clear();
                return forwarded;
            }
            const bool string_terminated = remaining.substr(*end).starts_with("\x1b\\");
            state.discard = AppearanceDiscardKind::None;
            index += *end + (string_terminated ? 2 : 1);
            continue;
        }
        if (remaining.starts_with(kColorSchemeResponsePrefix)) {
            const auto end = remaining.find('n', kColorSchemeResponsePrefix.size());
            if (end == std::string_view::npos) {
                if (remaining.size() > kAppearanceResponseMaxBytes) {
                    state.discard = AppearanceDiscardKind::ColorScheme;
                    state.pending.clear();
                    return forwarded;
                }
                state.pending.erase(0, index);
                return forwarded;
            }
            const auto value = remaining.substr(
                kColorSchemeResponsePrefix.size(),
                end - kColorSchemeResponsePrefix.size());
            if (value == "1") state.color_scheme = TerminalAppearance::Dark;
            if (value == "2") state.color_scheme = TerminalAppearance::Light;
            index += end + 1;
            continue;
        }
        if (remaining.starts_with(kOscBackgroundResponsePrefix)) {
            const auto body = remaining.substr(kOscBackgroundResponsePrefix.size());
            const auto end = osc_terminator(body);
            if (!end) {
                if (remaining.size() > kAppearanceResponseMaxBytes) {
                    state.discard = AppearanceDiscardKind::OscBackground;
                    state.pending.clear();
                    return forwarded;
                }
                state.pending.erase(0, index);
                return forwarded;
            }
            const auto rgb = parse_osc_background(body.substr(0, *end));
            if (rgb) state.background = appearance_for_rgb(*rgb);
            const bool string_terminated = body.substr(*end).starts_with("\x1b\\");
            index += kOscBackgroundResponsePrefix.size() + *end + (string_terminated ? 2 : 1);
            continue;
        }
        if (remaining.front() == '\x1b' && partial_response_prefix(remaining)) {
            state.pending.erase(0, index);
            return forwarded;
        }
        forwarded.push_back(remaining.front());
        ++index;
    }
    state.pending.clear();
    return forwarded;
}

struct AppearanceProbeResult {
    AppearanceInputState state;
    std::string forwarded_input;
};

[[nodiscard]] AppearanceProbeResult probe_terminal_appearance(int descriptor) {
    constexpr auto kProbeTimeout = std::chrono::milliseconds(100);
    const auto deadline = std::chrono::steady_clock::now() + kProbeTimeout;
    AppearanceProbeResult result;
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break;
        if ((item.revents & POLLIN) == 0) break;
        std::array<char, kAppearanceResponseMaxBytes> buffer{};
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (count == 0) break;
        result.forwarded_input += consume_appearance_input(
            result.state,
            std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
    return result;
}
#endif

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
#if defined(__linux__) || defined(__APPLE__)
    AppearanceInputState startup_appearance;
#endif
    std::jthread worker;
    std::optional<support::Error> worker_error;
    bool keyboard_protocol_pushed{false};
    bool modify_other_keys_active{false};
    std::size_t synchronized_update_depth{0};
    bool progress_active{false};
    std::chrono::steady_clock::time_point progress_next_keepalive{};
    std::uint64_t next_image_handle{1};
    bool draining{false};
    std::chrono::steady_clock::time_point drain_last_activity{};
    CursorPosition cursor{};
    /// Scrolled-out buffer lines (== the first buffer line visible on screen),
    /// mirroring VirtualTerminal's scroll emulation: the renderer writes the
    /// full composed buffer with buffer-relative rows under the main-screen
    /// scrollback flow, and rows at or past the visible viewport bottom advance
    /// this viewport top while `set_cursor` emits pi's CRLF line flow so the
    /// real terminal's native scrollback receives the overflow. `set_cursor`/
    /// `place_image` convert buffer rows to screen rows using it before
    /// emitting ANSI sequences.
    std::size_t viewport_top{0};
#if defined(__linux__) || defined(__APPLE__)
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
#endif
};

namespace {

template <typename T>
[[nodiscard]] support::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Process Terminal must be started before terminal operations"));
}

#if defined(__linux__) || defined(__APPLE__)
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
    try {
        (*sink)(std::move(input));
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
    try {
        (*sink)(dimensions);
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
    // While the ordered queue is non-empty the output is backed up; a direct
    // keepalive write could interleave ahead of queued render bytes and
    // truncate the escape sequence. Skip this beat and retry on the next
    // deadline (the indicator is best-effort and re-emitted).
    if (!impl.output_queue.empty() || impl.output_draining) {
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
    AppearanceInputState appearance;
    std::string keyboard_pending;
    std::string cell_size_pending;
    bool needs_input_flush{false};
    /// Readiness-driven deadlines: armed (now + kNegotiationTimeout) whenever
    /// input leaves partial protocol/appearance bytes or a decoder flush
    /// pending, firing once. They replace the old 10 ms poll counters.
    std::chrono::steady_clock::time_point appearance_deadline{
        std::chrono::steady_clock::time_point::max()};
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
    if (state.appearance_deadline != std::chrono::steady_clock::time_point::max()) {
        if (!earliest || state.appearance_deadline < *earliest) earliest = state.appearance_deadline;
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
void apply_detected_appearance(T& impl, const AppearanceInputState& appearance) {
    const auto detected = appearance.color_scheme
        ? appearance.color_scheme
        : appearance.background;
    if (!detected) return;
    std::lock_guard lock(impl.mutex);
    impl.capabilities.appearance = *detected;
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
    try {
        (*sink)(dimensions);
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
}

template <typename T>
void handle_forwarded_input(
    T& impl,
    WorkerInputState& state,
    std::string input) {
    auto cell_size = detail::consume_cell_size_input(
        std::move(state.cell_size_pending),
        input);
    state.cell_size_pending = std::move(cell_size.pending);
    if (!cell_size.responses.empty()) {
        apply_cell_size_response(impl, cell_size.responses.back());
    }
    auto parsed = detail::parse_keyboard_protocol_input(
        std::move(state.keyboard_pending),
        cell_size.forwarded_input);
    state.keyboard_pending = std::move(parsed.pending);
    state.negotiation_deadline = std::chrono::steady_clock::now() + kNegotiationTimeout;
    for (const auto& response : parsed.responses) apply_keyboard_response(impl, response);
    if (!parsed.forwarded_input.empty()) {
        invoke_input(impl, std::move(parsed.forwarded_input));
        state.needs_input_flush = true;
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
        state.appearance_deadline = std::chrono::steady_clock::now() + kNegotiationTimeout;
        auto forwarded = consume_appearance_input(state.appearance, event.input);
        apply_detected_appearance(impl, state.appearance);
        if (!forwarded.empty()) handle_forwarded_input(impl, state, std::move(forwarded));
        return true;
    }

    // A timeout wake means one armed deadline passed; fire the ones that are
    // due. Both deadlines are armed on input and fire once, mirroring the old
    // 15-poll (150 ms) idle windows.
    if (deadline_fired(state.appearance_deadline)) {
        const bool terminal_response_pending =
            state.appearance.discard != AppearanceDiscardKind::None ||
            state.appearance.pending.starts_with(kColorSchemeResponsePrefix) ||
            state.appearance.pending.starts_with(kOscBackgroundResponsePrefix);
        if (terminal_response_pending) {
            state.appearance.pending.clear();
            state.appearance.discard = AppearanceDiscardKind::None;
        } else if (!state.appearance.pending.empty()) {
            handle_forwarded_input(impl, state, std::move(state.appearance.pending));
            state.appearance.pending.clear();
        }
        state.appearance_deadline = std::chrono::steady_clock::time_point::max();
    }
    if (deadline_fired(state.negotiation_deadline)) {
        if (!state.keyboard_pending.empty()) {
            invoke_input(impl, std::move(state.keyboard_pending));
            state.keyboard_pending.clear();
            state.needs_input_flush = true;
        }
        if (!state.cell_size_pending.empty()) {
            invoke_input(impl, std::move(state.cell_size_pending));
            state.cell_size_pending.clear();
            state.needs_input_flush = true;
        }
        if (state.needs_input_flush && state.keyboard_pending.empty()) {
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
        input_state.appearance = std::move(impl.startup_appearance);
    }
    if (!startup_input.empty()) {
        handle_forwarded_input(impl, input_state, std::move(startup_input));
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
#endif

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
#if !defined(__linux__) && !defined(__APPLE__)
    (void)input_sink;
    (void)resize_sink;
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Process Terminal is unsupported on this platform",
        "supported platforms are Linux and macOS"));
#else
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
    impl_->startup_appearance = {};
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
    auto appearance = probe_terminal_appearance(impl_->options.input_fd);
    impl_->capabilities.appearance = appearance.state.color_scheme.value_or(
        appearance.state.background.value_or(environment_appearance));
    impl_->startup_appearance = std::move(appearance.state);
    impl_->startup_input = std::move(appearance.forwarded_input);

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
    impl_->last_resize_check = std::chrono::steady_clock::now();

    impl_->input_sink = std::move(owned_input_sink);
    impl_->resize_sink = std::move(owned_resize_sink);
    impl_->dimensions = *dimensions;
    impl_->modes.started = true;
    try {
        // Impl outlives this borrowed pointer: destruction cannot occur from a sink,
        // and destructor-driven stop joins the delivery worker.
        auto* worker_impl = impl_.get();
        impl_->worker = std::jthread([worker_impl](std::stop_token stop_token) {
            run_terminal_worker(*worker_impl, stop_token);
        });
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
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::stop() {
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
#if !defined(__linux__) && !defined(__APPLE__)
    return {};
#else
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
        retain_error(result, restore_terminal_modes(*impl_));
    }

    lifecycle_lock.lock();
    impl_->stop_in_progress = false;
    lifecycle_lock.unlock();
    impl_->lifecycle_cv.notify_all();
    return result;
#endif
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
    // scroll history and the mirrored viewport top both reset.
    impl_->viewport_top = 0;
#if defined(__linux__) || defined(__APPLE__)
    return enqueue_output(*impl_, kClearScreen);
#else
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::write(std::string_view output) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
    return enqueue_output(*impl_, output);
#else
    return {};
#endif
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
    // scroll history natively. The emulated viewport top stays in sync with
    // the renderer's own tracked viewport.
    std::string sequence;
    if (position.row >= impl_->viewport_top + impl_->dimensions.rows) {
        const auto scroll = position.row - (impl_->viewport_top + impl_->dimensions.rows - 1);
        const auto bottom = impl_->dimensions.rows - 1;
        if (impl_->cursor.row < bottom) {
            sequence += std::format("\x1b[{}B", bottom - impl_->cursor.row);
        }
        sequence.append(scroll, '\r');
        sequence.append(scroll, '\n');
        impl_->viewport_top += scroll;
        impl_->cursor.row = bottom;
        impl_->cursor.column = position.column;
#if defined(__linux__) || defined(__APPLE__)
        return enqueue_output(*impl_, sequence);
#else
        return {};
#endif
    }
    if (position.row < impl_->viewport_top) {
        impl_->cursor.row = 0;
    } else {
        impl_->cursor.row = position.row - impl_->viewport_top;
    }
    impl_->cursor.column = position.column;
    sequence += std::format("\x1b[{};{}H", impl_->cursor.row + 1, position.column + 1);
#if defined(__linux__) || defined(__APPLE__)
    return enqueue_output(*impl_, sequence);
#else
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::set_cursor_visible(bool visible) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.cursor_visible == visible) return {};
#if defined(__linux__) || defined(__APPLE__)
    auto result = enqueue_output(*impl_, visible ? kCursorShow : kCursorHide);
    if (result) impl_->modes.cursor_visible = visible;
    return result;
#else
    impl_->modes.cursor_visible = visible;
    return {};
#endif
}

support::Expected<TerminalImageHandle> ProcessTerminal::place_image(const TerminalImage& image) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
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
    // scrollback (row < viewport top) cannot be addressed by the real terminal
    // and is already held by its scroll history; skip the placement but keep
    // the handle allocation so the renderer's active-image tracking stays
    // consistent (fork-B image-follows-content, ADR 0037).
    if (image.region.row < impl_->viewport_top) {
        if (!image.preferred_handle) ++impl_->next_image_handle;
        return handle;
    }
    const auto screen_row = image.region.row - impl_->viewport_top;
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
#else
    (void)image;
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Process Terminal does not support inline images"));
#endif
}

support::ExpectedVoid ProcessTerminal::remove_image(
    TerminalImageHandle handle,
    const CellRegion& region) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
    if (region.column >= impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image removal region is outside terminal dimensions"));
    }
    // Buffer-absolute removal region (fork-B): regions that scrolled into the
    // native scrollback are already out of the visible screen, so there is
    // nothing to blank in place; skip them.
    if (region.row >= impl_->viewport_top) {
        const auto screen_row = region.row - impl_->viewport_top;
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
#else
    (void)handle;
    (void)region;
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::begin_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth > 0) {
        ++impl_->synchronized_update_depth;
        return {};
    }
#if defined(__linux__) || defined(__APPLE__)
    auto result = enqueue_output(*impl_, kBeginSynchronizedUpdate);
    if (result) impl_->synchronized_update_depth = 1;
    return result;
#else
    impl_->synchronized_update_depth = 1;
    return {};
#endif
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
#if defined(__linux__) || defined(__APPLE__)
    auto ended = enqueue_output(*impl_, kEndSynchronizedUpdate);
    if (ended) impl_->synchronized_update_depth = 0;
    return ended;
#else
    impl_->synchronized_update_depth = 0;
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::set_title(std::string_view title) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
    return enqueue_output(*impl_, std::format("\x1b]0;{}\x07", title));
#else
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::set_progress(bool active) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
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
#else
    impl_->progress_active = active;
    return {};
#endif
}

support::ExpectedVoid ProcessTerminal::drain_input(
    std::chrono::milliseconds max_ms,
    std::chrono::milliseconds idle_ms) {
    std::unique_lock lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if !defined(__linux__) && !defined(__APPLE__)
    (void)max_ms;
    (void)idle_ms;
    return {};
#else
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
#endif
}

} // namespace cch::tui
