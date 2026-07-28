#pragma once

#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

enum class KeybindingDiagnosticSeverity {
    Warning,
};

struct KeybindingDiagnostic {
    KeybindingDiagnosticSeverity severity{KeybindingDiagnosticSeverity::Warning};
    std::string code{};
    std::string message{};
    std::string path{};
};

struct KeybindingCatalogRequest {
    std::filesystem::path agent_config_directory{};
    /// Concrete application actions supplied by the assembling frontend.
    /// Omitted application actions are unavailable and are never registered.
    std::vector<cch::tui::KeybindingDefinition> application_definitions{};
    cch::tui::KeybindingPlatform platform{cch::tui::native_keybinding_platform()};
};

struct KeybindingCatalogResult {
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry{};
    std::vector<KeybindingDiagnostic> diagnostics{};
};

/// Load exactly <Agent Config Directory>/keybindings.json and resolve one
/// startup registry. No pi state directory or project path is discovered.
[[nodiscard]] util::Expected<KeybindingCatalogResult> load_keybinding_catalog(
    KeybindingCatalogRequest request);

/// Return baseline definitions only for the concrete application action IDs
/// selected by an assembling frontend. Unknown IDs fail rather than creating
/// placeholders. Platform-specific defaults remain explicit in the result.
[[nodiscard]] util::Expected<std::vector<cch::tui::KeybindingDefinition>>
baseline_application_keybindings(
    std::span<const std::string_view> assembled_action_ids,
    cch::tui::KeybindingPlatform platform);

} // namespace cch::coding_agent::tui
