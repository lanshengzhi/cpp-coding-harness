#pragma once

#include <cch/tui/Input.hpp>
#include <cch/util/Error.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

enum class KeybindingPlatform {
    Linux,
    MacOS,
    Windows,
    Other,
};

struct KeybindingDefinition {
    std::string id{};
    std::vector<std::string> default_keys{};
    std::string description{};
    std::string category{};
    bool available{true};
    std::optional<std::string> unavailable_reason{std::nullopt};
};

struct KeybindingOverride {
    std::string id{};
    std::vector<std::string> keys{};
};

struct KeybindingIssue {
    std::string code{};
    std::string message{};
    std::optional<std::string> action_id{std::nullopt};
    std::optional<std::string> key{std::nullopt};
};

struct EffectiveKeybinding {
    std::string id{};
    std::vector<std::string> keys{};
    std::string description{};
    std::string category{};
    bool available{true};
    std::optional<std::string> unavailable_reason{std::nullopt};
};

class KeybindingRegistry final {
public:
    KeybindingRegistry(std::vector<EffectiveKeybinding> entries, KeybindingPlatform platform);

    [[nodiscard]] const EffectiveKeybinding* find(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> keys(std::string_view id) const;
    [[nodiscard]] bool matches(const KeyEvent& event, std::string_view id) const;
    [[nodiscard]] std::optional<std::string> first_match(
        const KeyEvent& event,
        std::span<const std::string_view> candidate_ids) const;
    [[nodiscard]] std::string key_text(std::string_view id) const;
    [[nodiscard]] const std::vector<EffectiveKeybinding>& entries() const;

private:
    std::vector<EffectiveKeybinding> entries_;
    KeybindingPlatform platform_{KeybindingPlatform::Other};
};

struct KeybindingResolutionRequest {
    std::vector<KeybindingDefinition> definitions{};
    std::vector<KeybindingOverride> overrides{};
    KeybindingPlatform platform{KeybindingPlatform::Other};
};

struct KeybindingResolution {
    std::shared_ptr<const KeybindingRegistry> registry{};
    std::vector<KeybindingIssue> issues{};
};

/// Resolve one immutable registry. User entries replace only their action's
/// defaults. When multiple user actions claim one key, callers resolve it by
/// candidate order and the returned issues report the conflict.
[[nodiscard]] util::Expected<KeybindingResolution> resolve_keybindings(KeybindingResolutionRequest request);

/// Reusable actions implemented by cch_tui, pinned to pi baseline 864b35c.
[[nodiscard]] std::vector<KeybindingDefinition> builtin_tui_keybinding_definitions();
[[nodiscard]] std::shared_ptr<const KeybindingRegistry> default_tui_keybindings();
[[nodiscard]] KeybindingPlatform native_keybinding_platform();

} // namespace cch::tui
