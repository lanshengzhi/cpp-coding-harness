#include "coding_agent/tui/LoadedResources.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `formatCompactList`: one dim `  a, b, c` line, trimmed and filtered,
/// sorted unless `sort == false`.
[[nodiscard]] std::string format_compact_list(
    const LiveTheme& theme,
    std::vector<std::string> labels,
    bool sort) {
    std::vector<std::string> kept;
    kept.reserve(labels.size());
    for (auto& label : labels) {
        // pi `.trim()` then keep non-empty.
        std::size_t begin = 0;
        while (begin < label.size() &&
            (label[begin] == ' ' || label[begin] == '\t' || label[begin] == '\r' ||
             label[begin] == '\n')) {
            ++begin;
        }
        std::size_t end = label.size();
        while (end > begin &&
            (label[end - 1] == ' ' || label[end - 1] == '\t' || label[end - 1] == '\r' ||
             label[end - 1] == '\n')) {
            --end;
        }
        auto trimmed = label.substr(begin, end - begin);
        if (!trimmed.empty()) {
            kept.push_back(std::move(trimmed));
        }
    }
    if (sort) {
        std::sort(kept.begin(), kept.end());
    }
    std::string joined;
    for (std::size_t index = 0; index < kept.size(); ++index) {
        if (index != 0) joined += ", ";
        joined += kept[index];
    }
    return theme.foreground(ThemeToken::Dim, "  " + joined);
}

/// pi `formatDisplayPath`: replace the home prefix with `~`.
[[nodiscard]] std::string format_display_path(
    const std::string& path,
    const std::filesystem::path& home) {
    if (home.empty()) return path;
    const auto home_string = home.string();
    if (path.starts_with(home_string)) {
        return "~" + path.substr(home_string.size());
    }
    return path;
}

/// pi `getDisplaySourceInfo` subset (no npm/git package sources): the label
/// and optional temp scope-label for `formatPathWithSource`.
struct DisplaySourceInfo {
    std::string label;
    std::optional<std::string> scope_label;
};

[[nodiscard]] DisplaySourceInfo get_display_source_info(const SourceInfo& source_info) {
    const auto& source = source_info.source;
    if (source == "cli") {
        return {
            .label = "path",
            .scope_label = source_info.scope == SourceScope::Temporary
                ? std::optional<std::string>{"temp"}
                : std::nullopt,
        };
    }
    // "auto" (and the "local" default) resolve by scope, like pi.
    switch (source_info.scope) {
    case SourceScope::User:
        return {.label = "user", .scope_label = std::nullopt};
    case SourceScope::Project:
        return {.label = "project", .scope_label = std::nullopt};
    case SourceScope::Temporary:
        return {.label = "path", .scope_label = std::optional<std::string>{"temp"}};
    }
    return {.label = "path", .scope_label = std::nullopt};
}

/// pi `getScopeGroup`: source "cli" or scope temporary → "path"; scope
/// user/project → the same.
[[nodiscard]] std::string get_scope_group(const SourceInfo& source_info) {
    if (source_info.source == "cli" || source_info.scope == SourceScope::Temporary) {
        return "path";
    }
    return source_info.scope == SourceScope::User ? "user" : "project";
}

/// pi `findSourceInfoForPath`: exact path, then walk up parent directories
/// looking for a prefix entry.
[[nodiscard]] std::optional<SourceInfo> find_source_info_for_path(
    const std::string& path,
    const std::map<std::string, SourceInfo>& source_infos) {
    if (const auto exact = source_infos.find(path); exact != source_infos.end()) {
        return exact->second;
    }
    auto current = path;
    while (current.find('/') != std::string::npos) {
        const auto slash = current.find_last_of('/');
        current = current.substr(0, slash);
        if (const auto parent = source_infos.find(current); parent != source_infos.end()) {
            return parent->second;
        }
    }
    return std::nullopt;
}

/// pi `formatScopeGroups` subset: project → user → path groups, each a
/// two-space accent scope label followed by four-space dim display strings
/// sorted by the underlying path (pi sorts `group.paths` by `path`, then
/// formats each). Package sources are outside the loader subset (no
/// extensions/package-manager surface).
[[nodiscard]] std::string format_scope_groups(
    const LiveTheme& theme,
    const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& groups) {
    std::vector<std::string> lines;
    const auto group = [&](const char* name) {
        const auto it = groups.find(name);
        if (it == groups.end() || it->second.empty()) return;
        lines.push_back(theme.foreground(ThemeToken::Accent, "  " + std::string{name}));
        auto sorted = it->second;
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (const auto& [path, display] : sorted) {
            (void)path;
            lines.push_back(theme.foreground(ThemeToken::Dim, "    " + display));
        }
    };
    group("project");
    group("user");
    group("path");
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) result.push_back('\n');
        result += lines[index];
    }
    return result;
}

} // namespace

LoadedResources::LoadedResources(const LiveTheme& theme) : theme_(theme) {}

void LoadedResources::set_data(Data data) {
    data_ = std::move(data);
}

