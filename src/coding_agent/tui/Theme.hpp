#pragma once

#include <cch/tui/Editor.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/util/Error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace cch::coding_agent::tui {

inline constexpr std::size_t kRequiredThemeTokenCount{
#define CCH_THEME_TOKEN(enum_name, wire_name, required) +required
    0
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};
inline constexpr std::size_t kThemeTokenCount{
#define CCH_THEME_TOKEN(enum_name, wire_name, required) +1
    0
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};

enum class ThemeToken {
#define CCH_THEME_TOKEN(enum_name, wire_name, required) enum_name,
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};

struct TerminalDefaultThemeColor {
    bool operator==(const TerminalDefaultThemeColor&) const = default;
};

struct RgbThemeColor {
    std::uint8_t red{0};
    std::uint8_t green{0};
    std::uint8_t blue{0};

    bool operator==(const RgbThemeColor&) const = default;
};

struct XtermThemeColor {
    std::uint8_t index{0};

    bool operator==(const XtermThemeColor&) const = default;
};

using ResolvedThemeColor = std::variant<TerminalDefaultThemeColor, RgbThemeColor, XtermThemeColor>;

struct ResolvedTheme {
    std::string name;
    std::array<ResolvedThemeColor, kThemeTokenCount> colors{};

    bool operator==(const ResolvedTheme&) const = default;
};

[[nodiscard]] std::span<const ThemeToken> all_theme_tokens();
[[nodiscard]] std::string_view theme_token_name(ThemeToken token);
[[nodiscard]] const ResolvedThemeColor& color_for(const ResolvedTheme& theme, ThemeToken token);

[[nodiscard]] util::Expected<ResolvedTheme> parse_theme_json(
    std::string_view label,
    std::string_view json);
[[nodiscard]] util::Expected<ResolvedTheme> load_theme_file(const std::filesystem::path& path);
[[nodiscard]] ResolvedTheme builtin_dark_theme();
[[nodiscard]] ResolvedTheme builtin_light_theme();
[[nodiscard]] ResolvedTheme select_builtin_theme(const cch::tui::TerminalCapabilities& capabilities);

/// Shared live palette used by render-time hooks. The object must outlive calls
/// to replace(); hooks retain the shared palette state after this handle is destroyed.
class LiveTheme final {
public:
    explicit LiveTheme(
        ResolvedTheme theme,
        cch::tui::TerminalColorCapability capability);
    LiveTheme(LiveTheme&&) noexcept;
    LiveTheme& operator=(LiveTheme&&) noexcept;
    ~LiveTheme();

    LiveTheme(const LiveTheme&) = delete;
    LiveTheme& operator=(const LiveTheme&) = delete;

    void replace(
        ResolvedTheme theme,
        cch::tui::TerminalColorCapability capability);
    [[nodiscard]] std::string foreground(ThemeToken token, std::string text) const;
    [[nodiscard]] std::string background(ThemeToken token, std::string text) const;
    [[nodiscard]] cch::tui::TextStyleHook foreground_hook(ThemeToken token) const;
    [[nodiscard]] cch::tui::BackgroundHook background_hook(ThemeToken token) const;
    [[nodiscard]] cch::tui::MarkdownStyleConfig markdown_style() const;
    [[nodiscard]] cch::tui::EditorTheme editor_theme() const;
    [[nodiscard]] cch::tui::SelectListTheme select_list_theme() const;
    [[nodiscard]] cch::tui::SettingsListTheme settings_list_theme() const;

private:
    struct Impl;
    [[nodiscard]] static std::string apply_style(
        const std::shared_ptr<Impl>& impl,
        ThemeToken token,
        std::string text,
        bool background);

    std::shared_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::tui
