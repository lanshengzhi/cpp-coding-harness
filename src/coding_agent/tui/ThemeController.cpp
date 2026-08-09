#include "coding_agent/tui/ThemeController.hpp"

#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/BoundedText.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string combined_theme_error(const util::Error& error) {
    if (error.detail.empty() || error.detail == error.message) return error.message;
    return error.message + ": " + error.detail;
}

/// pi `ansi256ToHex` subset: the ANSI 256-color index → RGB mapping used by
/// the COLORFGBG luminance classification (basic 0-15, 6x6x6 cube 16-231,
/// grayscale 232-255).
[[nodiscard]] std::array<double, 3> ansi256_to_rgb(int index) {
    constexpr std::array<std::array<double, 3>, 16> kBasic{{
        {{0, 0, 0}}, {{128, 0, 0}}, {{0, 128, 0}}, {{128, 128, 0}},
        {{0, 0, 128}}, {{128, 0, 128}}, {{0, 128, 128}}, {{192, 192, 192}},
        {{128, 128, 128}}, {{255, 0, 0}}, {{0, 255, 0}}, {{255, 255, 0}},
        {{0, 0, 255}}, {{255, 0, 255}}, {{0, 255, 255}}, {{255, 255, 255}},
    }};
    if (index < 16) return kBasic[static_cast<std::size_t>(index)];
    if (index < 232) {
        const auto cube = index - 16;
        const auto channel = [](int value) {
            return static_cast<double>(value == 0 ? 0 : 55 + value * 40);
        };
        return {
            channel(cube / 36),
            channel((cube % 36) / 6),
            channel(cube % 6),
        };
    }
    const auto gray = 8 + (index - 232) * 10;
    return {static_cast<double>(gray), static_cast<double>(gray), static_cast<double>(gray)};
}

/// pi `getRgbColorLuminance` for an ANSI index (pi `getAnsiColorLuminance`):
/// relative luminance from the sRGB channels; ≥ 0.5 classifies light.
[[nodiscard]] bool ansi_index_is_light(int index) {
    const auto rgb = ansi256_to_rgb(index);
    const auto to_linear = [](double channel) {
        const auto value = channel / 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminance = 0.2126 * to_linear(rgb[0]) +
        0.7152 * to_linear(rgb[1]) + 0.0722 * to_linear(rgb[2]);
    return luminance >= 0.5;
}

/// pi `getColorFgBgBackgroundIndex`: the last `;`-separated COLORFGBG field
/// that parses to a 0..255 integer is the background index.
[[nodiscard]] std::optional<int> colorfgbg_background_index(std::string_view colorfgbg) {
    std::optional<int> background;
    std::size_t start = 0;
    while (start <= colorfgbg.size()) {
        const auto separator = colorfgbg.find(';', start);
        const auto end = separator == std::string_view::npos ? colorfgbg.size() : separator;
        auto part = colorfgbg.substr(start, end - start);
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
            const auto [pointer, error] = std::from_chars(
                part.data(), part.data() + part.size(), parsed);
            (void)pointer; // pi baseline parseInt semantics accept a valid numeric prefix.
            if (error == std::errc{} && parsed >= 0 && parsed <= 255) background = parsed;
        }
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return background;
}

/// pi `loadTheme` subset: registered themes first (already-parsed
/// instances, never re-read), then builtins, then
/// `<custom_themes_dir>/<name>.json`, with pi's `Theme not found: <name>`
/// error.
[[nodiscard]] util::Expected<ResolvedTheme> load_theme_by_name(
    std::string_view name,
    const std::filesystem::path& custom_themes_dir,
    const std::vector<RegisteredTheme>& registered) {
    for (const auto& candidate : registered) {
        if (candidate.theme.name == name) return candidate.theme;
    }
    if (name == builtin_dark_theme().name) return builtin_dark_theme();
    if (name == builtin_light_theme().name) return builtin_light_theme();
    if (!custom_themes_dir.empty()) {
        const auto path = custom_themes_dir / (std::string{name} + ".json");
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            return load_theme_file(path);
        }
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        std::format("Theme not found: {}", name)));
}