void LoadedResources::set_expanded(bool expanded) {
    expanded_ = expanded;
}

bool LoadedResources::expanded() const {
    return expanded_;
}

std::string LoadedResources::format_display_path(const std::string& path) const {
    return ::cch::coding_agent::tui::format_display_path(path, data_.home);
}

std::string LoadedResources::format_context_path(const std::string& path) const {
    // pi `formatContextPath`: cwd-relative when possible, else `~`-home
    // display of the absolute path.
    std::error_code ec;
    const auto cwd = std::filesystem::absolute(data_.cwd, ec);
    std::filesystem::path absolute;
    if (std::filesystem::path{path}.is_absolute()) {
        absolute = std::filesystem::path{path};
    } else {
        std::error_code resolve_ec;
        absolute = std::filesystem::absolute(cwd / path, resolve_ec);
    }
    std::error_code rel_ec;
    auto relative = std::filesystem::relative(absolute, cwd, rel_ec);
    if (!rel_ec) {
        const auto normalized = relative.lexically_normal().string();
        const auto inside =
            normalized.empty() || normalized == "." ||
            (normalized != ".." && !normalized.starts_with("../") &&
             !std::filesystem::path{normalized}.is_absolute());
        if (inside) {
            return normalized.empty() ? "." : normalized;
        }
    }
    return format_display_path(absolute.lexically_normal().string());
}

std::string LoadedResources::format_path_with_source(
    const std::string& path,
    const std::optional<SourceInfo>& source_info) const {
    if (!source_info) {
        return format_display_path(path);
    }
    // pi `getShortPath` has no package-source branches in the subset: it
    // falls through to `formatDisplayPath`.
    const auto short_path = format_display_path(path);
    const auto display = get_display_source_info(*source_info);
    const auto label_text = display.scope_label
        ? display.label + " (" + *display.scope_label + ")"
        : display.label;
    return label_text + " " + short_path;
}

SourceInfo LoadedResources::theme_source_info(const ThemeItem& theme_item) {
    // The scope is the only provenance the loader carries (pi
    // `Theme.sourceInfo`); source stays "auto" for display equivalence.
    return SourceInfo{
        .path = theme_item.path,
        .source = "auto",
        .scope = theme_item.scope,
        .origin = SourceOrigin::TopLevel,
        .base_dir = std::nullopt,
    };
}

std::map<std::string, SourceInfo> LoadedResources::build_source_infos() const {
    // pi builds the map from extensions + skills + prompts + themes.
    std::map<std::string, SourceInfo> source_infos;
    for (const auto& skill : data_.skills) {
        source_infos.emplace(skill.path, skill.source_info);
    }
    for (const auto& templ : data_.templates) {
        source_infos.emplace(templ.path, templ.source_info);
    }
    for (const auto& theme_item : data_.themes) {
        source_infos.emplace(theme_item.path, theme_source_info(theme_item));
    }
    return source_infos;
}

std::string LoadedResources::format_diagnostics(
    const std::vector<ResourceDiagnostic>& diagnostics,
    const std::map<std::string, SourceInfo>& source_infos) const {
    std::vector<std::string> lines;
    // Group collision diagnostics by name.
    std::map<std::string, std::vector<const ResourceDiagnostic*>> collisions;
    std::vector<const ResourceDiagnostic*> other;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.type == ResourceDiagnosticType::Collision && diagnostic.collision) {
            collisions[diagnostic.collision->name].push_back(&diagnostic);
        } else {
            other.push_back(&diagnostic);
        }
    }
    for (const auto& [name, collision_list] : collisions) {
        const auto* first = collision_list.front();
        if (!first || !first->collision) continue;
        lines.push_back(
            theme_.foreground(ThemeToken::Warning, std::format("  \"{}\" collision:", name)));
        lines.push_back(theme_.foreground(
            ThemeToken::Dim,
            std::format(
                "    {} {}",
                theme_.foreground(ThemeToken::Success, "\u2713"),
                format_path_with_source(
                    first->collision->winner_path,
                    find_source_info_for_path(first->collision->winner_path, source_infos)))));
        for (const auto* diagnostic : collision_list) {
            if (!diagnostic->collision) continue;
            lines.push_back(theme_.foreground(
                ThemeToken::Dim,
                std::format(
                    "    {} {} (skipped)",
                    theme_.foreground(ThemeToken::Warning, "\u2717"),
                    format_path_with_source(
                        diagnostic->collision->loser_path,
                        find_source_info_for_path(diagnostic->collision->loser_path, source_infos)))));
        }
    }
    for (const auto* diagnostic : other) {
        const auto color =
            diagnostic->type == ResourceDiagnosticType::Error
            ? ThemeToken::Error
            : ThemeToken::Warning;
        if (diagnostic->path) {
            lines.push_back(theme_.foreground(
                color,
                "  " + format_path_with_source(
                           *diagnostic->path,
                           find_source_info_for_path(*diagnostic->path, source_infos))));
            lines.push_back(
                theme_.foreground(color, "    " + diagnostic->message));
        } else {
            lines.push_back(
                theme_.foreground(color, "  " + diagnostic->message));
        }
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) result.push_back('\n');
        result += lines[index];
    }
    return result;
}

