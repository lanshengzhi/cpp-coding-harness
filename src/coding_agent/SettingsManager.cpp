#include <cch/coding_agent/Settings.hpp>

#include "util/Json.hpp"

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

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace cch::coding_agent {
namespace {

using JsonObject = util::JsonValue::object_t;

constexpr std::string_view kProjectConfigDir = ".pi";

// proper-lockfile-compatible lock parameters (pi FileSettingsStorage): 10
// attempts, 20 ms between attempts, stale lock reclaimed after 30 s.
constexpr int kLockMaxAttempts = 10;
constexpr auto kLockPollInterval = std::chrono::milliseconds{20};
constexpr auto kLockStaleAfter = std::chrono::seconds{30};

[[nodiscard]] util::Error settings_error(
    std::string message,
    std::string detail = {}) {
    return util::make_error(
        util::ErrorCode::Workspace,
        std::move(message),
        std::move(detail));
}

[[nodiscard]] util::Error settings_file_error(
    std::string message,
    const std::filesystem::path& path,
    std::string detail = {}) {
    if (detail.empty()) {
        detail = message;
    }
    return util::make_error(
        util::ErrorCode::JsonParse,
        std::move(message),
        std::move(detail),
        path.string());
}

[[nodiscard]] std::string settings_error_text(const util::Error& error) {
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
                settings["transport"] = util::JsonValue{
                    *enabled ? std::string{"websocket"} : std::string{"sse"}};
            }
            settings.erase(websockets);
        }
    }

    const auto skills = settings.find("skills");
    if (skills != settings.end() && !skills->second.holds<util::JsonValue::array_t>()) {
        if (const auto* skills_object = skills->second.get_if<JsonObject>()) {
            const auto enable_commands = skills_object->find("enableSkillCommands");
            if (enable_commands != skills_object->end() &&
                !settings.contains("enableSkillCommands")) {
                settings["enableSkillCommands"] = enable_commands->second;
            }
            const auto custom = skills_object->find("customDirectories");
            if (custom != skills_object->end() &&
                custom->second.holds<util::JsonValue::array_t>() &&
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
            util::JsonValue::object_t* provider_object = nullptr;
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
                provider_object->at("maxRetryDelayMs").holds<util::JsonValue::null_t>();
            if (max_delay != mutated.end() &&
                max_delay->second.holds<double>() &&
                provider_missing_delay) {
                JsonObject configured_provider = std::move(merged_provider);
                configured_provider["maxRetryDelayMs"] = max_delay->second;
                mutated["provider"] = util::JsonValue{std::move(configured_provider)};
            }
            mutated.erase("maxDelayMs");
            retry->second = util::JsonValue{std::move(mutated)};
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

/// Parse pi's nested `compaction` object (`{enabled, reserveTokens,
/// keepRecentTokens}`). Each field is optional; unknown or mistyped fields
/// are ignored (a mistyped field falls back to the pi default at resolution,
/// mirroring pi's `settings.compaction?.enabled ?? true` reads).
[[nodiscard]] util::Expected<UserCompactionSettings> parse_compaction_settings(
    const util::JsonValue& value) {
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
[[nodiscard]] util::Expected<UserRetrySettings> parse_retry_settings(
    const util::JsonValue& value) {
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
[[nodiscard]] util::Expected<UserSettings> parse_settings(
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
        if (const auto* array = found->second.get_if<util::JsonValue::array_t>()) {
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
    return settings;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pretty serialization (pi writes settings with `JSON.stringify(_, null, 2)`)
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] util::ExpectedVoid append_pretty_json(
    const util::JsonValue& value,
    std::string& output,
    std::size_t indentation) {
    const auto indent = [&output, indentation](std::size_t extra) {
        output.append(indentation + extra, ' ');
    };
    if (const auto* object = value.get_if<JsonObject>()) {
        output.push_back('{');
        if (!object->empty()) {
            output.push_back('\n');
            auto current = object->begin();
            while (current != object->end()) {
                indent(2);
                auto key = util::write_json(util::JsonValue{current->first});
                if (!key) {
                    return std::unexpected(key.error());
                }
                output.append(*key);
                output.append(": ");
                if (auto appended = append_pretty_json(current->second, output, indentation + 2); !appended) {
                    return appended;
                }
                ++current;
                if (current != object->end()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            indent(0);
        }
        output.push_back('}');
        return {};
    }
    if (const auto* array = value.get_if<util::JsonValue::array_t>()) {
        output.push_back('[');
        if (!array->empty()) {
            output.push_back('\n');
            for (std::size_t index = 0; index < array->size(); ++index) {
                indent(2);
                if (auto appended = append_pretty_json((*array)[index], output, indentation + 2); !appended) {
                    return appended;
                }
                if (index + 1 != array->size()) {
                    output.push_back(',');
                }
                output.push_back('\n');
            }
            indent(0);
        }
        output.push_back(']');
        return {};
    }
    auto serialized = util::write_json(value);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    output.append(*serialized);
    return {};
}

[[nodiscard]] util::Expected<std::string> serialize_pretty(const JsonObject& object) {
    std::string output;
    if (auto appended = append_pretty_json(util::JsonValue{object}, output, 0); !appended) {
        return std::unexpected(appended.error());
    }
    output.push_back('\n');
    return output;
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

    [[nodiscard]] static util::Expected<std::unique_ptr<FileLock>> acquire(
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

[[nodiscard]] util::Expected<std::optional<std::string>> read_text_file(
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
[[nodiscard]] util::Expected<UserSettings> load_scope(
    const std::filesystem::path& path,
    bool allow_default_project_trust) {
    auto content = read_text_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (!*content || (*content)->empty()) {
        return UserSettings{};
    }
    auto parsed = util::read_json<util::JsonValue>(**content);
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
[[nodiscard]] util::Expected<JsonObject> read_current_settings(
    const std::filesystem::path& path) {
    JsonObject object;
    auto content = read_text_file(path);
    if (!content) {
        return std::unexpected(content.error());
    }
    if (!*content || (*content)->empty()) {
        return object;
    }
    auto parsed = util::read_json<util::JsonValue>(**content);
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

[[nodiscard]] util::ExpectedVoid write_settings_text(
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
#if defined(__unix__) || defined(__APPLE__)
        (void)::chmod(parent.c_str(), 0700);
#endif
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
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(temporary.c_str(), 0600);
#endif

    std::error_code rename_error;
    std::filesystem::rename(temporary, settings_path, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return std::unexpected(settings_error(
            "could not replace settings file",
            rename_error.message()));
    }
#if defined(__unix__) || defined(__APPLE__)
    (void)::chmod(settings_path.c_str(), 0600);
#endif
    return {};
}

/// Surgical field-level merge write: re-read the current file under lock,
/// migrate it, and apply only the caller-supplied field, preserving every
/// unmodified and unknown field. The file is created (with parent directory)
/// when it does not exist.
[[nodiscard]] util::ExpectedVoid persist_field(
    const std::filesystem::path& path,
    std::string field,
    util::JsonValue value) {
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
#if defined(__unix__) || defined(__APPLE__)
        (void)::chmod(parent.c_str(), 0700);
#endif
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
    auto serialized = serialize_pretty(*object);
    if (!serialized) {
        return std::unexpected(serialized.error());
    }
    return write_settings_text(path, std::move(*serialized));
}

} // namespace

struct SettingsManager::Impl {
    std::filesystem::path cwd;
    std::filesystem::path global_path;
    std::filesystem::path project_path;
    bool project_trusted{true};

    UserSettings global_settings;
    UserSettings project_settings;
    UserSettings merged_settings;
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
        return merged;
    }

    void recompute_merged() {
        merged_settings = merge(global_settings, project_settings);
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
    if (!impl->global_path.empty()) {
        auto parsed = load_scope(impl->global_path, /* allow_default_project_trust */ true);
        if (!parsed) {
            impl->global_load_failed = true;
            impl->errors.push_back(SettingsError{
                .scope = SettingsScope::Global,
                .message = settings_error_text(parsed.error()),
            });
        } else {
            impl->global_settings = std::move(*parsed);
        }
    }

    // Project scope loads only while the project is trusted.
    if (impl->project_trusted && !impl->project_path.empty()) {
        auto parsed = load_scope(impl->project_path, /* allow_default_project_trust */ false);
        if (!parsed) {
            impl->project_load_failed = true;
            impl->errors.push_back(SettingsError{
                .scope = SettingsScope::Project,
                .message = settings_error_text(parsed.error()),
            });
        } else {
            impl->project_settings = std::move(*parsed);
        }
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

util::ExpectedVoid SettingsManager::set_project_trusted(bool trusted) {
    if (impl_->project_trusted == trusted) {
        return util::ExpectedVoid{};
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
        return util::ExpectedVoid{};
    }
    impl_->project_load_failed = false;
    if (!impl_->project_path.empty()) {
        auto parsed = load_scope(impl_->project_path, /* allow_default_project_trust */ false);
        if (!parsed) {
            impl_->project_load_failed = true;
            impl_->errors.push_back(SettingsError{
                .scope = SettingsScope::Project,
                .message = settings_error_text(parsed.error()),
            });
        } else {
            impl_->project_settings = std::move(*parsed);
        }
    }
    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

util::ExpectedVoid SettingsManager::reload() {
    impl_->errors.clear();
    impl_->global_load_failed = false;
    impl_->project_load_failed = false;

    if (!impl_->global_path.empty()) {
        auto parsed = load_scope(impl_->global_path, /* allow_default_project_trust */ true);
        if (!parsed) {
            impl_->global_load_failed = true;
            impl_->global_settings = {};
            impl_->errors.push_back(SettingsError{
                .scope = SettingsScope::Global,
                .message = settings_error_text(parsed.error()),
            });
        } else {
            impl_->global_settings = std::move(*parsed);
        }
    }

    if (impl_->project_trusted && !impl_->project_path.empty()) {
        auto parsed = load_scope(impl_->project_path, /* allow_default_project_trust */ false);
        if (!parsed) {
            impl_->project_load_failed = true;
            impl_->project_settings = {};
            impl_->errors.push_back(SettingsError{
                .scope = SettingsScope::Project,
                .message = settings_error_text(parsed.error()),
            });
        } else {
            impl_->project_settings = std::move(*parsed);
        }
    }

    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

util::ExpectedVoid SettingsManager::set_theme(
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
        return util::ExpectedVoid{};
    }
    const auto& path = scope == SettingsScope::Global
        ? impl_->global_path
        : impl_->project_path;
    if (path.empty()) {
        return util::ExpectedVoid{};
    }

    auto& target = scope == SettingsScope::Global
        ? impl_->global_settings
        : impl_->project_settings;
    if (target.theme == value) {
        return util::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    auto persisted = persist_field(path, "theme", util::JsonValue{std::string{value}});
    if (!persisted) {
        return persisted;
    }
    target.theme = std::string{value};
    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

util::ExpectedVoid SettingsManager::set_default_thinking_level(
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
        return util::ExpectedVoid{};
    }
    const auto& path = scope == SettingsScope::Global
        ? impl_->global_path
        : impl_->project_path;
    if (path.empty()) {
        return util::ExpectedVoid{};
    }

    auto& target = scope == SettingsScope::Global
        ? impl_->global_settings
        : impl_->project_settings;
    if (target.default_thinking_level == value) {
        return util::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    auto persisted = persist_field(
        path, "defaultThinkingLevel", util::JsonValue{std::string{value}});
    if (!persisted) {
        return persisted;
    }
    target.default_thinking_level = std::string{value};
    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

util::ExpectedVoid SettingsManager::set_default_model_and_provider(
    std::string provider,
    std::string model) {
    // pi `setDefaultModelAndProvider` always writes the global scope.
    if (impl_->global_load_failed) {
        return util::ExpectedVoid{};
    }
    if (impl_->global_path.empty()) {
        return util::ExpectedVoid{};
    }

    auto& target = impl_->global_settings;
    if (target.default_provider == provider && target.default_model == model) {
        return util::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = persist_field(
            impl_->global_path, "defaultProvider", util::JsonValue{provider});
        !persisted) {
        return persisted;
    }
    if (auto persisted = persist_field(
            impl_->global_path, "defaultModel", util::JsonValue{model});
        !persisted) {
        return persisted;
    }
    target.default_provider = std::move(provider);
    target.default_model = std::move(model);
    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

util::ExpectedVoid SettingsManager::set_enabled_models(
    std::optional<std::vector<std::string>> patterns) {
    // pi `setEnabledModels` always writes the global scope.
    if (impl_->global_load_failed) {
        return util::ExpectedVoid{};
    }
    if (impl_->global_path.empty()) {
        return util::ExpectedVoid{};
    }

    auto& target = impl_->global_settings;
    if (target.enabled_models == patterns) {
        return util::ExpectedVoid{};
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
        auto serialized = serialize_pretty(*object);
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
                       util::JsonValue::array_t entries;
                       entries.reserve(patterns->size());
                       for (const auto& pattern : *patterns) {
                           entries.emplace_back(pattern);
                       }
                       return util::JsonValue{std::move(entries)};
                   }());
               !persisted) {
        return persisted;
    }
    target.enabled_models = std::move(patterns);
    impl_->recompute_merged();
    return util::ExpectedVoid{};
}

} // namespace cch::coding_agent
