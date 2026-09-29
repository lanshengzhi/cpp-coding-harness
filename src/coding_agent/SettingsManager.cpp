#include <cch/coding_agent/Settings.hpp>

#include "PrettyJson.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/stat.h>

namespace cch::coding_agent {
namespace {

using JsonObject = support::JsonValue::object_t;

constexpr std::string_view kProjectConfigDir = ".pi";

// Upstream MCP Server configuration (issue #835, spec #833 stories 1, 2, 3, 5,
// 6). The Server Id is the map key and the sole stable identity for
// namespacing, credentials, trust, and status: `[A-Za-z0-9_-]`, at most 48
// characters, enforced here at config validation.
constexpr std::size_t kMcpServerIdMaxLength = 48;
/// The only credential form configuration accepts: an environment-variable
/// reference, never a literal value.
constexpr std::string_view kBearerEnvPrefix = "bearer-env:";
/// The only request header configuration accepts, and only as a
/// `bearer-env:<VAR>` reference.
constexpr std::string_view kMcpAuthorizationHeader = "Authorization";

// proper-lockfile-compatible lock parameters (pi FileSettingsStorage): 10
// attempts, 20 ms between attempts, stale lock reclaimed after 30 s.
constexpr int kLockMaxAttempts = 10;
constexpr auto kLockPollInterval = std::chrono::milliseconds{20};
constexpr auto kLockStaleAfter = std::chrono::seconds{30};

[[nodiscard]] support::Error settings_error(
    std::string message,
    std::string detail = {}) {
    return support::make_error(
        support::ErrorCode::Workspace,
        std::move(message),
        std::move(detail));
}

[[nodiscard]] support::Error settings_file_error(
    std::string message,
    const std::filesystem::path& path,
    std::string detail = {}) {
    if (detail.empty()) {
        detail = message;
    }
    return support::make_error(
        support::ErrorCode::JsonParse,
        std::move(message),
        std::move(detail),
        path.string());
}

[[nodiscard]] std::string settings_error_text(const support::Error& error) {
    if (error.detail.empty() || error.detail == error.message) {
        return error.message;
    }
    return error.message + ": " + error.detail;
}

// ─────────────────────────────────────────────────────────────────────────────
// pi read-time migrations (SettingsManager.migrateSettings). None of the
// migrated fields are consumed by this subset, but the write path must migrate
// the current file before merging so a hand-edited or pi-written file round-
// trips losslessly. There are no C++-private migrations (ADR 0031).
// ─────────────────────────────────────────────────────────────────────────────

void migrate_settings(JsonObject& settings) {
    if (settings.contains("queueMode") && !settings.contains("steeringMode")) {
        settings["steeringMode"] = settings["queueMode"];
        settings.erase("queueMode");
    }

    if (!settings.contains("transport")) {
        const auto websockets = settings.find("websockets");
        if (websockets != settings.end()) {
            if (const auto* enabled = websockets->second.get_if<bool>()) {
                settings["transport"] = support::JsonValue{
                    *enabled ? std::string{"websocket"} : std::string{"sse"}};
            }
            settings.erase(websockets);
        }
    }

    const auto skills = settings.find("skills");
    if (skills != settings.end() && !skills->second.holds<support::JsonValue::array_t>()) {
        if (const auto* skills_object = skills->second.get_if<JsonObject>()) {
            const auto enable_commands = skills_object->find("enableSkillCommands");
            if (enable_commands != skills_object->end() &&
                !settings.contains("enableSkillCommands")) {
                settings["enableSkillCommands"] = enable_commands->second;
            }
            const auto custom = skills_object->find("customDirectories");
            if (custom != skills_object->end() &&
                custom->second.holds<support::JsonValue::array_t>() &&
                !custom->second.get_array().empty()) {
                settings["skills"] = custom->second;
            } else {
                settings.erase(skills);
            }
        }
    }

    const auto retry = settings.find("retry");
    if (retry != settings.end()) {
        if (const auto* retry_object = retry->second.get_if<JsonObject>()) {
            auto mutated = *retry_object;
            const auto provider = mutated.find("provider");
            support::JsonValue::object_t* provider_object = nullptr;
            JsonObject merged_provider;
            if (provider != mutated.end()) {
                if (auto* provided = provider->second.get_if<JsonObject>()) {
                    merged_provider = *provided;
                    provider_object = &merged_provider;
                }
            }
            const auto max_delay = mutated.find("maxDelayMs");
            const bool provider_missing_delay =
                provider_object == nullptr ||
                !provider_object->contains("maxRetryDelayMs") ||
                provider_object->at("maxRetryDelayMs").holds<support::JsonValue::null_t>();
            if (max_delay != mutated.end() &&
                max_delay->second.holds<double>() &&
                provider_missing_delay) {
                JsonObject configured_provider = std::move(merged_provider);
                configured_provider["maxRetryDelayMs"] = max_delay->second;
                mutated["provider"] = support::JsonValue{std::move(configured_provider)};
            }
            mutated.erase("maxDelayMs");
            retry->second = support::JsonValue{std::move(mutated)};
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Field parsing
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] const std::string* string_field(
    const JsonObject& object,
    std::string_view name) {
    const auto found = object.find(std::string{name});
    return found == object.end() ? nullptr : found->second.get_if<std::string>();
}

[[nodiscard]] bool is_thinking_level(std::string_view value) {
    return value == "off" || value == "minimal" || value == "low" ||
           value == "medium" || value == "high" || value == "xhigh" ||
           value == "max";
}

// ─────────────────────────────────────────────────────────────────────────────
// `mcpServers` parsing and fail-closed validation (issue #835)
//
// Every diagnostic below names the offending field and, for a Server Id that
// passed validation, the Server Id itself. A value the validator rejects — an
// inline secret above all — never reaches a diagnostic: the rejected text is
// the thing being reported about, so echoing it would leak the very secret the
// rule exists to keep out of the config. A Server Id that failed validation is
// not echoed either, for the same reason: an unvalidated key is arbitrary
// text. Invalid Server Ids are located by their position in the map instead.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] bool is_mcp_server_id_char(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
}

[[nodiscard]] bool is_environment_variable_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto character = name[index];
        const bool leading =
                (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
        const bool trailing = leading || (character >= '0' && character <= '9');
        if (!(index == 0 ? leading : trailing)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_ascii_equal_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_character = left[index];
        auto right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] support::Error mcp_servers_error(std::string message, std::string detail) {
    return settings_file_error(std::move(message), {}, std::move(detail));
}

/// Validate a `bearer-env:<VAR>` credential reference and return the variable
/// name. Anything else — a literal token, a bare variable name, an empty
/// prefix — is rejected, and the rejected value is never echoed.
[[nodiscard]] support::Expected<std::string> parse_bearer_env_reference(
        const std::string& field, const std::string& value) {
    if (!value.starts_with(kBearerEnvPrefix)) {
        return std::unexpected(mcp_servers_error("invalid mcpServers credential",
                field + " must be an environment-variable reference of the form bearer-env:<VAR>; "
                        "inline secrets are rejected and the value is not shown"));
    }
    const auto name = std::string_view{value}.substr(kBearerEnvPrefix.size());
    if (!is_environment_variable_name(name)) {
        return std::unexpected(mcp_servers_error("invalid mcpServers credential",
                field + " must name an environment variable as bearer-env:<VAR>, "
                        "where <VAR> matches [A-Za-z_][A-Za-z0-9_]*"));
    }
    return std::string{name};
}

/// Reject a URL that carries credential material inline. A `user:password@`
/// userinfo section is a literal secret, so the whole entry is refused; the
/// credential belongs in a `bearer-env:<VAR>` reference.
[[nodiscard]] bool mcp_url_carries_userinfo(std::string_view url) {
    const auto authority_start = url.find("://");
    if (authority_start == std::string_view::npos) {
        return false;
    }
    const auto authority = url.substr(authority_start + 3);
    const auto path_start = authority.find_first_of("/?#");
    return authority.substr(0, path_start).find('@') != std::string_view::npos;
}

[[nodiscard]] support::Expected<UserMcpServerSettings> parse_mcp_server_settings(
        const std::string& server_id, const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(
                mcp_servers_error("invalid mcpServers entry", "mcpServers." + server_id + " must be an object"));
    }
    const std::string prefix = "mcpServers." + server_id + ".";

    // stdio is pre-cut but not implemented (ADR 0064): the fields parse, then
    // validation refuses the entry. Nothing is spawned on this path — the
    // entry never reaches an assembled server.
    static constexpr std::string_view kStdioFields[] = {"command", "args", "env"};
    for (const auto field : kStdioFields) {
        if (object->contains(std::string{field})) {
            return std::unexpected(mcp_servers_error("unsupported mcpServers transport",
                    prefix + std::string{field} +
                            " is a Deferred Capability: the stdio transport and "
                            "the Legacy Era adapter are not supported yet, so no process is started"));
        }
    }

    const auto url_field = string_field(*object, "url");
    if (url_field == nullptr || url_field->empty()) {
        return std::unexpected(mcp_servers_error(
                "invalid mcpServers entry", prefix + "url is required and must be a non-empty string"));
    }
    const std::string_view url{*url_field};
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return std::unexpected(mcp_servers_error(
                "invalid mcpServers entry", prefix + "url must be an http:// or https:// Streamable HTTP endpoint"));
    }
    if (mcp_url_carries_userinfo(url)) {
        return std::unexpected(mcp_servers_error("invalid mcpServers credential",
                prefix + "url must not carry userinfo; declare the credential as "
                         "bearer-env:<VAR> instead, and the value is not shown"));
    }

    UserMcpServerSettings server;
    server.server_id = server_id;
    server.url = *url_field;

    // `auth` and `headers.Authorization` are two spellings of one credential
    // reference; declaring both is ambiguous, so it is refused rather than
    // resolved by precedence.
    // A mistyped `auth` is refused rather than read as "no credential
    // configured": a field this validator cannot read is not a field it may
    // silently drop.
    const auto auth_entry = object->find("auth");
    const auto* auth_field = auth_entry == object->end() ? nullptr : auth_entry->second.get_if<std::string>();
    if (auth_entry != object->end() && auth_field == nullptr) {
        return std::unexpected(mcp_servers_error(
                "invalid mcpServers credential", prefix + "auth must be a string; the value is not shown"));
    }
    const auto headers_field = object->find("headers");
    const JsonObject* headers = headers_field == object->end() ? nullptr : headers_field->second.get_if<JsonObject>();
    if (headers_field != object->end() && headers == nullptr) {
        return std::unexpected(mcp_servers_error("invalid mcpServers entry", prefix + "headers must be an object"));
    }
    if (auth_field != nullptr && headers != nullptr) {
        return std::unexpected(mcp_servers_error("invalid mcpServers credential",
                prefix + "declares both auth and headers; declare the credential reference once"));
    }
    if (auth_field != nullptr) {
        auto reference = parse_bearer_env_reference(prefix + "auth", *auth_field);
        if (!reference) {
            return std::unexpected(reference.error());
        }
        server.bearer_env_var = std::move(*reference);
    }
    if (headers != nullptr) {
        for (const auto& [name, header_value] : *headers) {
            if (!is_ascii_equal_ignore_case(name, kMcpAuthorizationHeader)) {
                return std::unexpected(mcp_servers_error("invalid mcpServers headers",
                        prefix + "headers." + name +
                                " is not supported; the only header configuration "
                                "accepts is " +
                                std::string{kMcpAuthorizationHeader} + ": bearer-env:<VAR>"));
            }
            const auto* reference = header_value.get_if<std::string>();
            if (reference == nullptr) {
                return std::unexpected(mcp_servers_error("invalid mcpServers credential",
                        prefix + "headers.Authorization must be a string; the value is not shown"));
            }
            auto parsed = parse_bearer_env_reference(prefix + "headers.Authorization", *reference);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            server.bearer_env_var = std::move(*parsed);
        }
    }

    if (const auto found = object->find("activation"); found != object->end()) {
        const auto* value_text = found->second.get_if<std::string>();
        if (value_text == nullptr) {
            return std::unexpected(mcp_servers_error(
                    "invalid mcpServers activation", prefix + "activation must be a string: lazy or eager"));
        }
        if (*value_text == "lazy") {
            server.activation = McpServerActivation::Lazy;
        } else if (*value_text == "eager") {
            server.activation = McpServerActivation::Eager;
        } else {
            return std::unexpected(mcp_servers_error(
                    "invalid mcpServers activation", prefix + "activation must be one of: lazy, eager"));
        }
    }
    if (const auto found = object->find("approval"); found != object->end()) {
        const auto* value_text = found->second.get_if<std::string>();
        if (value_text == nullptr) {
            return std::unexpected(mcp_servers_error(
                    "invalid mcpServers approval", prefix + "approval must be a string: allow or ask"));
        }
        if (*value_text == "allow") {
            server.approval = McpServerApproval::Allow;
        } else if (*value_text == "ask") {
            server.approval = McpServerApproval::Ask;
        } else {
            return std::unexpected(
                    mcp_servers_error("invalid mcpServers approval", prefix + "approval must be one of: allow, ask"));
        }
    }
    return server;
}

/// Parse the `mcpServers` map. Validation is fail-closed: one invalid entry
/// fails the whole scope, matching how an invalid `defaultThinkingLevel`
/// behaves, so a rejected configuration never reaches the runtime in part.
[[nodiscard]] support::Expected<std::vector<UserMcpServerSettings>> parse_mcp_servers(const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(
                mcp_servers_error("invalid mcpServers", "mcpServers must be an object keyed by Server Id"));
    }
    std::vector<UserMcpServerSettings> servers;
    servers.reserve(object->size());
    std::size_t position = 0;
    for (const auto& [server_id, entry] : *object) {
        // An unvalidated key is arbitrary text, so it is reported by position
        // and by rule, never echoed.
        if (!is_valid_mcp_server_id(server_id)) {
            return std::unexpected(mcp_servers_error("invalid mcpServers Server Id",
                    "mcpServers entry " + std::to_string(position) +
                            " has a Server Id that must be 1-48 "
                            "characters of [A-Za-z0-9_-]; the key is not shown"));
        }
        auto parsed = parse_mcp_server_settings(server_id, entry);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        servers.push_back(std::move(*parsed));
        ++position;
    }
    return servers;
}

/// Parse pi's nested `compaction` object (`{enabled, reserveTokens,
/// keepRecentTokens}`). Each field is optional; unknown or mistyped fields
/// are ignored (a mistyped field falls back to the pi default at resolution,
/// mirroring pi's `settings.compaction?.enabled ?? true` reads).
[[nodiscard]] support::Expected<UserCompactionSettings> parse_compaction_settings(
    const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(settings_file_error(
            "invalid compaction",
            {},
            "compaction must be an object"));
    }
    UserCompactionSettings settings;
    const auto enabled = object->find("enabled");
    if (enabled != object->end()) {
        if (const auto* parsed = enabled->second.get_if<bool>()) {
            settings.enabled = *parsed;
        }
    }
    const auto reserve = object->find("reserveTokens");
    if (reserve != object->end()) {
        if (const auto* parsed = reserve->second.get_if<double>()) {
            if (*parsed >= 0) {
                settings.reserve_tokens =
                    static_cast<std::uint64_t>(*parsed);
            }
        }
    }
    const auto keep = object->find("keepRecentTokens");
    if (keep != object->end()) {
        if (const auto* parsed = keep->second.get_if<double>()) {
            if (*parsed >= 0) {
                settings.keep_recent_tokens =
                    static_cast<std::uint64_t>(*parsed);
            }
        }
    }
    return settings;
}

