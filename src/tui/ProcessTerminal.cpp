#include <cch/tui/ProcessTerminal.hpp>

#include "KeyboardProtocol.hpp"

#include <array>
#include <cerrno>
#include <condition_variable>
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

[[nodiscard]] bool supports_synchronized_output() {
    const auto* terminal_value = std::getenv("TERM");
    const auto* program_value = std::getenv("TERM_PROGRAM");
    const std::string_view terminal = terminal_value == nullptr ? "" : terminal_value;
    const std::string_view program = program_value == nullptr ? "" : program_value;
    return terminal == "xterm-kitty" || terminal == "alacritty" ||
        terminal == "foot" || terminal == "foot-extra" || terminal == "wezterm" ||
        terminal == "ghostty" || program == "iTerm.app" || program == "WezTerm" ||
        program == "ghostty";
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
    std::string keyboard_pending;
    bool needs_input_flush{false};
    int negotiation_idle_polls{0};
};

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
        auto parsed = detail::parse_keyboard_protocol_input(
            std::move(state.keyboard_pending),
            event.input);
        state.keyboard_pending = std::move(parsed.pending);
        state.negotiation_idle_polls = 0;
        for (const auto& response : parsed.responses) apply_keyboard_response(impl, response);
        if (!parsed.forwarded_input.empty()) {
            invoke_input(impl, std::move(parsed.forwarded_input));
            state.needs_input_flush = true;
        }
        return true;
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
    impl_->capabilities = TerminalCapabilities{
        .synchronized_output = supports_synchronized_output(),
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
