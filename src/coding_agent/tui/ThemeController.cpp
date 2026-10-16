#include "coding_agent/tui/ThemeController.hpp"

#include "coding_agent/tui/theme_color_math.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/BoundedText.hpp"
#include <cch/support/Error.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
using detail::warning_diagnostic;
namespace {

[[nodiscard]] std::string combined_theme_error(const support::Error& error) {
    if (error.detail.empty() || error.detail == error.message) return error.message;
    return error.message + ": " + error.detail;
}

/// pi `detectColorFgBgTheme`: only a one- or two-digit basic-16 index is
/// known; 0..6 and 8 are dark, 7 and 9..15 are light.
[[nodiscard]] std::optional<TerminalTheme> colorfgbg_theme(std::string_view colorfgbg) {
    const auto separator = colorfgbg.rfind(';');
    auto background = colorfgbg.substr(separator == std::string_view::npos ? 0 : separator + 1);
    const auto first = background.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::nullopt;
    const auto last = background.find_last_not_of(" \t\r\n");
    background = background.substr(first, last - first + 1);
    if (background.empty() || background.size() > 2) return std::nullopt;
    // pi's `/^\d{1,2}$/` pin: a sign or any other non-digit is undefined.
    if (background.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
    int index = 0;
    const auto [end, error] = std::from_chars(background.data(), background.data() + background.size(), index);
    if (error != std::errc{} || end != background.data() + background.size() || index > 15) {
        return std::nullopt;
    }
    return index <= 6 || index == 8 ? TerminalTheme::Dark : TerminalTheme::Light;
}

/// pi `loadTheme` subset: registered themes first (already-parsed
/// instances, never re-read), then builtins, then
/// `<custom_themes_dir>/<name>.json`, with pi's `Theme not found: <name>`
/// error.
[[nodiscard]] support::Expected<ResolvedTheme> load_theme_by_name(
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
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        std::format("Theme not found: {}", name)));
}

/// Shared dispatch state for one TUI report sink. The TUI may invoke a
/// registered callback after the controller is gone (a timed-out color query
/// keeps listening for late replies until the TUI stops), so callbacks hold
/// this state and see `active == false` instead of a dangling controller.
template <typename Sink> struct ReportDispatch {
    std::mutex mutex;
    bool active{true};
    /// Bumped on rebind so a callback the TUI copied before it cannot
    /// deliver through the new registration.
    std::uint64_t generation{0};
    std::shared_ptr<Sink> sink;

    void deactivate() {
        std::lock_guard lock(mutex);
        active = false;
        sink.reset();
    }
};

template <typename Sink, typename... Args>
void deliver_report(
        const std::shared_ptr<ReportDispatch<Sink>>& state, std::optional<std::uint64_t> generation, Args... args) {
    std::shared_ptr<Sink> sink;
    {
        std::lock_guard lock(state->mutex);
        if (!state->active) return;
        if (generation && state->generation != *generation) return;
        sink = state->sink;
    }
    if (sink && *sink) (*sink)(std::move(args)...);
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
    (void)std::forward<Sink>(sink)(std::forward<Args>(args)...);
}

/// §5.4 callback boundary for the settings provider: a throwing provider
/// reads as unset (the automatic-pair-unset semantics stay intact).
template <typename Provider>
[[nodiscard]] std::optional<std::string> read_theme_setting(Provider&& provider) {
    if (!provider) return std::nullopt;
    return provider();
}

} // namespace

std::optional<TerminalTheme> terminal_theme_from_colorfgbg(std::string_view colorfgbg) {
    return colorfgbg_theme(colorfgbg);
}

std::optional<TerminalTheme> detect_terminal_theme_from_env() {
    const char* colorfgbg = std::getenv("COLORFGBG");
    return terminal_theme_from_colorfgbg(
        colorfgbg == nullptr ? std::string_view{} : std::string_view{colorfgbg});
}