/// Parse pi's nested `retry` object (`{enabled, maxRetries, baseDelayMs}`).
/// Each field is optional; unknown or mistyped fields are ignored (a mistyped
/// field falls back to the pi default at resolution, mirroring pi's
/// `settings.retry?.enabled ?? true` reads).
[[nodiscard]] support::Expected<UserRetrySettings> parse_retry_settings(
    const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(settings_file_error(
            "invalid retry",
            {},
            "retry must be an object"));
    }
    UserRetrySettings settings;
    const auto enabled = object->find("enabled");
    if (enabled != object->end()) {
        if (const auto* parsed = enabled->second.get_if<bool>()) {
            settings.enabled = *parsed;
        }
    }
    const auto max_retries = object->find("maxRetries");
    if (max_retries != object->end()) {
        if (const auto* parsed = max_retries->second.get_if<double>()) {
            if (*parsed >= 0) {
                settings.max_retries =
                    static_cast<std::uint64_t>(*parsed);
            }
        }
    }
    const auto base_delay = object->find("baseDelayMs");
    if (base_delay != object->end()) {
        if (const auto* parsed = base_delay->second.get_if<double>()) {
            if (*parsed >= 0) {
                settings.base_delay_ms =
                    static_cast<std::uint64_t>(*parsed);
            }
        }
    }
    return settings;
}

