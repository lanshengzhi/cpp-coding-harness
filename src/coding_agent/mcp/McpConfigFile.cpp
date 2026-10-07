// MCP server configuration file (spec #865, ticket #876; exposure policy and
// pi validation spec #882, ticket #884). pi persists MCP servers in a separate
// `mcp.json` (global `<agentDir>/mcp.json`, trusted project
// `<cwd>/.pi/mcp.json`) using the `mcpServers` shape shared by other MCP
// clients, not in `settings.json`; this loader is the read half of that
// persistence. The write half (pi's `/mcp` manager) is a later slice: a user
// authors `mcp.json` and the list survives restarts because assembly reloads
// it. Entries and validation errors stay private to `cch_coding_agent`.
//
// pi source at `7c10bd43` (v1.0.4): `packages/coding-agent/src/extensions/mcp/
// config.ts` (`loadMcpConfig`, `readConfigFile`) and
// `packages/coding-agent/src/core/mcp-servers.ts` (`validateMcpServerConfig`).
// The validation messages are matched verbatim so a differential test can diff
// them against the frozen pi-v1.0.4 bundle.

#include "coding_agent/mcp/McpConfigFile.hpp"

#include "coding_agent/mcp/McpNamespace.hpp"

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

/// pi `URL.canParse(value) && /^https?:$/.test(new URL(value).protocol)`: an
/// absolute `http`/`https` URL with a host. The transport later enforces
/// TLS-only (ADR 0054); this gate only matches pi's config acceptance.
[[nodiscard]] bool is_http_url(std::string_view value) {
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos) {
        return false;
    }
    const std::string_view scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    const std::string_view rest = value.substr(scheme_end + 3);
    return !rest.empty() && rest.find_first_of("/?#") != 0;
}

[[nodiscard]] support::Error config_error(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
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

/// pi's `enabled` check (`validateMcpServerConfig`): absent or a boolean.
[[nodiscard]] std::optional<std::string> read_enabled(const JsonObject& object, bool& out) {
    const auto entry = object.find("enabled");
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* value = entry->second.get_if<bool>();
    if (value == nullptr) {
        return "enabled must be a boolean";
    }
    out = *value;
    return std::nullopt;
}

/// pi's `args` check: absent or an array of strings.
[[nodiscard]] std::optional<std::string> read_args(const JsonObject& object, std::vector<std::string>& out) {
    const auto entry = object.find("args");
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* array = entry->second.get_if<JsonArray>();
    if (array == nullptr) {
        return "args must be an array of strings";
    }
    for (const auto& element : *array) {
        if (!element.holds<std::string>()) {
            return "args must be an array of strings";
        }
        out.push_back(element.get_string());
    }
    return std::nullopt;
}

/// pi's `env`/`headers` check: absent or an object of strings.
[[nodiscard]] std::optional<std::string> read_string_map(
        const JsonObject& object, std::string_view key, std::map<std::string, std::string>& out) {
    const auto entry = object.find(std::string{key});
    if (entry == object.end()) {
        return std::nullopt;
    }
    const auto* map = entry->second.get_if<JsonObject>();
    if (map == nullptr) {
        return std::string{key} + " must map names to strings";
    }
    for (const auto& [name, value] : *map) {
        if (!value.holds<std::string>()) {
            return std::string{key} + " must map names to strings";
        }
        out.emplace(name, value.get_string());
    }
    return std::nullopt;
}

/// pi `toolExposure`: an object mapping tool names/patterns to exposures
/// (aliases resolved), in declaration order.
[[nodiscard]] support::Expected<std::vector<std::pair<std::string, McpExposure>>> read_tool_exposure(
        const std::string& server, const JsonObject& object) {
    const auto entry = object.find("toolExposure");
    if (entry == object.end()) {
        return std::vector<std::pair<std::string, McpExposure>>{};
    }
    const auto* map = entry->second.get_if<JsonObject>();
    if (map == nullptr) {
        return std::unexpected(config_error(
                "server \"" + server + "\": toolExposure must map tool names to exposures"));
    }
    std::vector<std::pair<std::string, McpExposure>> overrides;
    for (const auto& [tool, value] : *map) {
        std::optional<McpExposure> parsed;
        if (value.holds<std::string>()) {
            parsed = parse_mcp_exposure(value.get_string());
        }
        if (!parsed) {
            return std::unexpected(config_error("server \"" + server + "\": toolExposure \"" + tool +
                                                "\" must be one of " + mcp_exposure_list()));
        }
        overrides.emplace_back(tool, *parsed);
    }
    return overrides;
}

/// Apply the exposure/description/timeout half of a validated entry onto
/// `base` (the transport-independent fields).
void apply_base(McpServerConfigBase& base,
        std::optional<McpExposure> exposure,
        std::optional<std::string> description,
        std::vector<std::pair<std::string, McpExposure>> tool_exposure,
        std::optional<double> timeout) {
    base.exposure = exposure;
    base.description = std::move(description);
    base.tool_exposure = std::move(tool_exposure);
    base.timeout = timeout;
}

} // namespace

