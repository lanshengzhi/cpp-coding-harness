#include "ThemeCatalog.hpp"

#include "coding_agent/ResourceDiagnosticPolicy.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] util::Error catalog_error(std::string message, std::string detail = {}) {
    cch::coding_agent::detail::bound_resource_diagnostic_text(message);
    cch::coding_agent::detail::bound_resource_diagnostic_text(detail);
    return util::make_error(
        util::ErrorCode::Validation,
        std::move(message),
        std::move(detail));
}

void bound_diagnostic(ThemeDiagnostic& diagnostic) {
    cch::coding_agent::detail::bound_resource_diagnostic_text(diagnostic.message);
    if (diagnostic.path) {
        cch::coding_agent::detail::bound_resource_diagnostic_text(*diagnostic.path);
    }
}

void bound_diagnostics(std::vector<ThemeDiagnostic>& diagnostics) {
    for (auto& diagnostic : diagnostics) bound_diagnostic(diagnostic);
    if (diagnostics.size() <= cch::coding_agent::detail::kMaxResourceDiagnostics) return;
    diagnostics.resize(cch::coding_agent::detail::kMaxResourceDiagnostics - 1);
    diagnostics.push_back({
        .severity = ThemeDiagnosticSeverity::Warning,
        .code = "diagnostics_truncated",
        .message = "Additional theme resource diagnostics were omitted",
        .origin = ThemeResourceOrigin::Global,
    });
}

[[nodiscard]] std::string diagnostic_message(const util::Error& error) {
    if (error.detail.empty()) return error.message;
    return error.message + ": " + error.detail;
}

void add_diagnostic(
    std::vector<ThemeDiagnostic>& diagnostics,
    ThemeDiagnosticSeverity severity,
    std::string code,
    std::string message,
    ThemeResourceOrigin origin,
    std::optional<std::string> path = std::nullopt) {
    diagnostics.push_back({
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(path),
        .origin = origin,
    });
}

using ThemeMap = std::map<std::string, ThemeResource, std::less<>>;

void apply_theme(
    ThemeMap& themes,
    std::set<std::string, std::less<>>& names_in_tier,
    std::vector<ThemeDiagnostic>& diagnostics,
    ResolvedTheme theme,
    ThemeResourceOrigin origin,
    std::optional<std::filesystem::path> path) {
    if (!names_in_tier.insert(theme.name).second) {
        add_diagnostic(
            diagnostics,
            ThemeDiagnosticSeverity::Warning,
            "duplicate_theme_skipped",
            std::format("theme '{}' skipped: earlier resource at the same precedence takes priority", theme.name),
            origin,
            path ? std::optional<std::string>{path->string()} : std::nullopt);
        return;
    }
    const auto name = theme.name;
    themes.insert_or_assign(
        name,
        ThemeResource{
            .theme = std::move(theme),
            .origin = origin,
            .path = std::move(path),
        });
}