/// Parse one scope into `UserSettings`. `allow_default_project_trust` is true
/// only for the global scope; a project-scope `defaultProjectTrust` is ignored
/// (global-only). Unknown fields are ignored for forward compatibility and
/// preserved by the surgical write path.
[[nodiscard]] support::Expected<UserSettings> parse_settings(
    const JsonObject& object,
    bool allow_default_project_trust) {
    UserSettings settings;

    if (const auto* value = string_field(object, "defaultProvider")) {
        settings.default_provider = *value;
    }
    if (const auto* value = string_field(object, "defaultModel")) {
        settings.default_model = *value;
    }
    if (const auto* value = string_field(object, "defaultThinkingLevel")) {
        if (!is_thinking_level(*value)) {
            return std::unexpected(settings_file_error(
                "invalid defaultThinkingLevel",
                {},
                "defaultThinkingLevel must be one of: off, minimal, low, medium, high, xhigh, max"));
        }
        settings.default_thinking_level = *value;
    }
    if (const auto found = object.find("enabledModels"); found != object.end()) {
        if (const auto* array = found->second.get_if<support::JsonValue::array_t>()) {
            std::vector<std::string> patterns;
            for (const auto& item : *array) {
                if (const auto* text = item.get_if<std::string>()) {
                    patterns.push_back(*text);
                }
            }
            if (!patterns.empty()) {
                settings.enabled_models = std::move(patterns);
            }
        }
    }
    if (const auto* value = string_field(object, "sessionDir")) {
        settings.session_dir = *value;
    }
    if (allow_default_project_trust) {
        if (const auto* value = string_field(object, "defaultProjectTrust")) {
            auto parsed = parse_default_project_trust(*value);
            if (!parsed) {
                return std::unexpected(settings_file_error(
                    "invalid defaultProjectTrust",
                    {},
                    "defaultProjectTrust must be one of: ask, always, never"));
            }
            settings.default_project_trust = *parsed;
        }
    }
    if (const auto* value = string_field(object, "shellPath")) {
        settings.shell_path = *value;
    }
    if (const auto* value = string_field(object, "shellCommandPrefix")) {
        settings.shell_command_prefix = *value;
    }
    if (const auto* value = string_field(object, "theme")) {
        settings.theme = *value;
    }
    if (const auto found = object.find("compaction"); found != object.end()) {
        if (auto parsed = parse_compaction_settings(found->second); parsed) {
            settings.compaction = std::move(*parsed);
        } else {
            return std::unexpected(parsed.error());
        }
    }
    if (const auto found = object.find("retry"); found != object.end()) {
        if (auto parsed = parse_retry_settings(found->second); parsed) {
            settings.retry = std::move(*parsed);
        } else {
            return std::unexpected(parsed.error());
        }
    }
    if (const auto found = object.find("hideThinkingBlock"); found != object.end()) {
        if (const auto* parsed = found->second.get_if<bool>()) {
            settings.hide_thinking_block = *parsed;
        }
    }
    if (const auto found = object.find("outputPad"); found != object.end()) {
        if (const auto* parsed = found->second.get_if<double>()) {
            // pi `getOutputPad`: `settings.outputPad === 0 ? 0 : 1` — only 0
            // resolves as 0; every other number resolves as 1.
            settings.output_pad =
                static_cast<std::size_t>(*parsed == 0 ? 0 : 1);
        }
    }
    if (const auto found = object.find("enableSkillCommands"); found != object.end()) {
        if (const auto* parsed = found->second.get_if<bool>()) {
            settings.enable_skill_commands = *parsed;
        }
    }
    if (const auto found = object.find("mcpServers"); found != object.end()) {
        auto parsed = parse_mcp_servers(found->second);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        settings.mcp_servers = std::move(*parsed);
    }
    return settings;
}