support::Expected<McpServerConfigVariant> validate_mcp_server_config(
        std::string_view name, const support::JsonValue& raw) {
    const std::string server{name};
    if (!valid_server_name(name)) {
        return std::unexpected(config_error(
                "invalid server name \"" + server + "\" (use letters, digits, \"_\" and \"-\")"));
    }
    const auto* object = raw.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(config_error("server \"" + server + "\" must be an object"));
    }

    // `exposure` (alias-resolved), `toolExposure`, `description`, `timeout`,
    // and `enabled` are validated before the transport shape.
    std::optional<McpExposure> exposure;
    if (const auto entry = object->find("exposure"); entry != object->end()) {
        if (!entry->second.holds<std::string>()) {
            return std::unexpected(
                    config_error("server \"" + server + "\": exposure must be one of " + mcp_exposure_list()));
        }
        exposure = parse_mcp_exposure(entry->second.get_string());
        if (!exposure) {
            return std::unexpected(
                    config_error("server \"" + server + "\": exposure must be one of " + mcp_exposure_list()));
        }
    }

    auto tool_exposure = read_tool_exposure(server, *object);
    if (!tool_exposure) {
        return std::unexpected(std::move(tool_exposure.error()));
    }

    // pi validates `enabled` before `description`/`timeout`; the value itself is
    // read by `read_config_file` into `McpConfigEntry.enabled`.
    if (const auto entry = object->find("enabled"); entry != object->end() && !entry->second.holds<bool>()) {
        return std::unexpected(config_error("server \"" + server + "\": enabled must be a boolean"));
    }

    std::optional<std::string> description;
    if (const auto entry = object->find("description"); entry != object->end()) {
        if (!entry->second.holds<std::string>()) {
            return std::unexpected(config_error("server \"" + server + "\": description must be a string"));
        }
        description = entry->second.get_string();
    }

    std::optional<double> timeout;
    if (const auto entry = object->find("timeout"); entry != object->end()) {
        if (!entry->second.holds<double>() || !(entry->second.get_number() > 0)) {
            return std::unexpected(
                    config_error("server \"" + server + "\": timeout must be a positive number of seconds"));
        }
        timeout = entry->second.get_number();
    }

    std::string type;
    if (const auto entry = object->find("type"); entry != object->end()) {
        if (!entry->second.holds<std::string>()) {
            return std::unexpected(config_error("server \"" + server + "\": type must be a string"));
        }
        type = entry->second.get_string();
    }
    if (type == "sse") {
        return std::unexpected(config_error(
                "server \"" + server + "\": legacy SSE transport is not supported; use the streamable HTTP URL"));
    }

    const auto url_entry = object->find("url");
    const bool has_url = url_entry != object->end() && url_entry->second.holds<std::string>();
    const auto command_entry = object->find("command");
    const bool has_command = command_entry != object->end() && command_entry->second.holds<std::string>();

    if (has_url && (type.empty() || type == "http" || type == "streamable-http")) {
        McpHttpServerConfig config;
        config.name = server;
        apply_base(config, exposure, std::move(description), std::move(*tool_exposure), timeout);
        config.url = url_entry->second.get_string();
        if (!is_http_url(config.url)) {
            return std::unexpected(config_error("server \"" + server + "\": url must be an http or https URL"));
        }
        if (auto error = read_string_map(*object, "headers", config.headers)) {
            return std::unexpected(config_error("server \"" + server + "\": " + *error));
        }
        return McpServerConfigVariant{std::move(config)};
    }
    if (has_command && (type.empty() || type == "stdio")) {
        McpStdioServerConfig config;
        config.name = server;
        apply_base(config, exposure, std::move(description), std::move(*tool_exposure), timeout);
        config.command = command_entry->second.get_string();
        if (auto error = read_args(*object, config.args)) {
            return std::unexpected(config_error("server \"" + server + "\": " + *error));
        }
        if (auto error = read_string_map(*object, "env", config.env)) {
            return std::unexpected(config_error("server \"" + server + "\": " + *error));
        }
        return McpServerConfigVariant{std::move(config)};
    }
    return std::unexpected(config_error(
            "server \"" + server + "\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)"));
}

