#pragma once

#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <chrono>
#include <memory>

namespace cch::tui {

/// Non-owning descriptors used by ProcessTerminal. Both descriptors must remain
/// open and refer to terminals until stop() returns.
struct ProcessTerminalOptions {
    int input_fd{0};
    int output_fd{1};
};

/// Linux terminal adapter for the reusable TUI package.
/// Input and resize sinks run serially on an adapter-owned worker. A sink may
/// request stop without self-joining; a concurrent external stop restores modes
/// after the active sink quiesces. The adapter must not be destroyed from a sink.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