// ─────────────────────────────────────────────────────────────────────────────
// proper-lockfile-compatible lock (`<path>.lock` directory; ELOCKED retry)
// ─────────────────────────────────────────────────────────────────────────────

class FileLock {
public:
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock() {
        std::error_code error;
        std::filesystem::remove_all(lock_path_, error);
    }

    explicit FileLock(std::filesystem::path lock_path)
        : lock_path_(std::move(lock_path)) {}

    [[nodiscard]] static support::Expected<std::unique_ptr<FileLock>> acquire(
        const std::filesystem::path& target_path) {
        const auto lock_path = std::filesystem::path{target_path.string() + ".lock"};
        for (int attempt = 0; attempt < kLockMaxAttempts; ++attempt) {
            std::error_code error;
            if (std::filesystem::create_directory(lock_path, error)) {
                return std::make_unique<FileLock>(lock_path);
            }
            if (error && error != std::errc::file_exists) {
                return std::unexpected(settings_error(
                    "failed to acquire settings lock",
                    lock_path.string()));
            }
            // ELOCKED: reclaim a stale lock, otherwise retry.
            const auto modified = std::filesystem::last_write_time(lock_path, error);
            const bool stale =
                !error &&
                std::filesystem::file_time_type::clock::now() - modified > kLockStaleAfter;
            if (stale) {
                std::error_code remove_error;
                std::filesystem::remove_all(lock_path, remove_error);
                if (!remove_error) {
                    continue;
                }
            }
            if (attempt + 1 < kLockMaxAttempts) {
                std::this_thread::sleep_for(kLockPollInterval);
            }
        }
        return std::unexpected(settings_error(
            "settings file is locked",
            lock_path.string() + ": timed out acquiring the settings lock"));
    }

private:
    std::filesystem::path lock_path_;
};

