#include "KeybindingsManager.hpp"

#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "support/Json.hpp"

#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

struct ApplicationTemplate {
    std::string_view id;
    std::vector<std::string> keys;
    std::string_view description;
    std::string_view category;
};

[[nodiscard]] ApplicationTemplate make_application_template(
    std::string_view id,
    std::vector<std::string> keys,
    std::string_view description,
    std::string_view category) {
    return {
        .id = id,
        .keys = std::move(keys),
        .description = description,
        .category = category,
    };
}

/// The app layer adopts pi's full 42-action `AppKeybindings` table
/// (pi:packages/coding-agent/src/core/keybindings.ts at `83114817`, ADR 0036
/// G2). Descriptions and default keys are pi-verbatim; the category column is
/// the C++ `/hotkeys` presentation grouping (pi's `handleHotkeysCommand`
/// renders hardcoded sections instead).
[[nodiscard]] const std::vector<ApplicationTemplate>& application_templates() {
    static const std::vector<ApplicationTemplate> kTemplates{
        make_application_template("app.interrupt", {"escape"}, "Cancel or abort", "Application"),
        make_application_template("app.clear", {"ctrl+c"}, "Clear editor", "Application"),
        make_application_template("app.exit", {"ctrl+d"}, "Exit when editor is empty", "Application"),
        make_application_template("app.suspend", {"ctrl+z"}, "Suspend to background", "Application"),
        make_application_template("app.thinking.cycle", {"shift+tab"}, "Cycle thinking level", "Models and thinking"),
        make_application_template("app.model.cycleForward", {"ctrl+p"}, "Cycle to next model", "Models and thinking"),
        make_application_template(
            "app.model.cycleBackward",
            {"shift+ctrl+p"},
            "Cycle to previous model",
            "Models and thinking"),
        make_application_template("app.model.select", {"ctrl+l"}, "Open model selector", "Models and thinking"),
        make_application_template("app.tools.expand", {"ctrl+o"}, "Toggle tool output", "Display and queue"),
        make_application_template("app.thinking.toggle", {"ctrl+t"}, "Toggle thinking blocks", "Display and queue"),
        make_application_template(
            "app.session.toggleNamedFilter",
            {"ctrl+n"},
            "Toggle named session filter",
            "Sessions"),
        make_application_template("app.editor.external", {"ctrl+g"}, "Open external editor", "Application"),
        make_application_template("app.message.copy", {"ctrl+x"}, "Copy message to clipboard", "Display and queue"),
        make_application_template(
            "app.message.followUp",
            {"alt+enter"},
            "Queue follow-up message",
            "Display and queue"),
        make_application_template("app.message.dequeue", {"alt+up"}, "Restore queued messages", "Display and queue"),
        make_application_template("app.clipboard.pasteImage", {"ctrl+v"}, "Paste image from clipboard (text fallback)", "Application"),
        make_application_template("app.session.new", {}, "Start a new session", "Sessions"),
        make_application_template("app.session.tree", {}, "Open session tree", "Sessions"),
        make_application_template("app.session.fork", {}, "Fork current session", "Sessions"),
        make_application_template("app.session.resume", {}, "Resume a session", "Sessions"),
        make_application_template(
            "app.tree.foldOrUp",
            {"ctrl+left", "alt+left"},
            "Fold tree branch or move up",
            "Tree navigation"),
        make_application_template(
            "app.tree.unfoldOrDown",
            {"ctrl+right", "alt+right"},
            "Unfold tree branch or move down",
            "Tree navigation"),
        make_application_template("app.tree.editLabel", {"shift+l"}, "Edit tree label", "Tree navigation"),
        make_application_template(
            "app.tree.toggleLabelTimestamp",
            {"shift+t"},
            "Toggle tree label timestamps",
            "Tree navigation"),
        make_application_template("app.session.togglePath", {"ctrl+p"}, "Toggle session path display", "Sessions"),
        make_application_template("app.session.toggleSort", {"ctrl+s"}, "Toggle session sort mode", "Sessions"),
        make_application_template("app.session.rename", {"ctrl+r"}, "Rename session", "Sessions"),
        make_application_template("app.session.delete", {"ctrl+d"}, "Delete session", "Sessions"),
        make_application_template(
            "app.session.deleteNoninvasive",
            {"ctrl+backspace"},
            "Delete session when query is empty",
            "Sessions"),
        make_application_template("app.models.save", {"ctrl+s"}, "Save model selection", "Scoped models"),
        make_application_template("app.models.enableAll", {"ctrl+a"}, "Enable all models", "Scoped models"),
        make_application_template("app.models.clearAll", {"ctrl+x"}, "Clear all models", "Scoped models"),
        make_application_template(
            "app.models.toggleProvider",
            {"ctrl+p"},
            "Toggle all models for provider",
            "Scoped models"),
        make_application_template("app.models.reorderUp", {"alt+up"}, "Move model up in order", "Scoped models"),
        make_application_template("app.models.reorderDown", {"alt+down"}, "Move model down in order", "Scoped models"),
        make_application_template(
            "app.tree.filter.default",
            {"ctrl+d"},
            "Tree filter: default view",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.noTools",
            {"ctrl+t"},
            "Tree filter: hide tool results",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.userOnly",
            {"ctrl+u"},
            "Tree filter: user messages only",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.labeledOnly",
            {"ctrl+l"},
            "Tree filter: labeled entries only",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.all",
            {"ctrl+a"},
            "Tree filter: show all entries",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.cycleForward",
            {"ctrl+o"},
            "Tree filter: cycle forward",
            "Tree navigation"),
        make_application_template(
            "app.tree.filter.cycleBackward",
            {"shift+ctrl+o"},
            "Tree filter: cycle backward",
            "Tree navigation"),
    };
    return kTemplates;
}