std::optional<std::string> resolve_theme_setting(
        const std::optional<std::string>& setting, TerminalTheme terminal_theme) {
    if (!setting) return std::nullopt;
    const auto separator = setting->find('/');
    if (separator == std::string::npos) return *setting;
    if (setting->find('/', separator + 1) != std::string::npos) return std::nullopt;
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return std::string{};
        const auto last = value.find_last_not_of(" \t\r\n");
        return std::string{value.substr(first, last - first + 1)};
    };
    const auto light = trim(std::string_view{*setting}.substr(0, separator));
    const auto dark = trim(std::string_view{*setting}.substr(separator + 1));
    if (light.empty() || dark.empty()) return std::nullopt;
    return terminal_theme == TerminalTheme::Light ? light : dark;
}

std::string_view terminal_theme_name(TerminalTheme theme) {
    return theme == TerminalTheme::Light ? "light" : "dark";
}

struct ThemeController::Impl {
    Impl(std::filesystem::path configured_custom_themes_dir,
            std::vector<RegisteredTheme> configured_registered,
            ThemeSettingProvider configured_theme_setting,
            cch::tui::TerminalColorCapability configured_capability,
            cch::tui::ITui& configured_root,
            ThemeErrorSink configured_show_error,
            ThemeChangedSink configured_on_changed,
            ThemeTerminalColorsSink configured_on_terminal_colors,
            ThemeTerminalColorSchemeSink configured_on_terminal_color_scheme)
        : custom_themes_dir(std::move(configured_custom_themes_dir)), registered(std::move(configured_registered)),
          theme_setting(std::move(configured_theme_setting)), capability(configured_capability), root(&configured_root),
          show_error(std::move(configured_show_error)), on_changed(std::move(configured_on_changed)),
          live(builtin_dark_theme(), configured_capability) {
        color_dispatch->sink = std::make_shared<ThemeTerminalColorsSink>(std::move(configured_on_terminal_colors));
        scheme_dispatch->sink =
                std::make_shared<ThemeTerminalColorSchemeSink>(std::move(configured_on_terminal_color_scheme));
        terminal_theme = detected_terminal_theme();
        apply_current_selection(false, false);
    }

    /// The TUI delivers query results and scheme reports from its timer and
    /// input threads, so the callbacks address shared dispatch state instead
    /// of this controller: a report that arrives after destruction sees
    /// `active == false` rather than a dangling pointer.
    ~Impl() {
        color_dispatch->deactivate();
        scheme_dispatch->deactivate();
    }

    /// pi `applyThemeName` + `setTheme`: load by name; success replaces the
    /// palette and records the name, failure replaces the palette with
    /// `dark` and records the fallback. Either way the change notifies; the
    /// verbatim fallback message reports only when `show_error` is set. The
    /// constructor loads the boot theme without notifying (pi `initTheme`).
    [[nodiscard]] bool apply_theme_name(std::string_view name, bool show_error_, bool notify = true) {
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
        if (notify) notify_changed();
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
        root->invalidate();
        invoke_best_effort_sink(on_changed);
    }

    [[nodiscard]] TerminalTheme detected_terminal_theme() {
        if (terminal_colors && terminal_colors->background) {
            const auto appearance =
                    theme_color_math::terminal_appearance(*terminal_colors->background, terminal_colors->foreground);
            return appearance == theme_color_math::TerminalAppearance::Light ? TerminalTheme::Light
                                                                             : TerminalTheme::Dark;
        }
        if (terminal_scheme) return *terminal_scheme;
        return detect_terminal_theme_from_env().value_or(TerminalTheme::Dark);
    }

    [[nodiscard]] bool automatic_setting() {
        const auto setting = read_theme_setting(theme_setting);
        return !setting || setting->find('/') != std::string::npos || *setting == "system";
    }