[[nodiscard]] support::Expected<std::optional<std::string>> read_text_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::optional<std::string>{};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::unexpected(settings_error(
            "could not read settings file",
            path.string()));
    }
    return contents.str();
}

/// Load one scope: read, parse, and apply pi's read-time migrations.
[[nodiscard]] support::Expected<UserSettings> load_scope(
    const std::filesystem::path& path,
    bool allow_default_project_trust) {
    auto content = read_text_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (!*content || (*content)->empty()) {
        return UserSettings{};
    }
    auto parsed = support::read_json(**content);
    if (!parsed) {
        return std::unexpected(settings_file_error(
            "failed to parse settings file",
            path,
            "settings file contains invalid JSON"));
    }
    auto* parsed_object = parsed->get_if<JsonObject>();
    if (parsed_object == nullptr) {
        return std::unexpected(settings_file_error(
            "settings file is not a JSON object",
            path,
            "settings root must be an object"));
    }
    auto object = *parsed_object;
    migrate_settings(object);
    return parse_settings(object, allow_default_project_trust);
}

/// Read the current file content into a JSON object, applying pi's
/// migrations. Used by the surgical write path.
[[nodiscard]] support::Expected<JsonObject> read_current_settings(
    const std::filesystem::path& path) {
    JsonObject object;
    auto content = read_text_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (!*content || (*content)->empty()) {
        return object;
    }
    auto parsed = support::read_json(**content);
    if (!parsed) {
        return std::unexpected(settings_file_error(
            "could not update settings file",
            path,
            "settings file contains invalid JSON"));
    }
    auto* parsed_object = parsed->get_if<JsonObject>();
    if (parsed_object == nullptr) {
        return std::unexpected(settings_file_error(
            "could not update settings file",
            path,
            "settings root must be a JSON object"));
    }
    object = *parsed_object;
    migrate_settings(object);
    return object;
}

[[nodiscard]] support::ExpectedVoid write_settings_text(
    const std::filesystem::path& settings_path,
    std::string serialized) {
    const auto parent = settings_path.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return std::unexpected(settings_error(
                "could not create settings directory",
                directory_error.message()));
        }
        (void)::chmod(parent.c_str(), 0700);
    }

    std::error_code target_error;
    const auto target_status = std::filesystem::symlink_status(settings_path, target_error);
    if (!target_error && std::filesystem::is_symlink(target_status)) {
        return std::unexpected(settings_error(
            "refusing to write symlinked settings file",
            settings_path.string()));
    }
    if (target_error && target_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(settings_error(
            "could not inspect settings file",
            target_error.message()));
    }

    auto temporary = settings_path;
    temporary += ".tmp";
    std::error_code temporary_error;
    const auto temporary_status = std::filesystem::symlink_status(temporary, temporary_error);
    if (!temporary_error && std::filesystem::exists(temporary_status)) {
        return std::unexpected(settings_error(
            "refusing to replace existing temporary settings path",
            temporary.string()));
    }
    if (temporary_error && temporary_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(settings_error(
            "could not inspect temporary settings path",
            temporary_error.message()));
    }

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(settings_error(
                "could not open temporary settings file",
                temporary.string()));
        }
        output << serialized;
        if (!output) {
            return std::unexpected(settings_error(
                "could not write temporary settings file",
                temporary.string()));
        }
    }
    (void)::chmod(temporary.c_str(), 0600);

    std::error_code rename_error;
    std::filesystem::rename(temporary, settings_path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return std::unexpected(settings_error(
            "could not replace settings file",
            rename_error.message()));
    }
    (void)::chmod(settings_path.c_str(), 0600);
    return {};
}

/// Surgical field-level merge write: re-read the current file under lock,
/// migrate it, and apply only the caller-supplied field, preserving every
/// unmodified and unknown field. The file is created (with parent directory)
/// when it does not exist.
[[nodiscard]] support::ExpectedVoid persist_field(
    const std::filesystem::path& path,
    std::string field,
    support::JsonValue value) {
    // The proper-lockfile-compatible lock lives at `<path>.lock`, so the
    // parent directory must exist before acquiring it.
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return std::unexpected(settings_error(
                "could not create settings directory",
                directory_error.message()));
        }
        (void)::chmod(parent.c_str(), 0700);
    }

    auto lock = FileLock::acquire(path);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto object = read_current_settings(path);
    if (!object) {
        return std::unexpected(object.error());
    }
    object->insert_or_assign(std::move(field), std::move(value));
    auto serialized = detail::serialize_pretty_json(support::JsonValue{*object}, true);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return write_settings_text(path, std::move(*serialized));
}

} // namespace

