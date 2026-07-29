#include "../../include/cch/coding_agent/Settings.hpp"
#include "util/Json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace cch::coding_agent {
namespace {

[[nodiscard]] util::Error settings_write_error(std::string message, std::string detail = {}) {
    return util::make_error(
        util::ErrorCode::Workspace,
        std::move(message),
        std::move(detail));
}

void load_compatible_string(
    const util::JsonValue::object_t& object,
    std::string_view key,
    std::optional<std::string>& target) {
    if (auto member = object.find(std::string{key}); member != object.end()) {
        if (const auto* value = member->second.get_if<std::string>()) {
            target = *value;
        }
    }
}

[[nodiscard]] util::ExpectedVoid write_settings_text(
    const std::filesystem::path& settings_path,
    std::string serialized) {
    serialized.push_back('\n');

    const auto parent = settings_path.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return std::unexpected(settings_write_error(
                "could not create settings directory",
                directory_error.message()));
        }
#if defined(__unix__) || defined(__APPLE__)
        (void)::chmod(parent.c_str(), 0700);
#endif
    }

    std::error_code target_error;
    const auto target_status = std::filesystem::symlink_status(settings_path, target_error);
    if (!target_error && std::filesystem::is_symlink(target_status)) {
        return std::unexpected(settings_write_error(
            "refusing to write symlinked settings file",
            settings_path.string()));
    }
    if (target_error && target_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(settings_write_error(
            "could not inspect settings file",
            target_error.message()));
    }

    auto temporary = settings_path;
    temporary += ".tmp";
    std::error_code temporary_error;
    const auto temporary_status = std::filesystem::symlink_status(temporary, temporary_error);
    if (!temporary_error && std::filesystem::exists(temporary_status)) {
        return std::unexpected(settings_write_error(
            "refusing to replace existing temporary settings path",
            temporary.string()));
    }
    if (temporary_error && temporary_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(settings_write_error(
            "could not inspect temporary settings path",
            temporary_error.message()));
    }

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(settings_write_error(
                "could not open temporary settings file",
                temporary.string()));
        }
        output << serialized;
        if (!output) {
            return std::unexpected(settings_write_error(
                "could not write temporary settings file",
                temporary.string()));
        }
    }
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(temporary.c_str(), 0600);
#endif

    std::error_code rename_error;
    std::filesystem::rename(temporary, settings_path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return std::unexpected(settings_write_error(
            "could not replace settings file",
            rename_error.message()));
    }
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(settings_path.c_str(), 0600);
#endif
    return {};
}

[[nodiscard]] util::ExpectedVoid write_settings_object(
    const std::filesystem::path& settings_path,
    util::JsonValue::object_t object) {
    if (auto serialized = util::write_json(util::JsonValue{std::move(object)}); !serialized) {
        return std::unexpected(serialized.error());
    } else {
        return write_settings_text(settings_path, std::move(*serialized));
    }
}

} // namespace

