#include "../../include/cch/coding_agent/Settings.hpp"
#include "util/Json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace cch::coding_agent {

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
        }
    }

    return settings;
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
