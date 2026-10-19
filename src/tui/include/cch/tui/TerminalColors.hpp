#pragma once

#include <cch/tui/Utils.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// Terminal color-scheme modes reported by modern terminals (mode 2031 / CSI ? 997).
enum class TerminalColorScheme {
    Dark,
    Light,
};

/// Colors reported by the terminal for its foreground, background, and palette.
struct TerminalColors {
    /// Default foreground (OSC 10).
    std::optional<RgbColor> foreground{std::nullopt};
    /// Default background (OSC 11).
    std::optional<RgbColor> background{std::nullopt};
    /// ANSI colors 0-15 (OSC 4). Set only when all 16 distinct palette targets replied.
    std::optional<std::vector<RgbColor>> palette{std::nullopt};

    bool operator==(const TerminalColors&) const = default;
};

using TerminalColorsCallback = std::move_only_function<void(TerminalColors)>;

/// Configuration options for querying terminal colors via ITui::query_terminal_colors.
struct TerminalColorsQueryOptions {
    /// Maximum time to wait for complete query replies before resolving with partial results.
    std::chrono::milliseconds timeout{100};
    /// Callback invoked when the query resolves (at timeout with partial results or upon completion).
    TerminalColorsCallback on_complete{};
    /// Callback invoked when replies arrive after the query timed out.
    TerminalColorsCallback on_late_reply{};
};

/// Copyable because scheme dispatch snapshots observers under the registry
/// lock, then invokes them after releasing it.
using TerminalColorSchemeListener = std::function<void(TerminalColorScheme)>;
using TerminalColorSchemeListenerId = std::uint64_t;

/// RAII handle managing a terminal color-scheme listener registration.
class TerminalColorSchemeListenerHandle final {
public:
    TerminalColorSchemeListenerHandle() noexcept = default;
    explicit TerminalColorSchemeListenerHandle(std::move_only_function<void()> unsubscribe) noexcept
        : unsubscribe_(std::move(unsubscribe)) {}
    ~TerminalColorSchemeListenerHandle() { reset(); }

    TerminalColorSchemeListenerHandle(TerminalColorSchemeListenerHandle&& other) noexcept = default;
    TerminalColorSchemeListenerHandle& operator=(TerminalColorSchemeListenerHandle&& other) noexcept {
        if (this != &other) {
            reset();
            unsubscribe_ = std::move(other.unsubscribe_);
        }
        return *this;
    }

    TerminalColorSchemeListenerHandle(const TerminalColorSchemeListenerHandle&) = delete;
    TerminalColorSchemeListenerHandle& operator=(const TerminalColorSchemeListenerHandle&) = delete;

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(unsubscribe_); }

    void reset() {
        if (unsubscribe_) {
            auto fn = std::move(unsubscribe_);
            fn();
        }
    }

    void operator()() { reset(); }

private:
    std::move_only_function<void()> unsubscribe_{};
};

enum class OscColorTargetKind {
    Foreground,
    Background,
    Palette,
};

struct OscColorTarget {
    OscColorTargetKind kind{OscColorTargetKind::Foreground};
    int palette_index{0};

    [[nodiscard]] std::string key() const {
        switch (kind) {
        case OscColorTargetKind::Foreground:
            return "foreground";
        case OscColorTargetKind::Background:
            return "background";
        case OscColorTargetKind::Palette:
            return std::to_string(palette_index);
        }
        return {};
    }

    bool operator==(const OscColorTarget&) const = default;
};

struct OscColorResponse {
    OscColorTarget target;
    std::optional<RgbColor> rgb{std::nullopt};

    bool operator==(const OscColorResponse&) const = default;
};

/// Scales a 1-to-8 character hex channel string to [0, 255] using (value / (16^n - 1)) * 255.
[[nodiscard]] std::optional<int> parse_osc_hex_channel(std::string_view channel);

/// Parses an OSC color value: #rrggbb (6 hex), #rrrrggggbbbb (12 hex), or rgb:/rgba: with / separators.
[[nodiscard]] std::optional<RgbColor> parse_osc_color_value(std::string_view raw_value);

/// Parses an OSC 10, 11, or 4 response terminated by BEL (\x07) or ST (\x1b\).
/// Well-formed replies with unparseable colors return a valid target with rgb = std::nullopt.
[[nodiscard]] std::optional<OscColorResponse> parse_osc_color_response(std::string_view data);

/// Parses a terminal color scheme report (CSI ? 997 ; <1|2> n). Accepts single or concatenated reports.
[[nodiscard]] std::optional<TerminalColorScheme> parse_terminal_color_scheme_report(std::string_view data);

/// Builds the single request burst: OSC 10;? + OSC 11;? + OSC 4;0..15;? + ESC [ c (DA1).
[[nodiscard]] std::string build_terminal_color_query();

} // namespace cch::tui