[[nodiscard]] ResourceDiagnostic warning_diagnostic(
    std::string message,
    std::optional<std::string> path = std::nullopt) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Warning,
        .message = std::move(message),
        .path = std::move(path),
        .collision = std::nullopt,
    };
}

/// pi `dedupeThemes` collision diagnostic (`resource-loader.ts`): the first
/// loaded theme wins; the loser carries the winner/loser paths.
[[nodiscard]] ResourceDiagnostic theme_collision_diagnostic(
    const std::string& name,
    const std::string& winner_path,
    const std::string& loser_path) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Collision,
        .message = "name \"" + name + "\" collision",
        .path = loser_path,
        .collision = ResourceCollision{
            .resource_type = ResourceCollisionResourceType::Theme,
            .name = name,
            .winner_path = winner_path,
            .loser_path = loser_path,
            .winner_source = std::nullopt,
            .loser_source = std::nullopt,
        },
    };
}

void bound_diagnostics(std::vector<ResourceDiagnostic>& diagnostics) {
    const auto bound = [](auto& diagnostic) {
        detail::bound_resource_diagnostic_text(diagnostic.message);
        if (diagnostic.path) {
            detail::bound_resource_diagnostic_text(*diagnostic.path);
        }
        if (diagnostic.collision) {
            detail::bound_resource_diagnostic_text(diagnostic.collision->name);
            detail::bound_resource_diagnostic_text(diagnostic.collision->winner_path);
            detail::bound_resource_diagnostic_text(diagnostic.collision->loser_path);
        }
    };
    for (auto& diagnostic : diagnostics) bound(diagnostic);
    if (diagnostics.size() <= detail::kMaxResourceDiagnostics) return;
    diagnostics.resize(detail::kMaxResourceDiagnostics - 1);
    diagnostics.push_back(warning_diagnostic(
        "Additional theme resource diagnostics were omitted"));
}

/// §5.4/ADR 0017 callback boundary: best-effort void sinks invoked at the
/// controller's edge never let an exception unwind through the TUI. The
/// sinks are state-owned (the interactive host posts to its executor); a
/// throwing sink swallows, matching pi's unhandled `showError`/`onChanged`
/// calls.
template <typename Sink, typename... Args>
void invoke_best_effort_sink(Sink&& sink, Args&&... args) {
    if (!sink) return;
    try {
        (void)std::forward<Sink>(sink)(std::forward<Args>(args)...);
    } catch (...) {
    }
}

/// §5.4 callback boundary for the settings provider: a throwing provider
/// reads as unset (the automatic-pair-unset semantics stay intact).
template <typename Provider>
[[nodiscard]] std::optional<std::string> read_theme_setting(Provider&& provider) {
    if (!provider) return std::nullopt;
    try {
        return provider();
    } catch (...) {
        return std::nullopt;
    }
}

/// Fallible §5.4 callback boundary for the settings committer: failures
/// (including a throwing sink) return the converted error.
template <typename Sink, typename... Args>
[[nodiscard]] util::ExpectedVoid invoke_fallible_sink(Sink&& sink, Args&&... args) {
    if (!sink) return {};
    try {
        return std::forward<Sink>(sink)(std::forward<Args>(args)...);
    } catch (const std::exception& error) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "theme controller sink failed",
            bounded_redacted_presentation(error.what())));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "theme controller sink failed"));
    }
}

} // namespace

TerminalThemeDetection terminal_theme_from_colorfgbg(std::string_view colorfgbg) {
    const auto background = colorfgbg_background_index(colorfgbg);
    if (!background) {
        return {.theme = TerminalTheme::Dark, .high_confidence = false};
    }
    return {
        .theme = ansi_index_is_light(*background) ? TerminalTheme::Light
                                                  : TerminalTheme::Dark,
        .high_confidence = true,
    };
}

