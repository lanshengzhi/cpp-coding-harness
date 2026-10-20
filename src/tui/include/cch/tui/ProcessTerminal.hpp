#pragma once

#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <any>
#include <memory>
#include <string>

namespace cch::tui {

/// Non-owning descriptors used by ProcessTerminal. Both descriptors must remain
/// open and refer to terminals until stop() returns.
struct ProcessTerminalOptions {
    int input_fd{0};
    int output_fd{1};
    /// Optional Boost.Asio executor stored by value. Its execution context must
    /// remain alive and running until stop() returns after asynchronous I/O is attached.
    std::any executor{};
};

class ProcessTerminal;

namespace detail::testing {

/// Whether the terminal is holding an incomplete input sequence fragment in
/// either input stage. Test-only seam: the restart regression (#613) must know
/// the stale fragment has been read before the session is stopped, otherwise
/// the bytes are still in the descriptor and the next session reads them as
/// fresh input.
[[nodiscard]] bool input_holds_fragment(const ProcessTerminal& terminal) noexcept;

} // namespace detail::testing

/// Linux terminal adapter for the reusable TUI package.
/// Input and resize sinks run on the event loop when an executor is configured,
/// or via non-blocking polling. The adapter must not be destroyed from a sink.
class ProcessTerminal final : public Terminal {
public:
    explicit ProcessTerminal(ProcessTerminalOptions options = {});
    ProcessTerminal(ProcessTerminal&&) = delete;
    ProcessTerminal& operator=(ProcessTerminal&&) = delete;
    ~ProcessTerminal() override;

    ProcessTerminal(const ProcessTerminal&) = delete;
    ProcessTerminal& operator=(const ProcessTerminal&) = delete;

    using Terminal::stop;
    [[nodiscard]] support::ExpectedVoid start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) override;
    [[nodiscard]] support::ExpectedVoid stop() override;
    [[nodiscard]] support::ExpectedVoid stop(TerminalStopOptions options) override;
    [[nodiscard]] TerminalDimensions dimensions() const override;
    [[nodiscard]] TerminalCapabilities capabilities() const override;
    [[nodiscard]] TerminalModeState modes() const override;
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    [[nodiscard]] support::ExpectedVoid write(std::string_view output) override;
    [[nodiscard]] support::ExpectedVoid set_cursor(CursorPosition position) override;
    [[nodiscard]] support::ExpectedVoid set_cursor_visible(bool visible) override;
    [[nodiscard]] support::ExpectedVoid set_alternate_screen(bool enabled) override;
    [[nodiscard]] support::ExpectedVoid set_autowrap(bool enabled) override;
    [[nodiscard]] support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override;
    [[nodiscard]] support::ExpectedVoid reset_scroll_margins() override;
    [[nodiscard]] support::ExpectedVoid begin_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid end_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid set_title(std::string_view title) override;
    /// The downstream cell-size consumer: the `CSI 16 t` reply is forwarded to
    /// the input sink verbatim so the host's raw-input listener stage observes
    /// it first (#952), and this call refines the reported cell pixels.
    [[nodiscard]] support::ExpectedVoid apply_cell_pixel_dimensions(CellPixelDimensions pixels) override;
    [[nodiscard]] support::ExpectedVoid set_progress(bool active) override;
    /// Report the latest OSC 7501 status. The status is stored across stop()
    /// and only written to a terminal that confirmed support (or was forced by
    /// `PI_PROGRAM_STATUS=1`); stop() clears the reported status.
    [[nodiscard]] support::ExpectedVoid set_program_status(const ProgramStatus& status) override;
    [[nodiscard]] support::ExpectedVoid drain_input(std::chrono::milliseconds max_ms = kDrainInputMaxMs,
            std::chrono::milliseconds idle_ms = kDrainInputIdleMs) override;
    [[nodiscard]] support::ExpectedVoid enter_output_only() override;
    [[nodiscard]] support::ExpectedVoid leave_output_only() override;

    /// The diagnostic write-log path resolved from `PI_TUI_WRITE_LOG` at
    /// construction (pi `ProcessTerminal.writeLogPath`): empty disables logging,
    /// an existing directory selects `<dir>/tui-<timestamp>-<pid>.log`, and
    /// anything else is used as the literal file path. Every committed write is
    /// appended to that file; log failures are ignored.
    [[nodiscard]] const std::string& write_log_path() const noexcept;

private:
    friend bool detail::testing::input_holds_fragment(const ProcessTerminal& terminal) noexcept;

    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
