#pragma once

#include <cch/tui/Terminal.hpp>

#include <memory>

namespace cch::tui {

/// Non-owning descriptors used by ProcessTerminal. Both descriptors must remain
/// open and refer to terminals until stop() returns.
struct ProcessTerminalOptions {
    int input_fd{0};
    int output_fd{1};
};

/// Linux/macOS terminal adapter for the reusable TUI package.
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

    [[nodiscard]] util::ExpectedVoid start(
        TerminalInputSink input_sink,
        TerminalResizeSink resize_sink) override;
    [[nodiscard]] util::ExpectedVoid stop() override;
    [[nodiscard]] TerminalDimensions dimensions() const override;
    [[nodiscard]] TerminalCapabilities capabilities() const override;
    [[nodiscard]] TerminalModeState modes() const override;
    [[nodiscard]] util::ExpectedVoid clear_screen() override;
    [[nodiscard]] util::ExpectedVoid write(std::string_view output) override;
    [[nodiscard]] util::ExpectedVoid set_cursor(CursorPosition position) override;
    [[nodiscard]] util::ExpectedVoid set_cursor_visible(bool visible) override;
    [[nodiscard]] util::Expected<TerminalImageHandle> place_image(const TerminalImage& image) override;
    [[nodiscard]] util::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) override;
    [[nodiscard]] util::ExpectedVoid begin_synchronized_update() override;
    [[nodiscard]] util::ExpectedVoid end_synchronized_update() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
