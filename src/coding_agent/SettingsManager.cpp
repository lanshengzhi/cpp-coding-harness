#include <cch/coding_agent/Settings.hpp>

#include "PrettyJson.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/stat.h>

#include <array>

namespace cch::coding_agent {
namespace {

using JsonObject = support::JsonValue::object_t;

constexpr std::string_view kProjectConfigDir = ".pi";

/// RFC 4122 version 4 UUID (lowercase, hyphenated), matching pi's
/// `randomUUID` output used for `SettingsManager.deviceId`.
[[nodiscard]] std::string new_device_id() {
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    std::uniform_int_distribution<unsigned int> byte_distribution(0, 255);
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(byte_distribution(random));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

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
    // Every member this function copies is read out before the destination is
    // written: a JSON object holds its members in a contiguous sequence, so a
    // reference or iterator into `settings` does not survive an insertion that
    // grows it (the previous `std::map` gave reference stability, #1149).
    if (settings.contains("queueMode") && !settings.contains("steeringMode")) {
        const auto queue_mode = settings.at("queueMode");
        settings.insert_or_assign("steeringMode", queue_mode);
        settings.erase("queueMode");
    }

    if (!settings.contains("transport")) {
        const auto websockets = settings.find("websockets");
        if (websockets != settings.end()) {
            if (const auto* enabled = websockets->second.get_if<bool>()) {
                const support::JsonValue transport{*enabled ? std::string{"websocket"} : std::string{"sse"}};
                settings.insert_or_assign("transport", transport);
            }
            settings.erase("websockets");
        }
    }

    if (const auto skills = settings.find("skills");
            skills != settings.end() && !skills->second.holds<support::JsonValue::array_t>()) {
        if (const auto* skills_object = skills->second.get_if<JsonObject>()) {
            const auto enable_commands = skills_object->find("enableSkillCommands");
            if (enable_commands != skills_object->end() && !settings.contains("enableSkillCommands")) {
                const auto enabled = enable_commands->second;
                settings.insert_or_assign("enableSkillCommands", enabled);
            }
            const auto custom = skills_object->find("customDirectories");
            if (custom != skills_object->end() && custom->second.holds<support::JsonValue::array_t>() &&
                    !custom->second.get_array().empty()) {
                const auto directories = custom->second;
                settings.insert_or_assign("skills", directories);
            } else {
                settings.erase("skills");
            }
        }
    }

    if (const auto retry = settings.find("retry"); retry != settings.end()) {
        if (const auto* retry_object = retry->second.get_if<JsonObject>()) {
            auto mutated = *retry_object;
            const auto max_delay = mutated.find("maxDelayMs");
            // Copy the candidate value out before any member of `mutated` is
            // inserted: the insert can relocate the sequence that `max_delay`
            // points into.
            const bool has_numeric_delay = max_delay != mutated.end() && max_delay->second.holds<double>();
            const support::JsonValue delay_value = has_numeric_delay ? max_delay->second : support::JsonValue{};
            const bool provider_missing_delay = [&mutated] {
                const auto provider = mutated.find("provider");
                if (provider == mutated.end()) {
                    return true;
                }
                const auto* provider_object = provider->second.get_if<JsonObject>();
                return provider_object == nullptr || !provider_object->contains("maxRetryDelayMs") ||
                       provider_object->at("maxRetryDelayMs").holds<support::JsonValue::null_t>();
            }();
            if (has_numeric_delay && provider_missing_delay) {
                JsonObject configured_provider;
                if (const auto provider = mutated.find("provider");
                        provider != mutated.end() && provider->second.holds<JsonObject>()) {
                    configured_provider = provider->second.get<JsonObject>();
                }
                configured_provider.insert_or_assign("maxRetryDelayMs", delay_value);
                mutated.insert_or_assign("provider", support::JsonValue{std::move(configured_provider)});
            }
            mutated.erase("maxDelayMs");
            const auto migrated = support::JsonValue{std::move(mutated)};
            settings.insert_or_assign("retry", migrated);
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

/// pi `getFullscreenScrollbar`: only the exact stored strings `"always"` and
/// `"hidden"` resolve to themselves; every other stored value resolves as
/// `"auto"`.
[[nodiscard]] FullscreenScrollbar resolve_fullscreen_scrollbar(const support::JsonValue& value) {
    if (const auto* mode = value.get_if<std::string>()) {
        if (*mode == "always") {
            return FullscreenScrollbar::Always;
        }
        if (*mode == "hidden") {
            return FullscreenScrollbar::Hidden;
        }
    }
    return FullscreenScrollbar::Auto;
}

/// pi `getFullscreenCopyOnSelect` and its AltScreen consumer: the nullish
/// stored value falls back to `true` (`?? true`) and every other value keeps
/// JavaScript truthiness, so `false`, `0`, and `""` disable copy-on-select
/// while wrong types such as `"false"` and `[]` enable it.
[[nodiscard]] bool resolve_fullscreen_copy_on_select(const support::JsonValue& value) {
    if (value.holds<support::JsonValue::null_t>()) {
        return true;
    }
    if (const auto* enabled = value.get_if<bool>()) {
        return *enabled;
    }
    if (const auto* number = value.get_if<double>()) {
        return *number != 0.0;
    }
    if (const auto* text = value.get_if<std::string>()) {
        return !text->empty();
    }
    return true;
}

/// pi `getFullscreenWheelScrollLines`: only a finite number is a line count
/// and it is floored and clamped to 1..100 in the double domain (pi
/// `Math.max(1, Math.min(100, Math.floor(lines)))`); every other stored value
/// resolves as `"auto"`.
[[nodiscard]] FullscreenWheelScrollLines resolve_fullscreen_wheel_scroll_lines(const support::JsonValue& value) {
    const auto* number = value.get_if<double>();
    if (number == nullptr || !std::isfinite(*number)) {
        return FullscreenWheelScrollLines{AutoWheelScrollLines{}};
    }
    const auto lines = std::max(1.0, std::min(100.0, std::floor(*number)));
    return FullscreenWheelScrollLines{static_cast<std::size_t>(lines)};
}

/// pi `setFullscreenWheelScrollLines` clamp for a typed line count:
/// `Math.max(1, Math.min(100, lines))`.
[[nodiscard]] std::size_t clamp_wheel_scroll_lines(std::size_t lines) noexcept {
    return std::min<std::size_t>(std::max<std::size_t>(lines, 1), 100);
}

/// Wire-form canonicality of the three fullscreen settings in one scope: the
/// exact JSON shape pi's setters persist. A present but non-canonical raw
/// value still resolves to a typed value at load, so the setter for that same
/// resolved value must rewrite the field instead of treating it as a house
/// no-op and leaving the malformed wire value on disk.
struct FullscreenWireCanonical {
    bool scrollbar{false};
    bool copy_on_select{false};
    bool wheel_scroll_lines{false};
};

[[nodiscard]] bool is_canonical_fullscreen_scrollbar(const support::JsonValue& value) {
    const auto* mode = value.get_if<std::string>();
    return mode != nullptr && (*mode == "hidden" || *mode == "auto" || *mode == "always");
}

[[nodiscard]] bool is_canonical_fullscreen_copy_on_select(const support::JsonValue& value) {
    return value.holds<bool>();
}

[[nodiscard]] bool is_canonical_fullscreen_wheel_scroll_lines(const support::JsonValue& value) {
    if (const auto* mode = value.get_if<std::string>()) {
        return *mode == "auto";
    }
    const auto* number = value.get_if<double>();
    return number != nullptr && std::isfinite(*number) && std::floor(*number) == *number && *number >= 1.0 &&
           *number <= 100.0;
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

/// Parse pi's nested `codemode` object (`{mode, inlineBudget}`). Each field is
/// optional; unknown or mistyped fields are ignored (a mistyped field falls
/// back to the pi default at resolution, mirroring pi's
/// `settings.codemode?.mode ?? "on"` reads). `mode` is validated against pi's
/// `"on" | "only"`; an unknown value is ignored rather than rejected so a
/// newer pi setting cannot break the load.
[[nodiscard]] support::Expected<UserCodemodeSettings> parse_codemode_settings(const support::JsonValue& value) {
    const auto* object = value.get_if<JsonObject>();
    if (object == nullptr) {
        return std::unexpected(settings_file_error("invalid codemode", {}, "codemode must be an object"));
    }
    UserCodemodeSettings settings;
    const auto mode = object->find("mode");
    if (mode != object->end()) {
        if (const auto* parsed = mode->second.get_if<std::string>()) {
            if (*parsed == "on" || *parsed == "only") {
                settings.mode = *parsed;
            }
        }
    }
    const auto inline_budget = object->find("inlineBudget");
    if (inline_budget != object->end()) {
        if (const auto* parsed = inline_budget->second.get_if<double>()) {
            if (*parsed >= 0) {
                settings.inline_budget = static_cast<std::int64_t>(*parsed);
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
        const JsonObject& object, bool allow_default_project_trust, FullscreenWireCanonical* fullscreen_canonical) {
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
    if (const auto* value = string_field(object, "tuiMode")) {
        settings.tui_mode = *value;
    }
    if (const auto* value = string_field(object, "fullscreenExitOutput")) {
        settings.fullscreen_exit_output = *value;
    }
    // The three fullscreen settings below resolve each present value per scope
    // before the merge, because pi's deep merge gives any present project
    // value priority over global even when it is invalid.
    if (const auto found = object.find("fullscreenScrollbar"); found != object.end()) {
        settings.fullscreen_scrollbar = resolve_fullscreen_scrollbar(found->second);
        if (fullscreen_canonical != nullptr) {
            fullscreen_canonical->scrollbar = is_canonical_fullscreen_scrollbar(found->second);
        }
    }
    if (const auto found = object.find("fullscreenCopyOnSelect"); found != object.end()) {
        settings.fullscreen_copy_on_select = resolve_fullscreen_copy_on_select(found->second);
        if (fullscreen_canonical != nullptr) {
            fullscreen_canonical->copy_on_select = is_canonical_fullscreen_copy_on_select(found->second);
        }
    }
    if (const auto found = object.find("fullscreenWheelScrollLines"); found != object.end()) {
        settings.fullscreen_wheel_scroll_lines = resolve_fullscreen_wheel_scroll_lines(found->second);
        if (fullscreen_canonical != nullptr) {
            fullscreen_canonical->wheel_scroll_lines = is_canonical_fullscreen_wheel_scroll_lines(found->second);
        }
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
    if (const auto found = object.find("codemode"); found != object.end()) {
        if (auto parsed = parse_codemode_settings(found->second); parsed) {
            settings.codemode = std::move(*parsed);
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
    if (const auto found = object.find("showHardwareCursor"); found != object.end()) {
        if (const auto* parsed = found->second.get_if<bool>()) {
            settings.show_hardware_cursor = *parsed;
        }
    }
    if (const auto found = object.find("terminal"); found != object.end()) {
        if (const auto* terminal_obj = found->second.get_if<JsonObject>()) {
            if (const auto prog_found = terminal_obj->find("showTerminalProgress"); prog_found != terminal_obj->end()) {
                if (const auto* parsed = prog_found->second.get_if<bool>()) {
                    settings.show_terminal_progress = *parsed;
                }
            }
        }
    }
    if (const auto* value = string_field(object, "deviceId")) {
        settings.device_id = *value;
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

/// Load one scope: read, parse, and apply pi's read-time migrations. When
/// `fullscreen_canonical` is non-null it receives the wire-form canonicality
/// of the scope's three fullscreen settings.
[[nodiscard]] support::Expected<UserSettings> load_scope(const std::filesystem::path& path,
        bool allow_default_project_trust,
        FullscreenWireCanonical* fullscreen_canonical) {
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
    return parse_settings(object, allow_default_project_trust, fullscreen_canonical);
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

/// Surgical nested field-level merge write: re-read the current file under lock,
/// migrate it, and apply only the caller-supplied child field inside parent field,
/// preserving every unmodified sibling and unknown field.
[[nodiscard]] support::ExpectedVoid persist_nested_field(const std::filesystem::path& path,
        std::string parent_field,
        std::string child_field,
        support::JsonValue value) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return std::unexpected(settings_error("could not create settings directory", directory_error.message()));
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
    JsonObject parent_obj;
    if (auto existing_parent = object->find(parent_field);
            existing_parent != object->end() && existing_parent->second.holds<JsonObject>()) {
        if (const auto* existing_ptr = existing_parent->second.get_if<JsonObject>()) {
            parent_obj = *existing_ptr;
        }
    }
    parent_obj.insert_or_assign(std::move(child_field), std::move(value));
    object->insert_or_assign(std::move(parent_field), support::JsonValue{std::move(parent_obj)});
    auto serialized = detail::serialize_pretty_json(support::JsonValue{*object}, true);
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
    /// Wire-form canonicality of the global scope's three fullscreen settings.
    FullscreenWireCanonical global_fullscreen_canonical;

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
        if (project.codemode) {
            // Per-field deep merge for the nested `codemode` object.
            auto merged_codemode = merged.codemode.value_or(UserCodemodeSettings{});
            if (project.codemode->mode) {
                merged_codemode.mode = project.codemode->mode;
            }
            if (project.codemode->inline_budget) {
                merged_codemode.inline_budget = project.codemode->inline_budget;
            }
            merged.codemode = merged_codemode;
        }
        if (project.hide_thinking_block) {
            merged.hide_thinking_block = project.hide_thinking_block;
        }
        if (project.tui_mode) {
            merged.tui_mode = project.tui_mode;
        }
        if (project.fullscreen_exit_output) {
            merged.fullscreen_exit_output = project.fullscreen_exit_output;
        }
        if (project.fullscreen_scrollbar) {
            merged.fullscreen_scrollbar = project.fullscreen_scrollbar;
        }
        if (project.fullscreen_copy_on_select) {
            merged.fullscreen_copy_on_select = project.fullscreen_copy_on_select;
        }
        if (project.fullscreen_wheel_scroll_lines) {
            merged.fullscreen_wheel_scroll_lines = project.fullscreen_wheel_scroll_lines;
        }
        if (project.output_pad) {
            merged.output_pad = project.output_pad;
        }
        if (project.enable_skill_commands) {
            merged.enable_skill_commands = project.enable_skill_commands;
        }
        if (project.show_hardware_cursor) {
            merged.show_hardware_cursor = project.show_hardware_cursor;
        }
        if (project.show_terminal_progress) {
            merged.show_terminal_progress = project.show_terminal_progress;
        }
        return merged;
    }

    void apply_scope(const std::filesystem::path& path,
            SettingsScope scope,
            bool allow_default_project_trust,
            UserSettings& target,
            bool& load_failed,
            bool clear_on_failure,
            FullscreenWireCanonical* fullscreen_canonical = nullptr) {
        load_failed = false;
        if (fullscreen_canonical != nullptr) {
            *fullscreen_canonical = {};
        }
        if (path.empty()) {
            return;
        }

        auto parsed = load_scope(path, allow_default_project_trust, fullscreen_canonical);
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

    [[nodiscard]] support::ExpectedVoid persist_global_nested_field(
            std::string parent_field, std::string child_field, support::JsonValue value) {
        return persist_nested_field(global_path, std::move(parent_field), std::move(child_field), std::move(value));
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
    impl->apply_scope(impl->global_path,
            SettingsScope::Global,
            /* allow_default_project_trust */ true,
            impl->global_settings,
            impl->global_load_failed,
            /* clear_on_failure */ false,
            &impl->global_fullscreen_canonical);

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
            /* clear_on_failure */ true,
            &impl_->global_fullscreen_canonical);

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

FullscreenExitOutput SettingsManager::get_fullscreen_exit_output() const noexcept {
    const auto& stored = impl_->merged_settings.fullscreen_exit_output;
    if (stored && *stored == "resume-hint") {
        return FullscreenExitOutput::ResumeHint;
    }
    return FullscreenExitOutput::Transcript;
}

support::ExpectedVoid SettingsManager::set_fullscreen_exit_output(FullscreenExitOutput output) {
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    const std::string value = output == FullscreenExitOutput::ResumeHint ? "resume-hint" : "transcript";
    auto& target = impl_->global_settings;
    if (target.fullscreen_exit_output == value) {
        return support::ExpectedVoid{};
    }

    if (auto persisted = impl_->persist_global_field("fullscreenExitOutput", support::JsonValue{value}); !persisted) {
        return persisted;
    }
    target.fullscreen_exit_output = value;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

FullscreenScrollbar SettingsManager::get_fullscreen_scrollbar() const noexcept {
    return impl_->merged_settings.fullscreen_scrollbar.value_or(FullscreenScrollbar::Auto);
}

bool SettingsManager::get_fullscreen_copy_on_select() const noexcept {
    return impl_->merged_settings.fullscreen_copy_on_select.value_or(true);
}

FullscreenWheelScrollLines SettingsManager::get_fullscreen_wheel_scroll_lines() const noexcept {
    return impl_->merged_settings.fullscreen_wheel_scroll_lines.value_or(
            FullscreenWheelScrollLines{AutoWheelScrollLines{}});
}

support::ExpectedVoid SettingsManager::set_fullscreen_scrollbar(FullscreenScrollbar mode) {
    // pi `setFullscreenScrollbar` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    std::string value{"auto"};
    if (mode == FullscreenScrollbar::Hidden) {
        value = "hidden";
    } else if (mode == FullscreenScrollbar::Always) {
        value = "always";
    }
    auto& target = impl_->global_settings;
    // House no-op: only when the global field is already canonical on disk and
    // equal to the resolved value. A raw non-canonical value that resolves to
    // the same mode is rewritten as the canonical spelling pi persists.
    if (impl_->global_fullscreen_canonical.scrollbar && target.fullscreen_scrollbar == mode) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("fullscreenScrollbar", support::JsonValue{value}); !persisted) {
        return persisted;
    }
    target.fullscreen_scrollbar = mode;
    impl_->global_fullscreen_canonical.scrollbar = true;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_fullscreen_copy_on_select(bool enabled) {
    // pi `setFullscreenCopyOnSelect` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    // House no-op: only when the global field is already canonical on disk and
    // equal to the resolved value. A raw non-canonical value that resolves to
    // the same boolean is rewritten as the canonical JSON boolean.
    if (impl_->global_fullscreen_canonical.copy_on_select && target.fullscreen_copy_on_select == enabled) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("fullscreenCopyOnSelect", support::JsonValue{enabled});
            !persisted) {
        return persisted;
    }
    target.fullscreen_copy_on_select = enabled;
    impl_->global_fullscreen_canonical.copy_on_select = true;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

support::ExpectedVoid SettingsManager::set_fullscreen_wheel_scroll_lines(FullscreenWheelScrollLines lines) {
    // pi `setFullscreenWheelScrollLines` always writes the global scope and
    // keeps `"auto"` while clamping a number to 1..100.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    const auto* requested = std::get_if<std::size_t>(&lines);
    const FullscreenWheelScrollLines normalized =
            requested == nullptr ? FullscreenWheelScrollLines{AutoWheelScrollLines{}}
                                 : FullscreenWheelScrollLines{clamp_wheel_scroll_lines(*requested)};
    auto& target = impl_->global_settings;
    // House no-op: only when the global field is already canonical on disk and
    // equal to the resolved value. A raw non-canonical value (unclamped number,
    // wrong type) that resolves to the same lines is rewritten as the canonical
    // wire value.
    if (impl_->global_fullscreen_canonical.wheel_scroll_lines && target.fullscreen_wheel_scroll_lines == normalized) {
        return support::ExpectedVoid{};
    }

    support::JsonValue value = std::holds_alternative<AutoWheelScrollLines>(normalized)
                                       ? support::JsonValue{std::string{"auto"}}
                                       : support::JsonValue{static_cast<double>(std::get<std::size_t>(normalized))};
    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("fullscreenWheelScrollLines", std::move(value)); !persisted) {
        return persisted;
    }
    target.fullscreen_wheel_scroll_lines = normalized;
    impl_->global_fullscreen_canonical.wheel_scroll_lines = true;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

TuiMode SettingsManager::get_tui_mode() const noexcept {
    // pi `getTuiMode`: `this.settings.tuiMode === "regular" ? "regular" :
    // "fullscreen"`. #1024 (T78) flipped the unset default to fullscreen, so
    // the resolution now matches pi with no divergence.
    const auto& stored = impl_->merged_settings.tui_mode;
    if (stored) {
        return *stored == "regular" ? TuiMode::Regular : TuiMode::Fullscreen;
    }
    return TuiMode::Fullscreen;
}

support::ExpectedVoid SettingsManager::set_tui_mode(TuiMode mode) {
    // pi `setTuiMode` always writes the global scope.
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    const std::string value = mode == TuiMode::Regular ? "regular" : "fullscreen";
    auto& target = impl_->global_settings;
    if (target.tui_mode == value) {
        return support::ExpectedVoid{};
    }

    // Persist first; the in-memory view advances only when the surgical write
    // succeeded, so a persist failure never leaves memory diverged from disk.
    if (auto persisted = impl_->persist_global_field("tuiMode", support::JsonValue{value}); !persisted) {
        return persisted;
    }
    target.tui_mode = value;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
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

std::string SettingsManager::codemode_mode() const {
    // pi `getCodemodeMode`: `this.settings.codemode?.mode ?? "on"`.
    const auto& codemode = impl_->merged_settings.codemode;
    return codemode && codemode->mode ? *codemode->mode : std::string{"on"};
}

std::optional<std::int64_t> SettingsManager::codemode_inline_budget() const noexcept {
    // pi `getCodemodeInlineBudget`: `this.settings.codemode?.inlineBudget`.
    const auto& codemode = impl_->merged_settings.codemode;
    return codemode ? codemode->inline_budget : std::nullopt;
}

std::string SettingsManager::get_or_create_device_id() {
    // pi `getOrCreateDeviceId`: read the global scope only (project settings
    // are ignored so a committed project file cannot clone the ID); create and
    // persist a fresh UUID v4 on first use.
    if (impl_->global_settings.device_id && !impl_->global_settings.device_id->empty()) {
        return *impl_->global_settings.device_id;
    }
    auto created = new_device_id();
    if (impl_->global_write_suppressed()) {
        // A global write failure never blocks login; the ID stays in memory
        // for this process only.
        impl_->global_settings.device_id = created;
        impl_->recompute_merged();
        return created;
    }
    if (auto persisted = impl_->persist_global_field("deviceId", support::JsonValue{created}); !persisted) {
        impl_->global_settings.device_id = created;
        impl_->recompute_merged();
        return created;
    }
    impl_->global_settings.device_id = std::move(created);
    impl_->recompute_merged();
    return *impl_->global_settings.device_id;
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

bool SettingsManager::get_show_hardware_cursor() const noexcept {
    if (impl_->merged_settings.show_hardware_cursor.has_value()) {
        return *impl_->merged_settings.show_hardware_cursor;
    }
    const char* env = std::getenv("PI_HARDWARE_CURSOR");
    return env != nullptr && std::string_view{env} == "1";
}

support::ExpectedVoid SettingsManager::set_show_hardware_cursor(bool enabled) {
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.show_hardware_cursor == enabled) {
        return support::ExpectedVoid{};
    }

    if (auto persisted = impl_->persist_global_field("showHardwareCursor", support::JsonValue{enabled}); !persisted) {
        return persisted;
    }
    target.show_hardware_cursor = enabled;
    impl_->recompute_merged();
    return support::ExpectedVoid{};
}

bool SettingsManager::get_show_terminal_progress() const noexcept {
    return impl_->merged_settings.show_terminal_progress.value_or(false);
}

support::ExpectedVoid SettingsManager::set_show_terminal_progress(bool enabled) {
    if (impl_->global_write_suppressed()) {
        return support::ExpectedVoid{};
    }
    auto& target = impl_->global_settings;
    if (target.show_terminal_progress == enabled) {
        return support::ExpectedVoid{};
    }

    if (auto persisted =
                    impl_->persist_global_nested_field("terminal", "showTerminalProgress", support::JsonValue{enabled});
            !persisted) {
        return persisted;
    }
    target.show_terminal_progress = enabled;
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