TerminalThemeDetection detect_terminal_theme_from_env() {
    const char* colorfgbg = std::getenv("COLORFGBG");
    return terminal_theme_from_colorfgbg(
        colorfgbg == nullptr ? std::string_view{} : std::string_view{colorfgbg});
}

std::optional<std::string> resolve_theme_setting(
    const std::optional<std::string>& setting,
    TerminalTheme /*terminal_theme*/) {
    // pi `resolveThemeSetting` subset: the automatic `light/dark` pair
    // (any value containing "/") reads as unset; other string values pass
    // through.
    if (!setting || setting->find('/') != std::string::npos) return std::nullopt;
    return *setting;
}

std::string_view terminal_theme_name(TerminalTheme theme) {
    return theme == TerminalTheme::Light ? "light" : "dark";
}

struct ThemeController::Impl {
    Impl(
        std::filesystem::path configured_custom_themes_dir,
        std::vector<RegisteredTheme> configured_registered,
        ThemeSettingProvider configured_theme_setting,
        ThemeSettingCommitter configured_committer,
        cch::tui::TerminalColorCapability configured_capability,
        cch::tui::Tui& configured_root,
        ThemeErrorSink configured_show_error,
        ThemeChangedSink configured_on_changed)
        : custom_themes_dir(std::move(configured_custom_themes_dir)),
          registered(std::move(configured_registered)),
          theme_setting(std::move(configured_theme_setting)),
          committer(std::move(configured_committer)),
          capability(configured_capability),
          root(configured_root),
          show_error(std::move(configured_show_error)),
          on_changed(std::move(configured_on_changed)),
          terminal_theme(detect_terminal_theme_from_env().theme),
          live(builtin_dark_theme(), configured_capability) {
        // pi ctor: `activeThemeName = resolveThemeSetting(getThemeSetting(),
        // terminalTheme)` then `initTheme(activeThemeName)` — a defined
        // setting applies, otherwise the env-detected default, with pi's
        // silent dark fallback on load failure.
        const auto setting = read_theme_setting(theme_setting);
        const auto resolved = resolve_theme_setting(setting, terminal_theme);
        const auto name = resolved
            ? *resolved
            : std::string{terminal_theme_name(terminal_theme)};
        auto loaded = load_theme_by_name(name, custom_themes_dir, registered);
        if (loaded) {
            live.replace(std::move(*loaded), capability);
            active_name = std::move(name);
        } else {
            live.replace(builtin_dark_theme(), capability);
            active_name = "dark";
        }
    }

    /// pi `applyThemeName` + `setTheme`: load by name; success replaces the
    /// palette and records the name, failure replaces the palette with
    /// `dark` and records the fallback. Either way the change notifies; the
    /// verbatim fallback message reports only when `show_error` is set.
    [[nodiscard]] bool apply_theme_name(std::string_view name, bool show_error_) {
        auto loaded = load_theme_by_name(name, custom_themes_dir, registered);
        std::optional<std::string> error;
        if (loaded) {
            live.replace(std::move(*loaded), capability);
            active_name = std::string{name};
        } else {
            live.replace(builtin_dark_theme(), capability);
            active_name = "dark";
            error = combined_theme_error(loaded.error());
        }
        notify_changed();
        if (error && show_error_) {
            // pi `applyThemeName` verbatim:
            // `Failed to load theme "<name>": <error>\nFell back to dark theme.`
            invoke_best_effort_sink(
                show_error,
                std::format(
                    "Failed to load theme \"{}\": {}\nFell back to dark theme.",
                    name,
                    *error));
        }
        return loaded.has_value();
    }

    void notify_changed() {
        root.invalidate();
        invoke_best_effort_sink(on_changed);
    }

