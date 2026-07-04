#include "../../include/cch/coding_agent/AuthLoader.hpp"

#include "../../include/cch/util/JsonValue.hpp"
#include "../util/Json.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace cch::coding_agent {

std::string AuthLoader::default_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.cpp-harness/agent/auth.json" : "";
}

util::Expected<std::map<std::string, AuthEntry>> AuthLoader::load(const std::string& path) {
    std::map<std::string, AuthEntry> entries;
    if (path.empty()) {
        return entries;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return entries;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    if (content.empty()) {
        return entries;
    }

    auto json = util::read_json<util::JsonValue>(content);
    if (!json) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "failed to parse auth file",
            "auth file at '" + path + "' contains invalid JSON"));
    }

    const auto* object = json->get_if<util::JsonValue::object_t>();
    if (object == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::JsonParse,
            "auth file is not a JSON object",
            "auth file root must be an object"));
    }

    for (const auto& [name, value] : *object) {
        const auto* entry_object = value.get_if<util::JsonValue::object_t>();
        if (entry_object == nullptr) {
            continue;
        }

        AuthEntry entry;
        if (auto it = entry_object->find("type"); it != entry_object->end()) {
            if (const auto* str = it->second.get_if<std::string>()) {
                entry.type = *str;
            }
        }
        if (auto it = entry_object->find("key"); it != entry_object->end()) {
            if (const auto* str = it->second.get_if<std::string>()) {
                entry.key = *str;
            }
        }

        if (!entry.key.empty()) {
            entries.emplace(name, std::move(entry));
        }
    }

    return entries;
}

} // namespace cch::coding_agent