[[nodiscard]] const ApplicationTemplate* find_application_template(std::string_view id) {
    const auto& templates = application_templates();
    const auto found = std::find_if(templates.begin(), templates.end(), [id](const auto& candidate) {
        return candidate.id == id;
    });
    return found == templates.end() ? nullptr : &*found;
}

[[nodiscard]] bool known_unassembled_id(std::string_view id) {
    if (cch::tui::is_known_unassembled_tui_keybinding(id)) return true;
    return find_application_template(id) != nullptr;
}

void add_diagnostic(
    std::vector<KeybindingDiagnostic>& diagnostics,
    std::string code,
    std::string message,
    const std::filesystem::path& path) {
    detail::bound_resource_diagnostic_text(message);
    auto bounded_path = path.string();
    detail::bound_resource_diagnostic_text(bounded_path);
    diagnostics.push_back({
        .severity = KeybindingDiagnosticSeverity::Warning,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(bounded_path),
    });
}

void bound_diagnostics(std::vector<KeybindingDiagnostic>& diagnostics) {
    if (diagnostics.size() <= detail::kMaxResourceDiagnostics) return;
    diagnostics.resize(detail::kMaxResourceDiagnostics - 1);
    diagnostics.push_back({
        .severity = KeybindingDiagnosticSeverity::Warning,
        .code = "diagnostics_truncated",
        .message = "Additional keybinding diagnostics were omitted",
        .path = {},
    });
}

struct ParsedOverrides {
    std::vector<cch::tui::KeybindingOverride> overrides{};
    std::vector<KeybindingDiagnostic> diagnostics{};
};

[[nodiscard]] ParsedOverrides parse_overrides(
    const support::JsonValue::object_t& object,
    const std::set<std::string, std::less<>>& assembled_ids,
    const std::filesystem::path& path) {
    ParsedOverrides result;
    for (const auto& [id, value] : object) {
        if (!assembled_ids.contains(id)) {
            add_diagnostic(
                result.diagnostics,
                known_unassembled_id(id) ? "unavailable_action" : "unknown_action",
                known_unassembled_id(id)
                    ? std::format("keybinding action '{}' is not assembled", id)
                    : std::format("unknown keybinding action '{}'", id),
                path);
            continue;
        }

        std::vector<std::string> keys;
        bool valid_value = true;
        if (const auto* key = value.get_if<std::string>()) {
            keys.push_back(*key);
        } else if (value.holds<support::JsonValue::array_t>()) {
            for (const auto& item : value.get_array()) {
                if (const auto* key = item.get_if<std::string>()) {
                    keys.push_back(*key);
                } else {
                    valid_value = false;
                    break;
                }
            }
        } else {
            valid_value = false;
        }
        if (!valid_value) {
            add_diagnostic(
                result.diagnostics,
                "invalid_binding_value",
                std::format("keybinding action '{}' must use a string or string array", id),
                path);
            continue;
        }
        result.overrides.push_back({.id = id, .keys = std::move(keys)});
    }
    return result;
}