namespace {

/// One project entry with no `command`/`url`/`type` (pi `isOverride`): it only
/// carries override keys and applies them to the global server with the same
/// name (pi `{ ...base.config, ...value }` re-validated).
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

    McpConfigEntry& base = load.servers[found->second];
    if (const auto entry = object.find("enabled"); entry != object.end()) {
        const auto* value = entry->second.get_if<bool>();
        if (value == nullptr) {
            load.errors.push_back(path.string() + ": server \"" + name + "\": enabled must be a boolean");
            return;
        }
        base.enabled = *value;
    }
    if (const auto entry = object.find("exposure"); entry != object.end()) {
        std::optional<McpExposure> parsed;
        if (entry->second.holds<std::string>()) {
            parsed = parse_mcp_exposure(entry->second.get_string());
        }
        if (!parsed) {
            load.errors.push_back(
                    path.string() + ": server \"" + name + "\": exposure must be one of " + mcp_exposure_list());
            return;
        }
        std::visit([&parsed](auto& config) { config.exposure = parsed; }, base.config);
    }
    if (object.contains("toolExposure")) {
        auto overrides = read_tool_exposure(name, object);
        if (!overrides) {
            load.errors.push_back(path.string() + ": " + overrides.error().message);
            return;
        }
        std::visit([&overrides](auto& config) { config.tool_exposure = std::move(*overrides); }, base.config);
    }
    base.override = path;
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
    if (const auto auto_enable = root->find("autoEnableCodemode"); auto_enable != root->end()) {
        if (const auto* value = auto_enable->second.get_if<bool>()) {
            load.auto_enable_codemode = *value;
        } else {
            load.errors.push_back(path.string() + ": autoEnableCodemode must be a boolean");
        }
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
        const auto* object = value.get_if<JsonObject>();
        const bool has_command = object != nullptr && object->contains("command");
        const bool has_url = object != nullptr && object->contains("url");
        const bool has_type = object != nullptr && object->contains("type");
        if (project && object != nullptr && !has_command && !has_url && !has_type) {
            apply_override(path, name, *object, load, index);
            continue;
        }
        auto validated = validate_mcp_server_config(name, value);
        if (!validated) {
            load.errors.push_back(path.string() + ": " + validated.error().message);
            continue;
        }
        McpConfigEntry entry;
        entry.name = name;
        entry.source = path;
        entry.config = std::move(*validated);
        bool enabled = true;
        if (object != nullptr) {
            static_cast<void>(read_enabled(*object, enabled));
        }
        entry.enabled = enabled;
        bool clash = false;
        for (const auto& existing : load.servers) {
            if (existing.name != name && detail::mcp_namespace(existing.name) == detail::mcp_namespace(name)) {
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
