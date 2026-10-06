// MCP server configuration file (spec #865, ticket #876). pi persists MCP
// servers in a separate `mcp.json` (global `<agentDir>/mcp.json`, trusted
// project `<cwd>/.pi/mcp.json`) using the `mcpServers` shape shared by other
// MCP clients, not in `settings.json`; this loader is the read half of that
// persistence. The write half (pi's `/mcp` manager) is a later slice: a user
// authors `mcp.json` and the list survives restarts because assembly reloads
// it. entries and validation errors stay private to `cch_coding_agent`.

#include "coding_agent/mcp/McpConfigFile.hpp"

#include "support/Json.hpp"

#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::mcp {
namespace {

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

/// pi `SERVER_NAME`: `^[A-Za-z0-9_-]+$`.
[[nodiscard]] bool valid_server_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const char character : name) {
        const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

/// pi `mcpNamespace`: `mcp__<server>` with `-` replaced by `_`. Two names that
/// differ only in `-`/`_` share a tool namespace and cannot coexist.
[[nodiscard]] std::string server_namespace(std::string_view name) {
    std::string result = "mcp__";
    result.reserve(name.size() + 5);
    for (const char character : name) {
        result.push_back(character == '-' ? '_' : character);
    }
    return result;
}

/// Read a config file's text. `std::nullopt` when the file is missing or
/// unreadable.
[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return contents.str();
}

[[nodiscard]] std::string entry_error(const std::filesystem::path& path, const std::string& name, std::string message) {
    return path.string() + ": server \"" + name + "\": " + std::move(message);
}

/// A required non-empty string member. `std::nullopt` when present and valid.
[[nodiscard]] std::optional<std::string> read_required_string(
        const JsonObject& object, std::string_view key, std::string& out) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end()) {
        return "\"" + std::string{key} + "\" is required";
    }
    if (!entry->second.holds<std::string>()) {
        return "\"" + std::string{key} + "\" must be a string";
    }
    out = entry->second.get_string();
    if (out.empty()) {
        return "\"" + std::string{key} + "\" must not be empty";
    }
    return std::nullopt;
}

/// An optional string-map member (`env`/`headers`). `std::nullopt` when absent
/// or valid; the parsed map is written to `out` only when present.
[[nodiscard]] std::optional<std::string> read_optional_string_map(
        const JsonObject& object, std::string_view key, std::map<std::string, std::string>& out) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* map = entry->second.get_if<JsonObject>();
    if (map == nullptr) {
        return "\"" + std::string{key} + "\" must be an object of strings";
    }
    for (const auto& [name, value] : *map) {
        if (!value.holds<std::string>()) {
            return "\"" + std::string{key} + "." + name + "\" must be a string";
        }
        out.emplace(name, value.get_string());
    }
    return std::nullopt;
}

/// An optional array-of-strings member (`args`).
[[nodiscard]] std::optional<std::string> read_optional_string_array(
        const JsonObject& object, std::string_view key, std::vector<std::string>& out) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* array = entry->second.get_if<JsonArray>();
    if (array == nullptr) {
        return "\"" + std::string{key} + "\" must be an array of strings";
    }
    for (const auto& element : *array) {
        if (!element.holds<std::string>()) {
            return "\"" + std::string{key} + "\" must be an array of strings";
        }
        out.push_back(element.get_string());
    }
    return std::nullopt;
}

/// The `enabled` flag (pi default true). `std::nullopt` when absent or valid.
[[nodiscard]] std::optional<std::string> read_optional_bool(const JsonObject& object, std::string_view key, bool& out) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* value = entry->second.get_if<bool>();
    if (value == nullptr) {
        return "\"" + std::string{key} + "\" must be a boolean";
    }
    out = *value;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_entry(const std::filesystem::path& path,
        const std::string& name,
        const JsonObject& object,
        bool has_command,
        bool has_url,
        bool has_type,
        McpConfigEntry& out) {
    std::optional<std::string> type;
    if (has_type) {
        if (auto error = read_required_string(object, "type", type.emplace())) {
            return entry_error(path, name, std::move(*error));
        }
        if (*type != "stdio" && *type != "http") {
            return entry_error(path, name, "\"type\" must be \"stdio\" or \"http\"");
        }
    }

    bool is_stdio = false;
    if (type.has_value()) {
        is_stdio = *type == "stdio";
    } else if (has_command && !has_url) {
        is_stdio = true;
    } else if (has_url && !has_command) {
        is_stdio = false;
    } else if (has_command && has_url) {
        return entry_error(path, name, "sets both \"command\" and \"url\"; set \"type\" to choose one");
    } else {
        return entry_error(path, name, "needs \"command\" or \"url\"");
    }
    if (is_stdio && !has_command) {
        return entry_error(path, name, "\"type\": \"stdio\" requires \"command\"");
    }
    if (!is_stdio && !has_url) {
        return entry_error(path, name, "\"type\": \"http\" requires \"url\"");
    }

    bool enabled = true;
    if (auto error = read_optional_bool(object, "enabled", enabled)) {
        return entry_error(path, name, std::move(*error));
    }

    McpConfigEntry entry;
    entry.name = name;
    entry.enabled = enabled;
    entry.source = path;
    if (is_stdio) {
        McpStdioServerConfig config;
        config.name = name;
        if (auto error = read_required_string(object, "command", config.command)) {
            return entry_error(path, name, std::move(*error));
        }
        if (auto error = read_optional_string_array(object, "args", config.args)) {
            return entry_error(path, name, std::move(*error));
        }
        if (auto error = read_optional_string_map(object, "env", config.env)) {
            return entry_error(path, name, std::move(*error));
        }
        entry.config = std::move(config);
    } else {
        McpHttpServerConfig config;
        config.name = name;
        if (auto error = read_required_string(object, "url", config.url)) {
            return entry_error(path, name, std::move(*error));
        }
        if (auto error = read_optional_string_map(object, "headers", config.headers)) {
            return entry_error(path, name, std::move(*error));
        }
        entry.config = std::move(config);
    }
    out = std::move(entry);
    return std::nullopt;
}

