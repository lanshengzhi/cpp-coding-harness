#pragma once

#include <cch/tui/Terminal.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace cch::tui {

struct VirtualTerminalOptions {
    std::size_t columns{80};
    std::size_t rows{24};
};

struct VirtualTerminalStyle {
    bool bold{false};
    bool dim{false};
    bool italic{false};
    bool underline{false};
    bool blink{false};
    bool inverse{false};
    bool hidden{false};
    bool strikethrough{false};
    std::string fg_color;
    std::string bg_color;
    std::string hyperlink;
    std::string hyperlink_params;

    bool operator==(const VirtualTerminalStyle&) const = default;
};

struct VirtualTerminalCell {
    std::string grapheme;
    bool continuation{false};
    VirtualTerminalStyle style;

    bool operator==(const VirtualTerminalCell&) const = default;
};

class VirtualTerminal final : public Terminal {
public:
    explicit VirtualTerminal(VirtualTerminalOptions options = {});
    VirtualTerminal(VirtualTerminal&&) noexcept;
    VirtualTerminal& operator=(VirtualTerminal&&) noexcept;
    ~VirtualTerminal() override;

    VirtualTerminal(const VirtualTerminal&) = delete;
    VirtualTerminal& operator=(const VirtualTerminal&) = delete;

    [[nodiscard]] util::ExpectedVoid start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) override;
    [[nodiscard]] util::ExpectedVoid stop() override;
    [[nodiscard]] TerminalDimensions dimensions() const override;
    [[nodiscard]] TerminalCapabilities capabilities() const override;
    [[nodiscard]] TerminalModeState modes() const override;
    [[nodiscard]] util::ExpectedVoid clear_screen() override;
    [[nodiscard]] util::ExpectedVoid write(std::string_view output) override;
    [[nodiscard]] util::ExpectedVoid set_cursor(CursorPosition position) override;
    [[nodiscard]] util::ExpectedVoid set_cursor_visible(bool visible) override;
    [[nodiscard]] util::ExpectedVoid begin_synchronized_update() override;
    [[nodiscard]] util::ExpectedVoid end_synchronized_update() override;

    [[nodiscard]] util::ExpectedVoid inject_input(std::string input);
    [[nodiscard]] util::ExpectedVoid flush_input();
    [[nodiscard]] util::ExpectedVoid inject_resize(TerminalDimensions dimensions);
    [[nodiscard]] const std::vector<std::string>& output() const;
    [[nodiscard]] const std::vector<std::string>& screen() const;
    [[nodiscard]] const std::vector<std::vector<VirtualTerminalCell>>& cells() const;
    [[nodiscard]] VirtualTerminalStyle final_style() const;
    [[nodiscard]] CursorPosition cursor() const;
    /// Returns true if clear_screen() was called since the last check.
    /// Resets the flag on read.
    [[nodiscard]] bool check_clear_screen_called();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
