#pragma once

#include "Theme.hpp"

#include <cch/tui/Overlay.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

enum class ThemeResourceOrigin {
    Builtin,
    Global,
    Project,
    Explicit,
};

enum class ThemeDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

struct ThemeResource {
    ResolvedTheme theme;
    ThemeResourceOrigin origin{ThemeResourceOrigin::Builtin};
    std::optional<std::filesystem::path> path{std::nullopt};
};

/// One project theme document already admitted by Project Trust and read
/// through the workspace-contained project-resource loader.
struct ThemeSourceDocument {
    std::string label;
    std::string json;
};

struct ThemeDiagnostic {
    ThemeDiagnosticSeverity severity{ThemeDiagnosticSeverity::Warning};
    std::string code;
    std::string message;
    std::optional<std::string> path{std::nullopt};
    ThemeResourceOrigin origin{ThemeResourceOrigin::Global};
};

struct ThemeCatalogRequest {
    std::filesystem::path agent_config_directory;
    /// Relative explicit paths resolve against this caller-selected base.
    std::filesystem::path explicit_path_base;
    std::vector<std::filesystem::path> explicit_paths;
    /// The caller supplies only resources admitted by the authoritative
    /// Project Trust and Project Resource load plan.
    std::vector<ThemeSourceDocument> trusted_project_themes;
    std::optional<std::string> explicit_active_theme{std::nullopt};
    std::optional<std::string> user_active_theme{std::nullopt};
    cch::tui::TerminalCapabilities terminal_capabilities;
};

struct ThemeCatalogResult {
    std::vector<ThemeResource> effective_themes;
    ResolvedTheme initial_theme;
    std::string initial_theme_name;
    ThemeResourceOrigin initial_theme_origin{ThemeResourceOrigin::Builtin};
    std::vector<ThemeDiagnostic> diagnostics;
};

/// Perform one deterministic startup discovery pass. This API deliberately
/// exposes no watcher, reload, or resource-rescan operation.
[[nodiscard]] util::Expected<ThemeCatalogResult> load_theme_catalog(ThemeCatalogRequest request);

using ThemeSelectionCommitter = std::move_only_function<util::ExpectedVoid(std::string_view)>;
using ThemeSettingsCancelSink = std::move_only_function<void()>;

/// Owns the selected palette for one Native TUI and invalidates the borrowed
/// root after a successful settings-time selection. The Tui must outlive this
/// controller and every settings overlay made from it.
class ThemeController final {
public:
    ThemeController(
        ThemeCatalogResult catalog,
        cch::tui::Tui& root,
        cch::tui::TerminalColorCapability color_capability,
        ThemeSelectionCommitter committer = {});
    ThemeController(ThemeController&&) = delete;
    ThemeController& operator=(ThemeController&&) = delete;
    ~ThemeController();

    ThemeController(const ThemeController&) = delete;
    ThemeController& operator=(const ThemeController&) = delete;

    [[nodiscard]] std::string_view active_theme_name() const;
    [[nodiscard]] ThemeResourceOrigin active_theme_origin() const;
    [[nodiscard]] std::vector<std::string> available_theme_names() const;
    [[nodiscard]] const LiveTheme& live_theme() const;
    [[nodiscard]] util::ExpectedVoid select_theme(std::string_view name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Build the supported Theme settings interaction from the immutable effective
/// catalog. The caller owns overlay attachment/removal; no generalized resource
/// reload or hot-reload control is exposed.
[[nodiscard]] util::Expected<std::unique_ptr<cch::tui::Overlay>> make_theme_settings_overlay(
    ThemeController& controller,
    ThemeSettingsCancelSink on_cancel = {});

} // namespace cch::coding_agent::tui