bool is_valid_mcp_server_id(std::string_view server_id) {
    if (server_id.empty() || server_id.size() > kMcpServerIdMaxLength) {
        return false;
    }
    for (const auto character : server_id) {
        if (!is_mcp_server_id_char(character)) {
            return false;
        }
    }
    return true;
}

struct SettingsManager::Impl {
    std::filesystem::path cwd;
    std::filesystem::path global_path;
    std::filesystem::path project_path;
    bool project_trusted{true};

    UserSettings global_settings;
    UserSettings project_settings;
    UserSettings merged_settings;
    std::vector<UserMcpServerSettings> merged_mcp_servers;
    std::vector<SettingsError> errors;
    bool global_load_failed{false};
    bool project_load_failed{false};

    [[nodiscard]] UserSettings merge(const UserSettings& global, const UserSettings& project) const {
        UserSettings merged = global;
        if (project.default_provider) merged.default_provider = project.default_provider;
        if (project.default_model) merged.default_model = project.default_model;
        if (project.default_thinking_level) merged.default_thinking_level = project.default_thinking_level;
        if (project.enabled_models) merged.enabled_models = project.enabled_models;
        if (project.session_dir) merged.session_dir = project.session_dir;
        if (project.shell_path) merged.shell_path = project.shell_path;
        if (project.shell_command_prefix) merged.shell_command_prefix = project.shell_command_prefix;
        if (project.theme) merged.theme = project.theme;
        if (project.compaction) {
            // Per-field deep merge: a project-scope field wins, fields the
            // project scope does not set keep the global scope value (pi
            // deep-merge semantics for the nested `compaction` object).
            auto merged_compaction =
                merged.compaction.value_or(UserCompactionSettings{});
            if (project.compaction->enabled) {
                merged_compaction.enabled = project.compaction->enabled;
            }
            if (project.compaction->reserve_tokens) {
                merged_compaction.reserve_tokens =
                    project.compaction->reserve_tokens;
            }
            if (project.compaction->keep_recent_tokens) {
                merged_compaction.keep_recent_tokens =
                    project.compaction->keep_recent_tokens;
            }
            merged.compaction = merged_compaction;
        }
        if (project.retry) {
            // Per-field deep merge for the nested `retry` object.
            auto merged_retry = merged.retry.value_or(UserRetrySettings{});
            if (project.retry->enabled) {
                merged_retry.enabled = project.retry->enabled;
            }
            if (project.retry->max_retries) {
                merged_retry.max_retries = project.retry->max_retries;
            }
            if (project.retry->base_delay_ms) {
                merged_retry.base_delay_ms = project.retry->base_delay_ms;
            }
            merged.retry = merged_retry;
        }
        if (project.hide_thinking_block) {
            merged.hide_thinking_block = project.hide_thinking_block;
        }
        if (project.output_pad) {
            merged.output_pad = project.output_pad;
        }
        if (project.enable_skill_commands) {
            merged.enable_skill_commands = project.enable_skill_commands;
        }
        if (project.mcp_servers) {
            // Per-Server-Id deep merge, matching the nested `compaction`
            // rule: a project entry overrides the global entry carrying the
            // same Server Id field by field, and project-only Server Ids are
            // appended in Server Id order. Each entry is self-contained (a
            // `url` is validated per scope), so an omitted field means
            // "inherit", never "clear". The Server Id is the identity and
            // never merges.
            auto merged_servers = merged.mcp_servers.value_or(std::vector<UserMcpServerSettings>{});
            for (const auto& project_server : *project.mcp_servers) {
                const auto existing =
                        std::ranges::find_if(merged_servers, [&project_server](const UserMcpServerSettings& candidate) {
                            return candidate.server_id == project_server.server_id;
                        });
                if (existing == merged_servers.end()) {
                    merged_servers.push_back(project_server);
                    continue;
                }
                if (!project_server.url.empty()) existing->url = project_server.url;
                if (project_server.bearer_env_var) {
                    existing->bearer_env_var = project_server.bearer_env_var;
                }
                if (project_server.activation) existing->activation = project_server.activation;
                if (project_server.approval) existing->approval = project_server.approval;
            }
            merged.mcp_servers = std::move(merged_servers);
        }
        return merged;
    }

    void apply_scope(const std::filesystem::path& path,
            SettingsScope scope,
            bool allow_default_project_trust,
            UserSettings& target,
            bool& load_failed,
            bool clear_on_failure) {
        load_failed = false;
        if (path.empty()) {
            return;
        }

        auto parsed = load_scope(path, allow_default_project_trust);
        if (!parsed) {
            load_failed = true;
            if (clear_on_failure) {
                target = {};
            }
            errors.push_back(SettingsError{
                    .scope = scope,
                    .message = settings_error_text(parsed.error()),
            });
            return;
        }
        target = std::move(*parsed);
    }

    [[nodiscard]] bool global_write_suppressed() const noexcept { return global_load_failed || global_path.empty(); }

