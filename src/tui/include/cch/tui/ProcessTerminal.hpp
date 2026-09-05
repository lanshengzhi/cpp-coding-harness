#pragma once

#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <chrono>
#include <boost/asio/any_io_executor.hpp>
#include <optional>
#include <memory>

namespace cch::tui {

/// Non-owning descriptors used by ProcessTerminal. Both descriptors must remain
/// open and refer to terminals until stop() returns.
struct ProcessTerminalOptions {
    int input_fd{0};
    int output_fd{1};
    std::optional<boost::asio::any_io_executor> executor{std::nullopt};
};

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

    [[nodiscard]] support::ExpectedVoid start(
        TerminalInputSink input_sink,
        TerminalResizeSink resize_sink) override;
    [[nodiscard]] support::ExpectedVoid stop() override;
    [[nodiscard]] TerminalDimensions dimensions() const override;
    [[nodiscard]] TerminalCapabilities capabilities() const override;
    [[nodiscard]] TerminalModeState modes() const override;
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    [[nodiscard]] support::ExpectedVoid write(std::string_view output) override;
    [[nodiscard]] support::ExpectedVoid set_cursor(CursorPosition position) override;
    [[nodiscard]] support::ExpectedVoid set_cursor_visible(bool visible) override;
    [[nodiscard]] support::ExpectedVoid set_scroll_margins(
        std::size_t top_row,
        std::size_t bottom_row) override;
    [[nodiscard]] support::ExpectedVoid reset_scroll_margins() override;
    [[nodiscard]] support::ExpectedVoid set_dock_cursor(
        std::size_t dock_row,
        std::size_t column) override;
    [[nodiscard]] support::Expected<TerminalImageHandle> place_image(const TerminalImage& image) override;
    [[nodiscard]] support::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) override;
    [[nodiscard]] support::ExpectedVoid begin_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid end_synchronized_update() override;
    [[nodiscard]] support::ExpectedVoid set_title(std::string_view title) override;
    [[nodiscard]] support::ExpectedVoid set_progress(bool active) override;
    [[nodiscard]] support::ExpectedVoid drain_input(
        std::chrono::milliseconds max_ms = kDrainInputMaxMs,
        std::chrono::milliseconds idle_ms = kDrainInputIdleMs) override;
    [[nodiscard]] support::ExpectedVoid poll_input();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
