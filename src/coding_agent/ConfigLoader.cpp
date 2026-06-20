#include "../../include/cch/coding_agent/Config.hpp"
#include "../../include/cch/util/Json.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace cch::coding_agent {

util::Expected<ConfigData> ConfigLoader::load(const std::string& config_path) {
    ConfigData config;

    std::ifstream file(config_path);
    if (!file.is_open()) {
        // File does not exist or is unreadable — not an error; use defaults.
        return config;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (content.empty()) {
        return config;
    }

    auto json = util::read_json<util::JsonValue>(content);
    if (!json) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "failed to parse config file",
            "config file at '" + config_path + "' contains invalid JSON"));
    }

    // Read known fields; ignore unknown keys for forward compatibility.
    if (json->holds<util::JsonValue::object_t>()) {
        auto& obj = json->get_object();
        if (auto it = obj.find("provider"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                config.provider = *str;
            }
        }
        if (auto it = obj.find("model"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                config.model = *str;
            }
        }
        if (auto it = obj.find("base_url"); it != obj.end()) {
            if (auto* str = it->second.get_if<std::string>()) {
                config.base_url = *str;
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
                    config.api_key_env = std::move(chain);
                }
            } else if (auto* str = it->second.get_if<std::string>()) {
                // Single string → single-element chain
                std::vector<std::string> chain;
                chain.push_back(*str);
                config.api_key_env = std::move(chain);
            }
        }
    }

    return config;
}

std::optional<std::string> ConfigLoader::resolve_api_key(
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
