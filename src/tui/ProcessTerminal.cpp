#include <cch/tui/ProcessTerminal.hpp>

#include "KeyboardProtocol.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
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
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cch::tui {
namespace {

constexpr std::string_view kBracketedPasteEnable = "\x1b[?2004h";
constexpr std::string_view kBracketedPasteDisable = "\x1b[?2004l";
constexpr std::string_view kCursorShow = "\x1b[?25h";
constexpr std::string_view kCursorHide = "\x1b[?25l";
constexpr std::string_view kClearScreen = "\x1b[2J\x1b[H";
constexpr std::string_view kBeginSynchronizedUpdate = "\x1b[?2026h";
constexpr std::string_view kEndSynchronizedUpdate = "\x1b[?2026l";
constexpr std::string_view kKeyboardProtocolPush = "\x1b[>7u";
constexpr std::string_view kKeyboardProtocolQuery = "\x1b[?u\x1b[c";
constexpr std::string_view kKeyboardProtocolPop = "\x1b[<u";
constexpr std::string_view kColorSchemeQuery = "\x1b[?996n";
constexpr std::string_view kBackgroundColorQuery = "\x1b]11;?\x07";
constexpr std::string_view kModifyOtherKeysEnable = "\x1b[>4;2m";
constexpr std::string_view kModifyOtherKeysDisable = "\x1b[>4;0m";

[[nodiscard]] util::Error process_error(std::string message, std::string_view operation, int error_number) {
    return util::make_error(
        util::ErrorCode::Process,
        std::move(message),
        std::format("{} failed (errno {})", operation, error_number));
}

#if defined(__linux__) || defined(__APPLE__)
struct WriteAttempt {
    util::ExpectedVoid result;
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

[[nodiscard]] util::ExpectedVoid write_all(int descriptor, std::string_view output) {
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
    std::optional<util::Error> worker_error;
    bool keyboard_protocol_pushed{false};
    bool modify_other_keys_active{false};
    std::size_t synchronized_update_depth{0};
#if defined(__linux__) || defined(__APPLE__)
    termios original_termios{};
    bool has_original_termios{false};
#endif
};

namespace {

template <typename T>
[[nodiscard]] util::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Process Terminal must be started before terminal operations"));
}

#if defined(__linux__) || defined(__APPLE__)
[[nodiscard]] util::Expected<TerminalDimensions> read_dimensions(int descriptor) {
    winsize size{};
    if (::ioctl(descriptor, TIOCGWINSZ, &size) != 0) {
        return std::unexpected(process_error(
            "Process Terminal could not read terminal dimensions",
            "ioctl(TIOCGWINSZ)",
            errno));
    }
    if (size.ws_col == 0 || size.ws_row == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Process Terminal requires positive terminal dimensions"));
    }
    return TerminalDimensions{
        .columns = size.ws_col,
        .rows = size.ws_row,
    };
}

[[nodiscard]] std::string describe_error(const util::Error& error) {
    if (error.detail.empty()) return error.message;
    return std::format("{} [{}]", error.message, error.detail);
}

[[nodiscard]] util::Error combine_errors(
    util::Error primary,
    const util::Error& secondary,
    std::string message) {
    return util::make_error(
        primary.code,
        std::move(message),
        std::format(
            "primary: {}; secondary: {}",
            describe_error(primary),
            describe_error(secondary)));
}

void retain_error(util::ExpectedVoid& accumulated, util::ExpectedVoid candidate) {
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

[[nodiscard]] util::Error startup_failure(
    util::Error acquisition_error,
    const util::ExpectedVoid& rollback) {
    if (rollback) return acquisition_error;
    return combine_errors(
        std::move(acquisition_error),
        rollback.error(),
        "Process Terminal startup failed and rollback was incomplete");
}

template <typename T>
void record_worker_error(T& impl, util::Error error) {
    std::lock_guard lock(impl.mutex);
    if (!impl.worker_error) impl.worker_error = std::move(error);
}

template <typename T>
void invoke_input(T& impl, std::string input) {
    std::shared_ptr<TerminalInputSink> sink;
    {
        std::lock_guard lock(impl.mutex);
        sink = impl.input_sink;
    }
    if (!sink || !*sink) return;
    try {
        (*sink)(std::move(input));
    } catch (const std::exception&) {
        record_worker_error(impl, util::make_error(
            util::ErrorCode::Unknown,
            "Process Terminal input sink failed",
            "the input callback threw an exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.input_sink == sink) impl.input_sink.reset();
    } catch (...) {
        record_worker_error(impl, util::make_error(
            util::ErrorCode::Unknown,
            "Process Terminal input sink failed",
            "the input callback threw an unknown exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.input_sink == sink) impl.input_sink.reset();
    }
}

template <typename T>
void deliver_resize_if_changed(T& impl) {
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
        record_worker_error(impl, util::make_error(
            util::ErrorCode::Unknown,
            "Process Terminal resize sink failed",
            "the resize callback threw an exception"));
        std::lock_guard lock(impl.mutex);
        if (impl.resize_sink == sink) impl.resize_sink.reset();
    } catch (...) {
        record_worker_error(impl, util::make_error(
            util::ErrorCode::Unknown,
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

enum class WorkerEventKind {
    Timeout,
    Input,
    Closed,
    Error,
};

struct WorkerEvent {
    WorkerEventKind kind{WorkerEventKind::Timeout};
    std::string input;
    std::optional<util::Error> error{std::nullopt};
};

[[nodiscard]] WorkerEvent wait_for_terminal_input(int descriptor) {
    constexpr int kPollMilliseconds = 10;
    pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
    const auto ready = ::poll(&item, 1, kPollMilliseconds);
    if (ready < 0) {
        if (errno == EINTR) {
            return WorkerEvent{
                .kind = WorkerEventKind::Timeout,
                .input = {},
                .error = std::nullopt,
            };
        }
        return WorkerEvent{
            .kind = WorkerEventKind::Error,
            .input = {},
            .error = process_error("Process Terminal input polling failed", "poll", errno),
        };
    }
    if (ready == 0) {
        return WorkerEvent{
            .kind = WorkerEventKind::Timeout,
            .input = {},
            .error = std::nullopt,
        };
    }
    if ((item.revents & POLLIN) != 0) {
        std::array<char, 4096> buffer{};
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            return WorkerEvent{
                .kind = WorkerEventKind::Input,
                .input = std::string(buffer.data(), static_cast<std::size_t>(count)),
                .error = std::nullopt,
            };
        }
        if (count < 0 && errno == EINTR) {
            return WorkerEvent{
                .kind = WorkerEventKind::Timeout,
                .input = {},
                .error = std::nullopt,
            };
        }
        if (count < 0) {
            return WorkerEvent{
                .kind = WorkerEventKind::Error,
                .input = {},
                .error = process_error("Process Terminal input read failed", "read", errno),
            };
        }
    }
    return WorkerEvent{
        .kind = WorkerEventKind::Closed,
        .input = {},
        .error = std::nullopt,
    };
}

struct WorkerInputState {
    AppearanceInputState appearance;
    std::string keyboard_pending;
    bool needs_input_flush{false};
    int appearance_idle_polls{0};
    int negotiation_idle_polls{0};
};

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
void handle_forwarded_input(
    T& impl,
    WorkerInputState& state,
    std::string input) {
    auto parsed = detail::parse_keyboard_protocol_input(
        std::move(state.keyboard_pending),
        input);
    state.keyboard_pending = std::move(parsed.pending);
    state.negotiation_idle_polls = 0;
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
    constexpr int kNegotiationTimeoutPolls = 15;
    if (event.kind == WorkerEventKind::Closed) return false;
    if (event.kind == WorkerEventKind::Error) {
        record_worker_error(impl, std::move(*event.error));
        return false;
    }
    if (event.kind == WorkerEventKind::Input) {
        state.appearance_idle_polls = 0;
        auto forwarded = consume_appearance_input(state.appearance, event.input);
        apply_detected_appearance(impl, state.appearance);
        if (!forwarded.empty()) handle_forwarded_input(impl, state, std::move(forwarded));
        return true;
    }

    ++state.appearance_idle_polls;
    if (state.appearance_idle_polls >= kNegotiationTimeoutPolls) {
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
        state.appearance_idle_polls = 0;
    }
    ++state.negotiation_idle_polls;
    if (!state.keyboard_pending.empty() &&
        state.negotiation_idle_polls >= kNegotiationTimeoutPolls) {
        invoke_input(impl, std::move(state.keyboard_pending));
        state.keyboard_pending.clear();
        state.needs_input_flush = true;
    }
    if (state.needs_input_flush && state.keyboard_pending.empty()) {
        invoke_input(impl, {});
        state.needs_input_flush = false;
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
    while (!stop_token.stop_requested()) {
        if (!handle_worker_event(
                impl,
                input_state,
                wait_for_terminal_input(impl.options.input_fd))) {
            return;
        }
        if (!stop_token.stop_requested()) deliver_resize_if_changed(impl);
    }
}

template <typename T>
[[nodiscard]] util::ExpectedVoid restore_terminal_modes(T& impl) {
    util::ExpectedVoid first_error;

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

util::ExpectedVoid ProcessTerminal::start(
    TerminalInputSink input_sink,
    TerminalResizeSink resize_sink) {
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
    impl_->lifecycle_cv.wait(lifecycle_lock, [this] { return !impl_->stop_in_progress; });
    std::lock_guard lock(impl_->mutex);
    if (impl_->modes.started) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Process Terminal is already started"));
    }
    if (impl_->worker.joinable()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Process Terminal delivery worker is still stopping"));
    }
#if !defined(__linux__) && !defined(__APPLE__)
    (void)input_sink;
    (void)resize_sink;
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Process Terminal is unsupported on this platform",
        "supported platforms are Linux and macOS"));
#else
    if (impl_->has_original_termios || impl_->modes.bracketed_paste ||
        !impl_->modes.cursor_visible || impl_->keyboard_protocol_pushed ||
        impl_->modify_other_keys_active || impl_->synchronized_update_depth > 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Process,
            "Process Terminal has unrestored state from a previous stop"));
    }
    if (::isatty(impl_->options.input_fd) != 1 || ::isatty(impl_->options.output_fd) != 1) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
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
    impl_->capabilities = TerminalCapabilities{
        .synchronized_output = supports_synchronized_output(),
        .color = detect_color_capability(),
        .appearance = environment_appearance,
    };
    auto owned_input_sink = std::make_shared<TerminalInputSink>(std::move(input_sink));
    auto owned_resize_sink = std::make_shared<TerminalResizeSink>(std::move(resize_sink));

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
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(startup_failure(paste_attempt.result.error(), rollback));
    }
    impl_->modes.bracketed_paste = true;

    auto appearance_query = attempt_write_all(
        impl_->options.output_fd,
        std::string(kColorSchemeQuery) + std::string(kBackgroundColorQuery));
    if (!appearance_query.result) {
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(startup_failure(appearance_query.result.error(), rollback));
    }
    auto appearance = probe_terminal_appearance(impl_->options.input_fd);
    impl_->capabilities.appearance = appearance.state.color_scheme.value_or(
        appearance.state.background.value_or(environment_appearance));
    impl_->startup_appearance = std::move(appearance.state);
    impl_->startup_input = std::move(appearance.forwarded_input);

    auto keyboard_push = attempt_write_all(impl_->options.output_fd, kKeyboardProtocolPush);
    if (!keyboard_push.result) {
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(startup_failure(keyboard_push.result.error(), rollback));
    }
    impl_->keyboard_protocol_pushed = true;

    auto keyboard_query = attempt_write_all(impl_->options.output_fd, kKeyboardProtocolQuery);
    if (!keyboard_query.result) {
        auto rollback = restore_terminal_modes(*impl_);
        return std::unexpected(startup_failure(keyboard_query.result.error(), rollback));
    }

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
        auto rollback = restore_terminal_modes(*impl_);
        auto acquisition_error = util::make_error(
            util::ErrorCode::Process,
            "Process Terminal could not start input and resize delivery",
            std::format("thread creation failed (code {})", error.code().value()));
        return std::unexpected(startup_failure(std::move(acquisition_error), rollback));
    }
    return {};
#endif
}

util::ExpectedVoid ProcessTerminal::stop() {
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
    }
    if (impl_->worker.joinable() && !called_from_worker) impl_->worker.join();

    util::ExpectedVoid result;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->worker_error) {
            result = std::unexpected(std::move(*impl_->worker_error));
            impl_->worker_error.reset();
        }
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

util::ExpectedVoid ProcessTerminal::clear_screen() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
    return write_all(impl_->options.output_fd, kClearScreen);
#else
    return {};
#endif
}

util::ExpectedVoid ProcessTerminal::write(std::string_view output) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
#if defined(__linux__) || defined(__APPLE__)
    return write_all(impl_->options.output_fd, output);
#else
    return {};
#endif
}

util::ExpectedVoid ProcessTerminal::set_cursor(CursorPosition position) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (position.row >= impl_->dimensions.rows || position.column > impl_->dimensions.columns) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Process Terminal cursor position is outside its dimensions"));
    }
#if defined(__linux__) || defined(__APPLE__)
    return write_all(
        impl_->options.output_fd,
        std::format("\x1b[{};{}H", position.row + 1, position.column + 1));
#else
    return {};
#endif
}

