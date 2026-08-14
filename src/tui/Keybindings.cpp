#include <cch/tui/Keybindings.hpp>

#include <cch/util/Error.hpp>
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

[[nodiscard]] util::Expected<std::vector<std::string>> canonical_keys(
    const std::vector<std::string>& keys,
    std::string_view action_id) {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const auto& key : keys) {
        if (auto parsed = parse_key_id(key); !parsed) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                std::format("keybinding '{}' has invalid key '{}'", action_id, key),
                key));
        } else {
            auto canonical = key_id(*parsed);
            if (seen.insert(canonical).second) result.push_back(std::move(canonical));
        }
    }
    return result;
}

[[nodiscard]] std::string display_key(std::string_view key, KeybindingPlatform platform) {
    if (platform != KeybindingPlatform::MacOS) return std::string(key);
    std::string result;
    std::size_t start = 0;
    while (start <= key.size()) {
        const auto separator = key.find('+', start);
        const auto end = separator == std::string_view::npos ? key.size() : separator;
        if (!result.empty()) result.push_back('+');
        const auto part = key.substr(start, end - start);
        result += part == "alt" ? "option" : std::string(part);
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return result;
}

[[nodiscard]] util::Error definition_error(std::string message) {
    return util::make_error(util::ErrorCode::Validation, std::move(message));
}

[[nodiscard]] KeybindingDefinition make_definition(
    std::string id,
    std::vector<std::string> default_keys,
    std::string description,
    std::string category) {
    return {
        .id = std::move(id),
        .default_keys = std::move(default_keys),
        .description = std::move(description),
        .category = std::move(category),
    };
}

} // namespace

KeybindingRegistry::KeybindingRegistry(
    std::vector<EffectiveKeybinding> entries,
    KeybindingPlatform platform)
    : entries_(std::move(entries)), platform_(platform) {}

const EffectiveKeybinding* KeybindingRegistry::find(std::string_view id) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [id](const auto& entry) {
        return entry.id == id;
    });
    return found == entries_.end() ? nullptr : &*found;
}

std::vector<std::string> KeybindingRegistry::keys(std::string_view id) const {
    const auto* entry = find(id);
    return entry == nullptr ? std::vector<std::string>{} : entry->keys;
}

bool KeybindingRegistry::matches(const KeyEvent& event, std::string_view id) const {
    const auto* entry = find(id);
    if (entry == nullptr || !entry->available) return false;
    return std::any_of(entry->keys.begin(), entry->keys.end(), [&event](const auto& key) {
        return matches_key(event, key);
    });
}

std::optional<std::string> KeybindingRegistry::first_match(
    const KeyEvent& event,
    std::span<const std::string_view> candidate_ids) const {
    for (const auto id : candidate_ids) {
        if (matches(event, id)) return std::string(id);
    }
    return std::nullopt;
}

std::string KeybindingRegistry::key_text(std::string_view id) const {
    const auto* entry = find(id);
    if (entry == nullptr) return {};
    if (!entry->available) return entry->unavailable_reason.value_or("Unavailable");
    std::string text;
    for (const auto& key : entry->keys) {
        if (!text.empty()) text.push_back('/');
        text += display_key(key, platform_);
    }
    return text;
}

const std::vector<EffectiveKeybinding>& KeybindingRegistry::entries() const {
    return entries_;
}