    [[nodiscard]] std::optional<std::string> effective_theme_setting() {
        const auto setting = read_theme_setting(theme_setting);
        if (!setting || *setting == "system") return std::nullopt;
        return resolve_theme_setting(setting, detected_terminal_theme());
    }

    void apply_current_selection(bool show_error, bool notify = true) {
        terminal_theme = detected_terminal_theme();
        auto theme_name = effective_theme_setting();
        if (!theme_name) theme_name = std::string{terminal_theme_name(terminal_theme)};
        (void)apply_theme_name(*theme_name, show_error, notify);
    }

    void reapply_for_terminal() {
        if (active_name == "<in-memory>") return;
        const auto theme_name = effective_theme_setting();
        if (!theme_name || *theme_name != active_name) {
            apply_current_selection(false);
        }
    }

    /// pi `queryTerminalColors`: one OSC 10/11/4 + DA1 burst; the timeout
    /// result and every later reply carry the theme update. A report from an
    /// older query still applies: the TUI answers the frontmost pending
    /// query, so the reply to a superseded burst can be the only one that
    /// arrives.
    void start_color_query() {
        {
            std::lock_guard lock(color_dispatch->mutex);
            if (!color_dispatch->active) return;
        }
        cch::tui::TerminalColorsQueryOptions options;
        options.timeout = std::chrono::milliseconds{100};
        options.on_complete = [state = color_dispatch](cch::tui::TerminalColors colors) {
            deliver_report(state, std::nullopt, std::move(colors));
        };
        options.on_late_reply = [state = color_dispatch](cch::tui::TerminalColors colors) {
            deliver_report(state, std::nullopt, std::move(colors));
        };
        (void)root->query_terminal_colors(std::move(options));
    }

    void set_auto_sync(bool enabled) {
        if (auto_sync_enabled == enabled) return;
        auto_sync_enabled = enabled;
        root->set_terminal_color_scheme_notifications(enabled);
    }

    std::filesystem::path custom_themes_dir;
    std::vector<RegisteredTheme> registered;
    ThemeSettingProvider theme_setting;
    cch::tui::TerminalColorCapability capability{cch::tui::TerminalColorCapability::Xterm256};
    cch::tui::ITui* root; // re-pointed before the previously selected root is destroyed.
    ThemeErrorSink show_error;
    ThemeChangedSink on_changed;
    TerminalTheme terminal_theme{TerminalTheme::Dark};
    std::string active_name{"dark"};
    std::shared_ptr<ReportDispatch<ThemeTerminalColorsSink>> color_dispatch{
            std::make_shared<ReportDispatch<ThemeTerminalColorsSink>>()};
    std::shared_ptr<ReportDispatch<ThemeTerminalColorSchemeSink>> scheme_dispatch{
            std::make_shared<ReportDispatch<ThemeTerminalColorSchemeSink>>()};
    cch::tui::TerminalColorSchemeListenerHandle scheme_listener;
    std::optional<cch::tui::TerminalColors> terminal_colors;
    std::optional<TerminalTheme> terminal_scheme;
    bool auto_sync_enabled{false};
    LiveTheme live;
};

ThemeController::ThemeController(std::filesystem::path custom_themes_dir,
        std::vector<RegisteredTheme> registered,
        ThemeSettingProvider theme_setting,
        cch::tui::TerminalColorCapability color_capability,
        cch::tui::ITui& root,
        ThemeErrorSink show_error,
        ThemeChangedSink on_changed,
        ThemeTerminalColorsSink on_terminal_colors,
        ThemeTerminalColorSchemeSink on_terminal_color_scheme)
    : impl_(std::make_unique<Impl>(std::move(custom_themes_dir),
              std::move(registered),
              std::move(theme_setting),
              color_capability,
              root,
              std::move(show_error),
              std::move(on_changed),
              std::move(on_terminal_colors),
              std::move(on_terminal_color_scheme))) {
    rebind_terminal_color_scheme_listener();
}