util::Expected<UserSettings> SettingsLoader::load(const std::filesystem::path& settings_path) {
    UserSettings settings;

    std::ifstream file(settings_path);
    if (!file.is_open()) {
        // File does not exist or is unreadable — not an error; use defaults.
        return settings;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (content.empty()) {
        return settings;
    }

    auto json = util::read_json<util::JsonValue>(content);
    if (!json) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "failed to parse settings file",
            "settings file at '" + settings_path.string() + "' contains invalid JSON"));
    }

    // Read known fields; ignore unknown keys for forward compatibility.
    if (json->holds<util::JsonValue::object_t>()) {
        auto& obj = json->get_object();
        if (auto it = obj.find("provider"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.provider = *str;
            }
        }
        if (auto it = obj.find("model"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.model = *str;
            }
        }
        if (auto it = obj.find("base_url"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.base_url = *str;
            }
        }
        if (auto it = obj.find("api_key_env"); it != obj.end()) {
            if (it->second.holds<util::JsonValue::array_t>()) {
                auto& arr = it->second.get_array();
                std::vector<std::string> chain;
                for (const auto& item : arr) {
                    if (auto* str = item.get_if<std::string>()) {
                        chain.push_back(*str);
                    }
                }
                if (!chain.empty()) {
                    settings.api_key_env = std::move(chain);
                }
            } else if (auto* str = it->second.get_if<std::string>()) {
                // Single string → single-element chain
                std::vector<std::string> chain;
                chain.push_back(*str);
                settings.api_key_env = std::move(chain);
            }
        }
        if (auto it = obj.find("auth"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.auth = *str;
            }
        }
        if (auto it = obj.find("sessionDir"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.session_dir = *str;
            }
        }
        if (auto it = obj.find("theme"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                settings.theme = *str;
            }
        }
        load_compatible_string(obj, "shellPath", settings.shell_path);
        load_compatible_string(obj, "shellCommandPrefix", settings.shell_command_prefix);
        if (auto it = obj.find("default_project_trust"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                auto parsed = parse_default_project_trust(*str);
                if (!parsed) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Validation,
                        "invalid default_project_trust",
                        "default_project_trust must be one of: ask, always, never"));
                }
                settings.default_project_trust = *parsed;
            } else {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "invalid default_project_trust",
                    "default_project_trust must be a string"));
            }
        }
        if (auto it = obj.find("project_resources"); it != obj.end()) {
            if (!it->second.holds<util::JsonValue::object_t>()) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Validation,
                    "invalid project_resources",
                    "project_resources must be an object"));
            }
            const auto& resources = it->second.get_object();
            if (auto skills_it = resources.find("skills"); skills_it != resources.end()) {
                if (auto* str = skills_it->second.get_if<std::string>()) {
                    auto parsed = parse_resource_enablement(*str);
                    if (!parsed) {
                        return std::unexpected(util::make_error(
                            util::ErrorCode::Validation,
                            "invalid project_resources.skills",
                            "project_resources.skills must be one of: auto, on, off"));
                    }
                    settings.project_skills = *parsed;
                } else {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Validation,
                        "invalid project_resources.skills",
                        "project_resources.skills must be a string"));
                }
            }
            if (auto themes_it = resources.find("themes"); themes_it != resources.end()) {
                if (auto* str = themes_it->second.get_if<std::string>()) {
                    if (auto parsed = parse_resource_enablement(*str); !parsed) {
                        return std::unexpected(util::make_error(
                            util::ErrorCode::Validation,
                            "invalid project_resources.themes",
                            "project_resources.themes must be one of: auto, on, off"));
                    } else {
                        settings.project_themes = *parsed;
                    }
                } else {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Validation,
                        "invalid project_resources.themes",
                        "project_resources.themes must be a string"));
                }
            }
        }
    }

    return settings;
}

util::ExpectedVoid SettingsLoader::save_theme_selection(
    const std::filesystem::path& settings_path,
    std::string_view theme_name) {
    if (settings_path.empty()) {
        return std::unexpected(settings_write_error("settings path is empty"));
    }
    if (theme_name.empty() || theme_name.find('/') != std::string_view::npos) {
        return std::unexpected(settings_write_error(
            "invalid theme selection",
            "theme selection must be a non-empty available theme name"));
    }

    util::JsonValue::object_t object;
    std::ifstream input(settings_path, std::ios::binary);
    if (input.is_open()) {
        std::ostringstream content;
        content << input.rdbuf();
        if (!input.good() && !input.eof()) {
            return std::unexpected(settings_write_error(
                "could not read settings file",
                settings_path.string()));
        }
        const auto text = content.str();
        if (!text.empty()) {
            if (auto parsed = util::read_json<util::JsonValue>(text); !parsed) {
                return std::unexpected(settings_write_error(
                    "could not update malformed settings file",
                    settings_path.string()));
            } else {
                const auto* parsed_object = parsed->get_if<util::JsonValue::object_t>();
                if (parsed_object == nullptr) {
                    return std::unexpected(settings_write_error(
                        "could not update settings file",
                        "settings root must be a JSON object"));
                }
                object = *parsed_object;
            }
        }
    }
    object.insert_or_assign("theme", util::JsonValue{std::string(theme_name)});

    return write_settings_object(settings_path, std::move(object));
}

std::optional<std::string> SettingsLoader::resolve_api_key(
    const std::vector<std::string>& env_chain) {
    for (const auto& env_name : env_chain) {
        const char* value = std::getenv(env_name.c_str());
        if (value != nullptr && value[0] != '\0') {
            return std::string(value);
        }
    }
    return std::nullopt;
}

} // namespace cch::coding_agent