[[nodiscard]] util::Expected<std::vector<std::filesystem::path>> json_files_in_directory(
    const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return std::unexpected(catalog_error(
            "could not list theme directory",
            directory.string() + ": " + error.message()));
    }

    std::vector<std::filesystem::path> files;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        std::error_code type_error;
        const auto regular = iterator->is_regular_file(type_error);
        if (type_error) {
            return std::unexpected(catalog_error(
                "could not inspect theme resource",
                iterator->path().string() + ": " + type_error.message()));
        }
        if (regular && iterator->path().extension() == ".json") files.push_back(iterator->path());
        iterator.increment(error);
        if (error) {
            return std::unexpected(catalog_error(
                "could not list theme directory",
                directory.string() + ": " + error.message()));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void load_automatic_directory(
    const std::filesystem::path& directory,
    ThemeResourceOrigin origin,
    ThemeMap& themes,
    std::vector<ThemeDiagnostic>& diagnostics) {
    if (directory.empty()) return;
    std::error_code error;
    const auto status = std::filesystem::status(directory, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) return;
        add_diagnostic(
            diagnostics,
            ThemeDiagnosticSeverity::Warning,
            "theme_directory_unavailable",
            "could not inspect theme directory: " + error.message(),
            origin,
            directory.string());
        return;
    }
    if (!std::filesystem::exists(status)) return;
    if (!std::filesystem::is_directory(status)) {
        add_diagnostic(
            diagnostics,
            ThemeDiagnosticSeverity::Warning,
            "theme_directory_invalid",
            "automatic theme path is not a directory",
            origin,
            directory.string());
        return;
    }

    if (auto files = json_files_in_directory(directory); !files) {
        add_diagnostic(
            diagnostics,
            ThemeDiagnosticSeverity::Warning,
            "theme_directory_unavailable",
            diagnostic_message(files.error()),
            origin,
            directory.string());
    } else {
        std::set<std::string, std::less<>> names_in_tier;
        for (const auto& file : *files) {
            if (auto loaded = load_theme_file(file); !loaded) {
                add_diagnostic(
                    diagnostics,
                    ThemeDiagnosticSeverity::Warning,
                    "theme_load_failed",
                    diagnostic_message(loaded.error()),
                    origin,
                    file.string());
            } else {
                apply_theme(
                    themes,
                    names_in_tier,
                    diagnostics,
                    std::move(*loaded),
                    origin,
                    file);
            }
        }
    }
}

void load_project_documents(
    std::vector<ThemeSourceDocument> documents,
    ThemeMap& themes,
    std::vector<ThemeDiagnostic>& diagnostics) {
    std::sort(documents.begin(), documents.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    });
    std::set<std::string, std::less<>> names_in_tier;
    for (auto& document : documents) {
        if (auto parsed = parse_theme_json(document.label, document.json); !parsed) {
            add_diagnostic(
                diagnostics,
                ThemeDiagnosticSeverity::Warning,
                "theme_load_failed",
                diagnostic_message(parsed.error()),
                ThemeResourceOrigin::Project,
                document.label);
        } else {
            apply_theme(
                themes,
                names_in_tier,
                diagnostics,
                std::move(*parsed),
                ThemeResourceOrigin::Project,
                std::filesystem::path(document.label));
        }
    }
}

[[nodiscard]] util::ExpectedVoid load_explicit_paths(
    const ThemeCatalogRequest& request,
    ThemeMap& themes,
    std::vector<ThemeDiagnostic>& diagnostics) {
    std::set<std::string, std::less<>> names_in_tier;
    for (const auto& supplied : request.explicit_paths) {
        const auto path = supplied.is_absolute()
            ? supplied.lexically_normal()
            : (request.explicit_path_base / supplied).lexically_normal();
        std::error_code error;
        const auto status = std::filesystem::status(path, error);
        if (error || !std::filesystem::exists(status)) {
            return std::unexpected(catalog_error(
                "explicit theme path is unavailable",
                path.string() + (error ? ": " + error.message() : "")));
        }

        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(status)) {
            if (auto discovered = json_files_in_directory(path); !discovered) {
                return std::unexpected(discovered.error());
            } else {
                files = std::move(*discovered);
            }
            if (files.empty()) {
                return std::unexpected(catalog_error(
                    "explicit theme directory contains no loadable JSON files",
                    path.string()));
            }
        } else if (std::filesystem::is_regular_file(status) && path.extension() == ".json") {
            files.push_back(path);
        } else {
            return std::unexpected(catalog_error(
                "explicit theme path is not a JSON file or directory",
                path.string()));
        }

        for (const auto& file : files) {
            if (auto loaded = load_theme_file(file); !loaded) {
                auto error_result = loaded.error();
                error_result.message = "failed explicit theme resource: " + error_result.message;
                cch::coding_agent::detail::bound_resource_diagnostic_text(error_result.message);
                cch::coding_agent::detail::bound_resource_diagnostic_text(error_result.detail);
                return std::unexpected(std::move(error_result));
            } else {
                apply_theme(
                    themes,
                    names_in_tier,
                    diagnostics,
                    std::move(*loaded),
                    ThemeResourceOrigin::Explicit,
                    file);
            }
        }
    }
    return {};
}