util::Expected<KeybindingResolution> resolve_keybindings(KeybindingResolutionRequest request) {
    std::map<std::string, std::size_t, std::less<>> definition_indices;
    std::vector<EffectiveKeybinding> entries;
    entries.reserve(request.definitions.size());
    for (auto& definition : request.definitions) {
        if (definition.id.empty()) {
            return std::unexpected(definition_error("keybinding definition has an empty id"));
        }
        if (!definition_indices.emplace(definition.id, entries.size()).second) {
            return std::unexpected(definition_error(
                std::format("duplicate keybinding definition '{}'", definition.id)));
        }
        if (auto defaults = canonical_keys(definition.default_keys, definition.id); !defaults) {
            return std::unexpected(definition_error(
                std::format("keybinding definition '{}' has an invalid default", definition.id)));
        } else {
            entries.push_back({
                .id = std::move(definition.id),
                .keys = std::move(*defaults),
                .description = std::move(definition.description),
                .category = std::move(definition.category),
                .available = definition.available,
                .unavailable_reason = std::move(definition.unavailable_reason),
            });
        }
    }

    std::vector<KeybindingIssue> issues;
    std::set<std::string, std::less<>> overridden_actions;
    std::map<std::string, std::vector<std::string>, std::less<>> user_claims;
    for (const auto& override : request.overrides) {
        const auto definition = definition_indices.find(override.id);
        if (definition == definition_indices.end()) {
            if (is_known_unassembled_tui_keybinding(override.id)) {
                issues.push_back({
                    .code = "unavailable_action",
                    .message = std::format(
                        "keybinding action '{}' is recognized but not assembled by cch_tui",
                        override.id),
                    .action_id = override.id,
                });
            } else {
                issues.push_back({
                    .code = "unknown_action",
                    .message = std::format("unknown keybinding action '{}'", override.id),
                    .action_id = override.id,
                });
            }
            continue;
        }
        auto& entry = entries[definition->second];
        if (!entry.available) {
            issues.push_back({
                .code = "unavailable_action",
                .message = std::format("keybinding action '{}' is unavailable", override.id),
                .action_id = override.id,
            });
            continue;
        }
        if (!overridden_actions.insert(override.id).second) {
            issues.push_back({
                .code = "duplicate_override",
                .message = std::format("duplicate keybinding override '{}' was skipped", override.id),
                .action_id = override.id,
            });
            continue;
        }
        if (auto keys = canonical_keys(override.keys, override.id); !keys) {
            issues.push_back({
                .code = "invalid_key",
                .message = keys.error().message,
                .action_id = override.id,
                .key = keys.error().detail,
            });
            overridden_actions.erase(override.id);
        } else {
            entry.keys = std::move(*keys);
            for (const auto& key : entry.keys) user_claims[key].push_back(entry.id);
        }
    }

    for (const auto& [key, claimants] : user_claims) {
        if (claimants.size() > 1) {
            std::string actions;
            for (const auto& claimant : claimants) {
                if (!actions.empty()) actions += ", ";
                actions += claimant;
            }
            issues.push_back({
                .code = "conflicting_user_key",
                .message = std::format("key '{}' is assigned to multiple user actions: {}", key, actions),
                .key = key,
            });
        }
    }

    return KeybindingResolution{
        .registry = std::make_shared<const KeybindingRegistry>(std::move(entries), request.platform),
        .issues = std::move(issues),
    };
}