/// A project entry with no `command`/`url`/`type` (pi `isOverride`): it only
/// carries override keys and applies them to the global server with the same
/// name. Only `enabled` is in scope for this slice; pi's `exposure`/
/// `toolExposure` override keys are accepted and ignored, so a pi config using
/// a not-yet-supported exposure does not become unusable.
void apply_override(const std::filesystem::path& path,
        const std::string& name,
        const JsonObject& object,
        McpConfigLoad& load,
        const std::map<std::string, std::size_t, std::less<>>& index) {
    const auto found = index.find(name);
    if (found == index.end()) {
        load.errors.push_back(path.string() + ": server \"" + name +
                              "\" needs \"command\" or \"url\", or a global server to override");
        return;
    }
    for (const auto& [key, unused] : object) {
        static_cast<void>(unused);
        if (key != "enabled" && key != "exposure" && key != "toolExposure") {
            load.errors.push_back(path.string() + ": server \"" + name +
                                  "\": an override can only set enabled, exposure, or toolExposure");
            return;
        }
    }
    bool enabled = load.servers[found->second].enabled;
    if (auto error = read_optional_bool(object, "enabled", enabled)) {
        load.errors.push_back(entry_error(path, name, std::move(*error)));
        return;
    }
    load.servers[found->second].enabled = enabled;
}

void read_config_file(const std::filesystem::path& path,
        bool project,
        McpConfigLoad& load,
        std::map<std::string, std::size_t, std::less<>>& index) {
    const auto content = read_text_file(path);
    if (!content) {
        return;
    }
    auto parsed = support::read_json(*content);
    if (!parsed) {
        load.errors.push_back(path.string() + ": contains invalid JSON");
        return;
    }
    const auto* root = parsed->get_if<JsonObject>();
    if (root == nullptr) {
        load.errors.push_back(path.string() + ": expected an object with an \"mcpServers\" object");
        return;
    }
    const auto servers_entry = root->find("mcpServers");
    if (servers_entry == root->end()) {
        return;
    }
    const auto* servers = servers_entry->second.get_if<JsonObject>();
    if (servers == nullptr) {
        load.errors.push_back(path.string() + ": expected an object with an \"mcpServers\" object");
        return;
    }
    for (const auto& [name, value] : *servers) {
        if (!valid_server_name(name)) {
            load.errors.push_back(path.string() + ": server name \"" + name + "\" is invalid");
            continue;
        }
        const auto* object = value.get_if<JsonObject>();
        if (object == nullptr) {
            load.errors.push_back(path.string() + ": server \"" + name + "\" must be an object");
            continue;
        }
        const bool has_command = object->contains("command");
        const bool has_url = object->contains("url");
        const bool has_type = object->contains("type");
        if (project && !has_command && !has_url && !has_type) {
            apply_override(path, name, *object, load, index);
            continue;
        }
        McpConfigEntry entry;
        if (auto error = parse_entry(path, name, *object, has_command, has_url, has_type, entry)) {
            load.errors.push_back(std::move(*error));
            continue;
        }
        bool clash = false;
        for (const auto& existing : load.servers) {
            if (existing.name != name && server_namespace(existing.name) == server_namespace(name)) {
                load.errors.push_back(
                        path.string() + ": server \"" + name + "\" conflicts with \"" + existing.name + "\"");
                clash = true;
                break;
            }
        }
        if (clash) {
            continue;
        }
        if (const auto existing = index.find(name); existing != index.end()) {
            load.servers[existing->second] = std::move(entry);
        } else {
            index.emplace(name, load.servers.size());
            load.servers.push_back(std::move(entry));
        }
    }
}

} // namespace

McpConfigLoad load_mcp_config(
        const std::filesystem::path& agent_dir, const std::filesystem::path& cwd, bool project_trusted) {
    McpConfigLoad load;
    std::map<std::string, std::size_t, std::less<>> index;
    if (!agent_dir.empty()) {
        read_config_file(agent_dir / "mcp.json", /* project */ false, load, index);
    }
    if (project_trusted && !cwd.empty()) {
        const auto project_path = cwd / kMcpProjectConfigDir / "mcp.json";
        std::error_code exists_error;
        if (std::filesystem::exists(project_path, exists_error) && !exists_error) {
            load.project_config = project_path;
        }
        read_config_file(project_path, /* project */ true, load, index);
    }
    return load;
}

} // namespace cch::coding_agent::mcp