[[nodiscard]] const ThemeResource* find_theme(const ThemeMap& themes, std::string_view name) {
    const auto found = themes.find(name);
    return found == themes.end() ? nullptr : &found->second;
}

struct InitialThemeSelection {
    ResolvedTheme theme;
    std::string name;
    ThemeResourceOrigin origin{ThemeResourceOrigin::Builtin};
    std::optional<ThemeDiagnostic> diagnostic{std::nullopt};
};

[[nodiscard]] util::Expected<InitialThemeSelection> select_initial_theme(
    const ThemeMap& themes,
    const ThemeCatalogRequest& request) {
    if (request.explicit_active_theme) {
        const auto* selected = find_theme(themes, *request.explicit_active_theme);
        if (selected == nullptr) {
            return std::unexpected(catalog_error(
                "explicit active theme is unavailable",
                *request.explicit_active_theme));
        }
        return InitialThemeSelection{
            .theme = selected->theme,
            .name = selected->theme.name,
            .origin = selected->origin,
        };
    }

    if (request.user_active_theme) {
        const auto* selected = find_theme(themes, *request.user_active_theme);
        if (selected != nullptr) {
            return InitialThemeSelection{
                .theme = selected->theme,
                .name = selected->theme.name,
                .origin = selected->origin,
            };
        }
        auto fallback = select_builtin_theme(request.terminal_capabilities);
        return InitialThemeSelection{
            .theme = fallback,
            .name = fallback.name,
            .origin = ThemeResourceOrigin::Builtin,
            .diagnostic = ThemeDiagnostic{
                .severity = ThemeDiagnosticSeverity::Warning,
                .code = "configured_theme_unavailable",
                .message = std::format(
                    "User Settings theme '{}' is unavailable; using the terminal default",
                    *request.user_active_theme),
                .origin = ThemeResourceOrigin::Global,
            },
        };
    }

    auto builtin = select_builtin_theme(request.terminal_capabilities);
    return InitialThemeSelection{
        .theme = builtin,
        .name = builtin.name,
        .origin = ThemeResourceOrigin::Builtin,
    };
}

} // namespace

util::Expected<ThemeCatalogResult> load_theme_catalog(ThemeCatalogRequest request) {
    ThemeMap themes;
    std::vector<ThemeDiagnostic> diagnostics;

    std::set<std::string, std::less<>> builtin_names;
    apply_theme(
        themes,
        builtin_names,
        diagnostics,
        builtin_dark_theme(),
        ThemeResourceOrigin::Builtin,
        std::nullopt);
    apply_theme(
        themes,
        builtin_names,
        diagnostics,
        builtin_light_theme(),
        ThemeResourceOrigin::Builtin,
        std::nullopt);

    const auto global_directory = request.agent_config_directory.empty()
        ? std::filesystem::path{}
        : request.agent_config_directory / "themes";
    load_automatic_directory(
        global_directory,
        ThemeResourceOrigin::Global,
        themes,
        diagnostics);
    load_project_documents(
        std::move(request.trusted_project_themes),
        themes,
        diagnostics);
    if (auto loaded = load_explicit_paths(request, themes, diagnostics); !loaded) {
        return std::unexpected(loaded.error());
    }

    ThemeCatalogResult result;
    result.diagnostics = std::move(diagnostics);
    if (auto selected = select_initial_theme(themes, request); !selected) {
        return std::unexpected(selected.error());
    } else {
        result.initial_theme = std::move(selected->theme);
        result.initial_theme_name = std::move(selected->name);
        result.initial_theme_origin = selected->origin;
        if (selected->diagnostic) result.diagnostics.push_back(std::move(*selected->diagnostic));
    }

    result.effective_themes.reserve(themes.size());
    for (auto& [name, resource] : themes) {
        (void)name;
        result.effective_themes.push_back(std::move(resource));
    }
    bound_diagnostics(result.diagnostics);
    return result;
}

} // namespace cch::coding_agent::tui