support::Expected<cch::tui::RenderResult> LoadedResources::render(std::size_t width) {
    std::string text;

    const auto add_section = [&](const char* name, std::string body) {
        if (!text.empty()) text.push_back('\n');
        text += theme_.foreground(ThemeToken::MdHeading, std::format("[{}]", name));
        text.push_back('\n');
        text += body;
    };
    const auto add_diagnostics_section = [&](const char* name, std::string body) {
        if (!text.empty()) text.push_back('\n');
        text += theme_.foreground(ThemeToken::Warning, std::format("[{}]", name));
        text.push_back('\n');
        text += body;
    };

    // ── Context (SYSTEM/APPEND sources + Project Context Files) ───────────
    if (!data_.context_paths.empty()) {
        if (!text.empty()) text.push_back('\n');
        std::string body;
        if (expanded_) {
            // Flat dim display paths in source order (no scope groups).
            for (const auto& path : data_.context_paths) {
                if (!body.empty()) body.push_back('\n');
                body += theme_.foreground(
                    ThemeToken::Dim,
                    "  " + format_display_path(path));
            }
        } else {
            std::vector<std::string> labels;
            labels.reserve(data_.context_paths.size());
            for (const auto& path : data_.context_paths) {
                labels.push_back(format_context_path(path));
            }
            body = format_compact_list(theme_, std::move(labels), /*sort*/ false);
        }
        add_section("Context", std::move(body));
    }

    // ── Skills ────────────────────────────────────────────────────────────
    if (!data_.skills.empty()) {
        std::string body;
        if (expanded_) {
            std::map<std::string, std::vector<std::pair<std::string, std::string>>>
                display_by_group;
            for (const auto& skill : data_.skills) {
                display_by_group[get_scope_group(skill.source_info)].emplace_back(
                    skill.path, format_display_path(skill.path));
            }
            body = format_scope_groups(theme_, display_by_group);
        } else {
            std::vector<std::string> labels;
            labels.reserve(data_.skills.size());
            for (const auto& skill : data_.skills) {
                labels.push_back(skill.name);
            }
            body = format_compact_list(theme_, std::move(labels), /*sort*/ true);
        }
        add_section("Skills", std::move(body));
    }

    // ── Prompts ───────────────────────────────────────────────────────────
    if (!data_.templates.empty()) {
        std::string body;
        if (expanded_) {
            std::map<std::string, std::vector<std::pair<std::string, std::string>>>
                display_by_group;
            for (const auto& templ : data_.templates) {
                display_by_group[get_scope_group(templ.source_info)].emplace_back(
                    templ.path, "/" + templ.name);
            }
            body = format_scope_groups(theme_, display_by_group);
        } else {
            std::vector<std::string> labels;
            labels.reserve(data_.templates.size());
            for (const auto& templ : data_.templates) {
                labels.push_back("/" + templ.name);
            }
            body = format_compact_list(theme_, std::move(labels), /*sort*/ true);
        }
        add_section("Prompts", std::move(body));
    }

    // ── Themes (custom only) ──────────────────────────────────────────────
    if (!data_.themes.empty()) {
        std::string body;
        if (expanded_) {
            std::map<std::string, std::vector<std::pair<std::string, std::string>>>
                display_by_group;
            for (const auto& theme_item : data_.themes) {
                const auto info = theme_source_info(theme_item);
                display_by_group[get_scope_group(info)].emplace_back(
                    theme_item.path, format_display_path(theme_item.path));
            }
            body = format_scope_groups(theme_, display_by_group);
        } else {
            std::vector<std::string> labels;
            labels.reserve(data_.themes.size());
            for (const auto& theme_item : data_.themes) {
                labels.push_back(theme_item.name);
            }
            body = format_compact_list(theme_, std::move(labels), /*sort*/ true);
        }
        add_section("Themes", std::move(body));
    }

    // ── Diagnostics (pi formatDiagnostics) ────────────────────────────────
    const auto source_infos = build_source_infos();
    if (!data_.skill_diagnostics.empty()) {
        add_diagnostics_section(
            "Skill conflicts", format_diagnostics(data_.skill_diagnostics, source_infos));
    }
    if (!data_.prompt_diagnostics.empty()) {
        add_diagnostics_section(
            "Prompt conflicts", format_diagnostics(data_.prompt_diagnostics, source_infos));
    }
    if (!data_.theme_diagnostics.empty()) {
        add_diagnostics_section(
            "Theme conflicts", format_diagnostics(data_.theme_diagnostics, source_infos));
    }

    if (text.empty()) {
        return cch::tui::RenderResult{.lines = {}};
    }
    cch::tui::Text component(std::move(text), 0, 0);
    auto rendered = component.render(width);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered;
}

void LoadedResources::invalidate() {}

} // namespace cch::coding_agent::tui