    std::filesystem::path custom_themes_dir;
    std::vector<RegisteredTheme> registered;
    ThemeSettingProvider theme_setting;
    ThemeSettingCommitter committer;
    cch::tui::TerminalColorCapability capability{cch::tui::TerminalColorCapability::Xterm256};
    cch::tui::Tui& root; // must outlive this controller.
    ThemeErrorSink show_error;
    ThemeChangedSink on_changed;
    TerminalTheme terminal_theme{TerminalTheme::Dark};
    std::string active_name{"dark"};
    LiveTheme live;
};

ThemeController::ThemeController(
    std::filesystem::path custom_themes_dir,
    std::vector<RegisteredTheme> registered,
    ThemeSettingProvider theme_setting,
    ThemeSettingCommitter committer,
    cch::tui::TerminalColorCapability color_capability,
    cch::tui::Tui& root,
    ThemeErrorSink show_error,
    ThemeChangedSink on_changed)
    : impl_(std::make_unique<Impl>(
          std::move(custom_themes_dir),
          std::move(registered),
          std::move(theme_setting),
          std::move(committer),
          color_capability,
          root,
          std::move(show_error),
          std::move(on_changed))) {}

ThemeController::~ThemeController() = default;

void ThemeController::set_registered_themes(std::vector<RegisteredTheme> registered) {
    // pi `setRegisteredThemes`: replace the name → theme map (last wins;
    // the loader already deduped).
    impl_->registered = std::move(registered);
}

const std::vector<RegisteredTheme>& ThemeController::registered_themes() const {
    return impl_->registered;
}

void ThemeController::apply_from_settings() {
    // pi `applyFromSettings` subset: the automatic `light/dark` pair (slash
    // settings values read as unset), the OSC 11/DSR query, auto-sync, and
    // the watcher are absent. A defined setting applies with the verbatim
    // failure message; an unset setting applies the env-only detection and
    // persists it to the global scope on high confidence (pi's
    // `settingsManager.setTheme(detection.theme)` + flush, best-effort).
    const auto raw_setting = read_theme_setting(impl_->theme_setting);
    const auto theme_setting = resolve_theme_setting(raw_setting, impl_->terminal_theme);
    if (theme_setting) {
        impl_->apply_theme_name(*theme_setting, /*show_error=*/true);
        return;
    }
    const auto detection = detect_terminal_theme_from_env();
    impl_->terminal_theme = detection.theme;
    if (!impl_->apply_theme_name(
            terminal_theme_name(detection.theme),
            /*show_error=*/false)) {
        return;
    }
    if (detection.high_confidence && impl_->committer) {
        // pi: `this.settingsManager.setTheme(detection.theme); await
        // flush();` — best-effort, errors ignored like pi's unhandled
        // setTheme.
        (void)invoke_fallible_sink(impl_->committer, terminal_theme_name(detection.theme));
    }
}

bool ThemeController::set_theme_name(std::string_view name, bool show_error) {
    // pi `setThemeName`: disables auto-sync (absent in the subset) and
    // applies.
    return impl_->apply_theme_name(name, show_error);
}

void ThemeController::preview(std::string_view theme_setting_or_name) {
    // pi `preview`: resolve a setting against the terminal theme, else keep
    // the active name; apply in memory (pi `setTheme` — failure replaces
    // the palette with dark silently) and invalidate only on success.
    const auto resolved = resolve_theme_setting(
        std::optional<std::string>{std::string{theme_setting_or_name}},
        impl_->terminal_theme);
    const auto& name = resolved ? *resolved : impl_->active_name;
    auto loaded = load_theme_by_name(name, impl_->custom_themes_dir, impl_->registered);
    if (loaded) {
        impl_->live.replace(std::move(*loaded), impl_->capability);
        impl_->root.invalidate();
    } else {
        impl_->live.replace(builtin_dark_theme(), impl_->capability);
    }
}