util::ExpectedVoid ProcessTerminal::set_cursor_visible(bool visible) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->modes.cursor_visible == visible) return {};
#if defined(__linux__) || defined(__APPLE__)
    auto attempt = attempt_write_all(
        impl_->options.output_fd,
        visible ? kCursorShow : kCursorHide);
    if (attempt.result) impl_->modes.cursor_visible = visible;
    return attempt.result;
#else
    impl_->modes.cursor_visible = visible;
    return {};
#endif
}

util::Expected<TerminalImageHandle> ProcessTerminal::place_image(const TerminalImage&) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Process Terminal does not support inline images"));
}

util::ExpectedVoid ProcessTerminal::remove_image(TerminalImageHandle, const CellRegion&) {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    return {};
}

util::ExpectedVoid ProcessTerminal::begin_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth > 0) {
        ++impl_->synchronized_update_depth;
        return {};
    }
#if defined(__linux__) || defined(__APPLE__)
    auto attempt = attempt_write_all(impl_->options.output_fd, kBeginSynchronizedUpdate);
    if (attempt.result) impl_->synchronized_update_depth = 1;
    return attempt.result;
#else
    impl_->synchronized_update_depth = 1;
    return {};
#endif
}

util::ExpectedVoid ProcessTerminal::end_synchronized_update() {
    std::lock_guard lock(impl_->mutex);
    if (auto started = require_started(*impl_); !started) return std::unexpected(started.error());
    if (impl_->synchronized_update_depth == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Process Terminal synchronized update is not active"));
    }
    if (impl_->synchronized_update_depth > 1) {
        --impl_->synchronized_update_depth;
        return {};
    }
#if defined(__linux__) || defined(__APPLE__)
    auto ended = write_all(impl_->options.output_fd, kEndSynchronizedUpdate);
    if (ended) impl_->synchronized_update_depth = 0;
    return ended;
#else
    impl_->synchronized_update_depth = 0;
    return {};
#endif
}

} // namespace cch::tui