ThemeController::~ThemeController() { dispose(); }

void ThemeController::set_registered_themes(std::vector<RegisteredTheme> registered) {
    // pi `setRegisteredThemes`: replace the name → theme map (last wins;
    // the loader already deduped).
    impl_->registered = std::move(registered);
}

const std::vector<RegisteredTheme>& ThemeController::registered_themes() const {
    return impl_->registered;
}

void ThemeController::apply_from_settings() {
    const auto setting = read_theme_setting(impl_->theme_setting);
    impl_->set_auto_sync(impl_->automatic_setting());
    impl_->apply_current_selection(setting.has_value());
    impl_->start_color_query();
}

void ThemeController::apply_terminal_colors(cch::tui::TerminalColors colors) {
    const auto previous = impl_->terminal_colors.value_or(cch::tui::TerminalColors{});
    cch::tui::TerminalColors next;
    next.foreground = colors.foreground ? colors.foreground : previous.foreground;
    next.background = colors.background ? colors.background : previous.background;
    next.palette = colors.palette ? colors.palette : previous.palette;
    // pi `applyTerminalColors`: skip the rebuild when the report repeats the
    // retained colors (timeouts report nothing), otherwise reapply and repaint.
    if (impl_->terminal_colors && *impl_->terminal_colors == next) return;
    impl_->terminal_colors = next;
    impl_->reapply_for_terminal();
    impl_->root->invalidate();
    impl_->root->request_render();
}

void ThemeController::apply_terminal_color_scheme(TerminalTheme scheme) {
    if (!impl_->auto_sync_enabled) return;
    const auto previous = impl_->detected_terminal_theme();
    impl_->terminal_scheme = scheme;
    if (impl_->detected_terminal_theme() != previous) impl_->reapply_for_terminal();
    impl_->start_color_query();
}

void ThemeController::rebind_tui(cch::tui::ITui& root) {
    if (impl_->scheme_listener.active()) impl_->scheme_listener.reset();
    impl_->root = &root;
    rebind_terminal_color_scheme_listener();
}

void ThemeController::rebind_terminal_color_scheme_listener() {
    if (impl_->scheme_listener.active()) impl_->scheme_listener.reset();
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(impl_->scheme_dispatch->mutex);
        if (!impl_->scheme_dispatch->active) return;
        generation = ++impl_->scheme_dispatch->generation;
    }
    impl_->scheme_listener = impl_->root->on_terminal_color_scheme_change(
            [state = impl_->scheme_dispatch, generation](cch::tui::TerminalColorScheme scheme) {
                deliver_report(state,
                        generation,
                        scheme == cch::tui::TerminalColorScheme::Light ? TerminalTheme::Light : TerminalTheme::Dark);
            });
    // pi `rebindTui`: the re-registered TUI re-applies the current mode.
    impl_->root->set_terminal_color_scheme_notifications(impl_->auto_sync_enabled);
}

void ThemeController::dispose() {
    if (!impl_) return;
    impl_->set_auto_sync(false);
    impl_->scheme_listener.reset();
}

bool ThemeController::set_theme_name(std::string_view name, bool show_error) {
    if (name == "system") {
        // pi `setThemeName` enables auto-sync for the system theme, whose
        // generated colors are outside #991: the appearance-selected
        // built-in theme stands in for it.
        impl_->set_auto_sync(true);
        impl_->apply_current_selection(show_error);
        return true;
    }
    impl_->set_auto_sync(false);
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
        impl_->root->invalidate();
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
    const auto resolved = resolve_theme_setting(theme_setting, detection.value_or(TerminalTheme::Dark));
    const auto name = resolved ? *resolved : std::string{terminal_theme_name(detection.value_or(TerminalTheme::Dark))};
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
            diagnostics.push_back(detail::make_collision_diagnostic("name \"" + parsed->name + "\" collision",
                    ResourceCollisionResourceType::Theme,
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