    [[nodiscard]] support::ExpectedVoid persist_global_field(std::string field, support::JsonValue value) {
        return persist_field(global_path, std::move(field), std::move(value));
    }

    void recompute_merged() {
        merged_settings = merge(global_settings, project_settings);
        merged_mcp_servers = merged_settings.mcp_servers.value_or(std::vector<UserMcpServerSettings>{});
    }
};

SettingsManager::SettingsManager(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SettingsManager::SettingsManager(SettingsManager&&) noexcept = default;
SettingsManager& SettingsManager::operator=(SettingsManager&&) noexcept = default;
SettingsManager::~SettingsManager() = default;

SettingsManager SettingsManager::create(
    std::filesystem::path cwd,
    std::filesystem::path agent_dir,
    bool project_trusted) {
    auto impl = std::make_unique<Impl>();
    impl->cwd = cwd;
    if (!agent_dir.empty()) {
        impl->global_path = agent_dir / "settings.json";
    }
    if (!cwd.empty()) {
        impl->project_path = cwd / kProjectConfigDir / "settings.json";
    }
    impl->project_trusted = project_trusted;

    // Global scope always loads.
    impl->apply_scope(impl->global_path,
            SettingsScope::Global,
            /* allow_default_project_trust */ true,
            impl->global_settings,
            impl->global_load_failed,
            /* clear_on_failure */ false);

    // Project scope loads only while the project is trusted.
    if (impl->project_trusted) {
        impl->apply_scope(impl->project_path,
                SettingsScope::Project,
                /* allow_default_project_trust */ false,
                impl->project_settings,
                impl->project_load_failed,
                /* clear_on_failure */ false);
    }

    impl->recompute_merged();
    return SettingsManager(std::move(impl));
}

const std::filesystem::path& SettingsManager::global_path() const noexcept {
    return impl_->global_path;
}

const std::filesystem::path& SettingsManager::project_path() const noexcept {
    return impl_->project_path;
}

const std::filesystem::path& SettingsManager::cwd() const noexcept {
    return impl_->cwd;
}

const UserSettings& SettingsManager::global_settings() const noexcept {
    return impl_->global_settings;
}

const UserSettings& SettingsManager::project_settings() const noexcept {
    return impl_->project_settings;
}

const UserSettings& SettingsManager::settings() const noexcept {
    return impl_->merged_settings;
}

bool SettingsManager::is_project_trusted() const noexcept {
    return impl_->project_trusted;
}

std::optional<DefaultProjectTrust> SettingsManager::default_project_trust() const noexcept {
    return impl_->global_settings.default_project_trust;
}

const std::vector<SettingsError>& SettingsManager::errors() const noexcept {
    return impl_->errors;
}

support::ExpectedVoid SettingsManager::set_project_trusted(bool trusted) {
    if (impl_->project_trusted == trusted) {
        return support::ExpectedVoid{};
    }
    impl_->project_trusted = trusted;
    if (!trusted) {
        impl_->project_settings = {};
        impl_->project_load_failed = false;
        impl_->errors.erase(
            std::remove_if(
                impl_->errors.begin(),
                impl_->errors.end(),
                [](const SettingsError& error) {
                    return error.scope == SettingsScope::Project;
                }),
            impl_->errors.end());
        impl_->recompute_merged();
        return support::ExpectedVoid{};
    }
    impl_->apply_scope(impl_->project_path,
            SettingsScope::Project,
            /* allow_default_project_trust */ false,
            impl_->project_settings,
            impl_->project_load_failed,
            /* clear_on_failure */ false);
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::reload() {
    impl_->errors.clear();
    impl_->global_load_failed = false;
    impl_->project_load_failed = false;

    impl_->apply_scope(impl_->global_path,
            SettingsScope::Global,
            /* allow_default_project_trust */ true,
            impl_->global_settings,
            impl_->global_load_failed,
            /* clear_on_failure */ true);

    if (impl_->project_trusted) {
        impl_->apply_scope(impl_->project_path,
                SettingsScope::Project,
                /* allow_default_project_trust */ false,
                impl_->project_settings,
                impl_->project_load_failed,
                /* clear_on_failure */ true);
    }

    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_theme(
    SettingsScope scope,
    std::string_view value) {
    if (scope == SettingsScope::Project && !impl_->project_trusted) {
        return std::unexpected(settings_error(
            "Project is not trusted; refusing to write project settings"));
    }
    // A scope whose load failed suppresses writes to that scope (pi).
    const bool load_failed = scope == SettingsScope::Global
        ? impl_->global_load_failed
        : impl_->project_load_failed;
    if (load_failed) {
        return support::ExpectedVoid{};
    }
    const auto& path = scope == SettingsScope::Global
        ? impl_->global_path
        : impl_->project_path;
    if (path.empty()) {
        return support::ExpectedVoid{};
    }

    auto& target = scope == SettingsScope::Global
        ? impl_->global_settings
        : impl_->project_settings;
    if (target.theme == value) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    auto persisted = persist_field(path, "theme", support::JsonValue{std::string{value}});
    if (!persisted) {
        return persisted;
    }
    target.theme = std::string{value};
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_default_thinking_level(
    SettingsScope scope,
    std::string_view value) {
    if (scope == SettingsScope::Project && !impl_->project_trusted) {
        return std::unexpected(settings_error(
            "Project is not trusted; refusing to write project settings"));
    }
    if (!is_thinking_level(value)) {
        return std::unexpected(settings_error(
            "invalid defaultThinkingLevel",
            "defaultThinkingLevel must be one of: off, minimal, low, medium, high, xhigh, max"));
    }
    // A scope whose load failed suppresses writes to that scope (pi).
    const bool load_failed = scope == SettingsScope::Global
        ? impl_->global_load_failed
        : impl_->project_load_failed;
    if (load_failed) {
        return support::ExpectedVoid{};
    }
    const auto& path = scope == SettingsScope::Global
        ? impl_->global_path
        : impl_->project_path;
    if (path.empty()) {
        return support::ExpectedVoid{};
    }

    auto& target = scope == SettingsScope::Global
        ? impl_->global_settings
        : impl_->project_settings;
    if (target.default_thinking_level == value) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    auto persisted = persist_field(
        path, "defaultThinkingLevel", support::JsonValue{std::string{value}});
    if (!persisted) {
        return persisted;
    }
    target.default_thinking_level = std::string{value};
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

bool SettingsManager::hide_thinking_block() const noexcept {
    return impl_->merged_settings.hide_thinking_block.value_or(false);
}

std::size_t SettingsManager::output_pad() const noexcept {
    // pi `getOutputPad`: `settings.outputPad === 0 ? 0 : 1`. The parse path
    // already resolves stored values to 0 or 1; the default is 1.
    const auto stored = impl_->merged_settings.output_pad.value_or(1);
    return stored == 0 ? 0 : 1;
}

support::ExpectedVoid SettingsManager::set_hide_thinking_block(bool hide) {
    // pi `setHideThinkingBlock` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.hide_thinking_block == hide) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("hideThinkingBlock", support::JsonValue{hide}); !persisted) {
        return persisted;
    }
    target.hide_thinking_block = hide;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_output_pad(std::size_t padding) {
    // pi `setOutputPad` always writes the global scope; only 0 and 1 exist.
    if (padding != 0 && padding != 1) {
        return std::unexpected(settings_error(
            "invalid outputPad",
            "outputPad must be 0 or 1"));
    }
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.output_pad == padding) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("outputPad", support::JsonValue{static_cast<double>(padding)});
            !persisted) {
        return persisted;
    }
    target.output_pad = padding;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

bool SettingsManager::get_enable_skill_commands() const noexcept {
    // pi `getEnableSkillCommands`: `this.settings.enableSkillCommands ?? true`.
    return impl_->merged_settings.enable_skill_commands.value_or(true);
}

const std::vector<UserMcpServerSettings>& SettingsManager::mcp_servers() const noexcept {
    return impl_->merged_mcp_servers;
}

support::ExpectedVoid SettingsManager::set_enable_skill_commands(bool enabled) {
    // pi `setEnableSkillCommands` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.enable_skill_commands == enabled) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("enableSkillCommands", support::JsonValue{enabled}); !persisted) {
        return persisted;
    }
    target.enable_skill_commands = enabled;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_default_project_trust(
    DefaultProjectTrust trust) {
    // pi `setDefaultProjectTrust` always writes the global scope and is
    // global-only (the project scope never carries a trust default).
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.default_project_trust == trust) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("defaultProjectTrust", support::JsonValue{to_string(trust)});
            !persisted) {
        return persisted;
    }
    target.default_project_trust = trust;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_default_model_and_provider(
    std::string provider,
    std::string model) {
    // pi `setDefaultModelAndProvider` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.default_provider == provider && target.default_model == model) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("defaultProvider", support::JsonValue{provider}); !persisted) {
        return persisted;
    }
    if (auto persisted = impl_->persist_global_field("defaultModel", support::JsonValue{model}); !persisted) {
        return persisted;
    }
    target.default_provider = std::move(provider);
    target.default_model = std::move(model);
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_enabled_models(
    std::optional<std::vector<std::string>> patterns) {
    // pi `setEnabledModels` always writes the global scope.
    if (impl_->global_load_failed) {
        return support::ExpectedVoid{};
    }
    if (impl_->global_path.empty()) {
        return support::ExpectedVoid{};
    }

    auto& target = impl_->global_settings;
    if (target.enabled_models == patterns) {
        return support::ExpectedVoid{};
    }

    if (!patterns) {
        // pi writes `undefined`, which its JSON serializer drops: remove the
        // field, preserving every other field.
        auto lock = FileLock::acquire(impl_->global_path);
        if (!lock) {
            return std::unexpected(lock.error());
        }
        auto object = read_current_settings(impl_->global_path);
        if (!object) {
            return std::unexpected(object.error());
        }
        object->erase("enabledModels");
        auto serialized = detail::serialize_pretty_json(support::JsonValue{*object}, true);
        if (!serialized) {
            return std::unexpected(serialized.error());
        }
        if (auto written = write_settings_text(impl_->global_path, std::move(*serialized));
            !written) {
            return written;
        }
    } else if (auto persisted = persist_field(
                   impl_->global_path,
                   "enabledModels",
                   [&] {
                       support::JsonValue::array_t entries;
                       entries.reserve(patterns->size());
                       for (const auto& pattern : *patterns) {
                           entries.emplace_back(pattern);
                       }
                       return support::JsonValue{std::move(entries)};
                   }());
               !persisted) {
        return persisted;
    }
    target.enabled_models = std::move(patterns);
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

} // namespace cch::coding_agent