std::vector<KeybindingDefinition> builtin_tui_keybinding_definitions() {
    // Compatibility baseline: pi 83114817, packages/tui/src/keybindings.ts.
    return {
        make_definition("tui.editor.cursorUp", {"up"}, "Move cursor up", "Editor cursor"),
        make_definition("tui.editor.cursorDown", {"down"}, "Move cursor down", "Editor cursor"),
        make_definition("tui.editor.cursorLeft", {"left", "ctrl+b"}, "Move cursor left", "Editor cursor"),
        make_definition("tui.editor.cursorRight", {"right", "ctrl+f"}, "Move cursor right", "Editor cursor"),
        make_definition(
            "tui.editor.cursorWordLeft",
            {"alt+left", "ctrl+left", "alt+b"},
            "Move cursor word left",
            "Editor cursor"),
        make_definition(
            "tui.editor.cursorWordRight",
            {"alt+right", "ctrl+right", "alt+f"},
            "Move cursor word right",
            "Editor cursor"),
        make_definition("tui.editor.cursorLineStart", {"home", "ctrl+a"}, "Move to line start", "Editor cursor"),
        make_definition("tui.editor.cursorLineEnd", {"end", "ctrl+e"}, "Move to line end", "Editor cursor"),
        make_definition("tui.editor.jumpForward", {"ctrl+]"}, "Jump forward to character", "Editor cursor"),
        make_definition("tui.editor.jumpBackward", {"ctrl+alt+]"}, "Jump backward to character", "Editor cursor"),
        make_definition("tui.editor.pageUp", {"pageUp"}, "Page up", "Editor cursor"),
        make_definition("tui.editor.pageDown", {"pageDown"}, "Page down", "Editor cursor"),
        make_definition("tui.editor.deleteCharBackward", {"backspace"}, "Delete character backward", "Editor deletion"),
        make_definition(
            "tui.editor.deleteCharForward",
            {"delete", "ctrl+d"},
            "Delete character forward",
            "Editor deletion"),
        make_definition(
            "tui.editor.deleteWordBackward",
            {"ctrl+w", "alt+backspace"},
            "Delete word backward",
            "Editor deletion"),
        make_definition(
            "tui.editor.deleteWordForward",
            {"alt+d", "alt+delete"},
            "Delete word forward",
            "Editor deletion"),
        make_definition("tui.editor.deleteToLineStart", {"ctrl+u"}, "Delete to line start", "Editor deletion"),
        make_definition("tui.editor.deleteToLineEnd", {"ctrl+k"}, "Delete to line end", "Editor deletion"),
        make_definition("tui.editor.yank", {"ctrl+y"}, "Yank", "Editor kill ring"),
        make_definition("tui.editor.yankPop", {"alt+y"}, "Yank pop", "Editor kill ring"),
        make_definition("tui.editor.undo", {"ctrl+-"}, "Undo", "Editor kill ring"),
        make_definition("tui.input.newLine", {"shift+enter", "ctrl+j"}, "Insert newline", "Input"),
        make_definition("tui.input.submit", {"enter"}, "Submit input", "Input"),
        make_definition("tui.input.tab", {"tab"}, "Tab / autocomplete", "Input"),
        make_definition("tui.select.up", {"up"}, "Move selection up", "Selection"),
        make_definition("tui.select.down", {"down"}, "Move selection down", "Selection"),
        make_definition("tui.select.pageUp", {"pageUp"}, "Selection page up", "Selection"),
        make_definition("tui.select.pageDown", {"pageDown"}, "Selection page down", "Selection"),
        make_definition("tui.select.confirm", {"enter"}, "Confirm selection", "Selection"),
        make_definition("tui.select.cancel", {"escape", "ctrl+c"}, "Cancel selection", "Selection"),
    };
}

bool is_known_unassembled_tui_keybinding(std::string_view id) {
    return id == "tui.input.copy" ||
        id == "tui.altScreen.pageUp" || id == "tui.altScreen.pageDown" ||
        id == "tui.altScreen.previousPrompt" || id == "tui.altScreen.nextPrompt" ||
        id == "tui.altScreen.top" || id == "tui.altScreen.bottom";
}

std::shared_ptr<const KeybindingRegistry> default_tui_keybindings() {
    static const auto kRegistry = [] {
        auto definitions = builtin_tui_keybinding_definitions();
        std::vector<EffectiveKeybinding> entries;
        entries.reserve(definitions.size());
        for (auto& definition : definitions) {
            entries.push_back({
                .id = std::move(definition.id),
                .keys = std::move(definition.default_keys),
                .description = std::move(definition.description),
                .category = std::move(definition.category),
                .available = definition.available,
                .unavailable_reason = std::move(definition.unavailable_reason),
            });
        }
        return std::make_shared<const KeybindingRegistry>(
            std::move(entries),
            native_keybinding_platform());
    }();
    return kRegistry;
}

KeybindingPlatform native_keybinding_platform() {
#if defined(__APPLE__)
    return KeybindingPlatform::MacOS;
#elif defined(_WIN32)
    return KeybindingPlatform::Windows;
#elif defined(__linux__)
    return KeybindingPlatform::Linux;
#else
    return KeybindingPlatform::Other;
#endif
}

} // namespace cch::tui