[[nodiscard]] std::vector<cch::tui::KeybindingOverride> load_overrides(
    const std::filesystem::path& path,
    const std::set<std::string, std::less<>>& assembled_ids,
    std::vector<KeybindingDiagnostic>& diagnostics) {
    if (path.empty()) return {};
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error) {
        if (status_error != std::errc::no_such_file_or_directory) {
            add_diagnostic(
                diagnostics,
                "keybindings_unavailable",
                "could not inspect keybindings file: " + status_error.message(),
                path);
        }
        return {};
    }
    if (!std::filesystem::exists(status)) return {};
    if (!std::filesystem::is_regular_file(status)) {
        add_diagnostic(
            diagnostics,
            "keybindings_unavailable",
            "keybindings path is not a regular file",
            path);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        add_diagnostic(diagnostics, "keybindings_unavailable", "could not read keybindings file", path);
        return {};
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (auto parsed = support::read_json(content.str());
        !parsed || !parsed->holds<support::JsonValue::object_t>()) {
        add_diagnostic(
            diagnostics,
            "invalid_keybindings_document",
            "keybindings file must contain one JSON object",
            path);
        return {};
    } else {
        auto transformed = parse_overrides(parsed->get_object(), assembled_ids, path);
        diagnostics.insert(
            diagnostics.end(),
            std::make_move_iterator(transformed.diagnostics.begin()),
            std::make_move_iterator(transformed.diagnostics.end()));
        return std::move(transformed.overrides);
    }
}

} // namespace

support::Expected<std::vector<cch::tui::KeybindingDefinition>> app_keybinding_definitions(
    std::span<const std::string_view> assembled_action_ids) {
    std::vector<cch::tui::KeybindingDefinition> definitions;
    definitions.reserve(assembled_action_ids.size());
    for (const auto id : assembled_action_ids) {
        const auto* source = find_application_template(id);
        if (source == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                std::format("unknown baseline application keybinding '{}'", id)));
        }
        definitions.push_back({
            .id = std::string(source->id),
            .default_keys = source->keys,
            .description = std::string(source->description),
            .category = std::string(source->category),
        });
    }
    return definitions;
}

support::Expected<KeybindingsManagerResult> load_keybindings_manager(
    KeybindingsManagerRequest request) {
    auto definitions = cch::tui::builtin_tui_keybinding_definitions();
    definitions.insert(
        definitions.end(),
        std::make_move_iterator(request.application_definitions.begin()),
        std::make_move_iterator(request.application_definitions.end()));

    std::set<std::string, std::less<>> assembled_ids;
    for (const auto& definition : definitions) assembled_ids.insert(definition.id);

    std::vector<KeybindingDiagnostic> diagnostics;
    const auto path = request.agent_config_directory.empty()
        ? std::filesystem::path{}
        : request.agent_config_directory / "keybindings.json";
    auto overrides = load_overrides(path, assembled_ids, diagnostics);

    cch::tui::KeybindingResolutionRequest resolution_request;
    resolution_request.definitions = std::move(definitions);
    resolution_request.overrides = std::move(overrides);
    if (auto resolution = cch::tui::resolve_keybindings(std::move(resolution_request)); !resolution) {
        return std::unexpected(resolution.error());
    } else {
        for (auto& issue : resolution->issues) {
            add_diagnostic(diagnostics, std::move(issue.code), std::move(issue.message), path);
        }
        bound_diagnostics(diagnostics);
        return KeybindingsManagerResult{
            .registry = std::move(resolution->registry),
            .diagnostics = std::move(diagnostics),
        };
    }
}

std::vector<HotkeyHelpEntry> hotkey_help_entries(
    const cch::tui::KeybindingRegistry& registry) {
    std::vector<HotkeyHelpEntry> result;
    result.reserve(registry.entries().size());
    for (const auto& entry : registry.entries()) {
        const auto keys = registry.key_text(entry.id);
        result.push_back({
            .id = entry.id,
            .keys = entry.available ? (keys.empty() ? "Unbound" : keys) : keys,
            .description = entry.description,
            .category = entry.category,
        });
    }
    return result;
}

std::string key_hint(
    const cch::tui::KeybindingRegistry& registry,
    std::string_view action_id,
    std::string_view description) {
    const auto keys = registry.key_text(action_id);
    return std::format("{} {}", keys.empty() ? "Unbound" : keys, description);
}

std::unique_ptr<cch::tui::Component> make_hotkey_help_view(
    std::shared_ptr<const cch::tui::KeybindingRegistry> registry) {
    std::string text = "Hotkeys\n";
    if (registry) {
        auto entries = hotkey_help_entries(*registry);
        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            const auto left_application = left.category == "Application";
            const auto right_application = right.category == "Application";
            if (left_application != right_application) return left_application;
            if (left.category != right.category) return left.category < right.category;
            return left.id < right.id;
        });
        std::string category;
        for (const auto& entry : entries) {
            if (entry.category != category) {
                category = entry.category;
                text += "\n" + category + "\n";
            }
            text += std::format("{}  {} — {}\n", entry.keys, entry.id, entry.description);
        }
    }
    return std::make_unique<cch::tui::Text>(std::move(text));
}

} // namespace cch::coding_agent::tui