std::vector<std::string> ThemeController::available_theme_names() const {
    // pi `getAvailableThemesWithPaths` + `getAvailableThemes`: builtins,
    // then the custom-directory scan (invalid files ignored here like pi),
    // then registered themes — deduped first-wins and sorted by name.
    std::vector<std::string> names;
    std::set<std::string, std::less<>> seen;
    const auto add = [&](std::string name) {
        if (seen.insert(name).second) names.push_back(std::move(name));
    };
    add(builtin_dark_theme().name);
    add(builtin_light_theme().name);

    if (!impl_->custom_themes_dir.empty()) {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        std::filesystem::directory_iterator iterator(impl_->custom_themes_dir, error);
        if (!error) {
            const std::filesystem::directory_iterator end;
            while (iterator != end) {
                std::error_code type_error;
                if (iterator->is_regular_file(type_error) &&
                    iterator->path().extension() == ".json") {
                    files.push_back(iterator->path());
                }
                iterator.increment(error);
                if (error) break;
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            if (auto loaded = load_theme_file(file); loaded) {
                add(std::move(loaded->name));
            }
        }
    }

    for (const auto& candidate : impl_->registered) {
        add(candidate.theme.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string_view ThemeController::active_theme_name() const {
    return impl_->active_name;
}

TerminalTheme ThemeController::terminal_theme() const {
    return impl_->terminal_theme;
}

const LiveTheme& ThemeController::live_theme() const {
    return impl_->live;
}

ResolvedTheme init_boot_theme(
    const std::filesystem::path& agent_config_directory,
    const std::optional<std::string>& theme_setting) {
    // pi `initTheme`: resolve the settings theme (slash values read as
    // unset) or the env-detected default, load it, and fall back to dark
    // silently. Registered themes are absent at boot (registration follows
    // session creation), exactly like pi's main.ts initTheme.
    const auto detection = detect_terminal_theme_from_env();
    const auto resolved = resolve_theme_setting(theme_setting, detection.theme);
    const auto name = resolved
        ? *resolved
        : std::string{terminal_theme_name(detection.theme)};
    const auto custom_themes_dir = agent_config_directory.empty()
        ? std::filesystem::path{}
        : agent_config_directory / "themes";
    auto loaded = load_theme_by_name(name, custom_themes_dir, {});
    if (loaded) return std::move(*loaded);
    return builtin_dark_theme();
}

ThemeDiscoveryResult discover_themes(std::vector<LoadedThemeResource> documents) {
    // pi `loadThemes` (parse step) + `dedupeThemes`: parse every document
    // (failures are warnings with the source path, like pi's
    // `loadThemeFromFile` catch), then dedupe by name first-wins in load
    // order (project `.pi/themes` → user → explicit `--theme`, pi's merge
    // order) with pi's collision diagnostic.
    ThemeDiscoveryResult result;
    std::vector<ResourceDiagnostic> diagnostics;
    std::unordered_map<std::string, std::string> names_to_winner_path;
    result.themes.reserve(documents.size());
    for (auto& document : documents) {
        auto parsed = parse_theme_json(document.path, document.json);
        if (!parsed) {
            diagnostics.push_back(warning_diagnostic(
                combined_theme_error(parsed.error()),
                document.path));
            continue;
        }
        const auto winner = names_to_winner_path.find(parsed->name);
        if (winner != names_to_winner_path.end()) {
            diagnostics.push_back(theme_collision_diagnostic(
                parsed->name,
                winner->second,
                document.path));
            continue;
        }
        names_to_winner_path.emplace(parsed->name, document.path);
        result.themes.push_back(RegisteredTheme{
            .theme = std::move(*parsed),
            .source_path = std::filesystem::path{document.path},
            .scope = document.scope,
        });
    }
    bound_diagnostics(diagnostics);
    result.diagnostics = std::move(diagnostics);
    return result;
}

} // namespace cch::coding_agent::tui
