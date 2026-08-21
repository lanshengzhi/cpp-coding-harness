#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/tui/Component.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

class LiveTheme;
struct RegisteredTheme;

/// pi `interactive-mode.ts` `showLoadedResources` subset (#418): the startup
/// container's Context/Skills/Prompts/Themes sections with pi's scope
/// grouping, compact name lists + expanded path lists, and the per-kind
/// diagnostics sections (`[Skill conflicts]`/`[Prompt conflicts]`/
/// `[Theme conflicts]`). Always shown (no `quietStartup`/`--verbose` in the
/// subset); expansion follows `app.tools.expand` like the header. There is
/// no Extensions section (no extensions surface, ADR 0036 G4).
class LoadedResources final : public cch::tui::Component {
public:
    /// One discovered skill (pi `Skill`: name + filePath + `sourceInfo`).
    struct SkillItem {
        std::string name;
        std::string path;
        SourceInfo source_info;
    };
    /// One discovered prompt template (pi `PromptTemplate`).
    struct TemplateItem {
        std::string name;
        std::string path;
        SourceInfo source_info;
    };
    /// One registered custom theme (pi `Theme.sourcePath` + `SourceInfo`).
    struct ThemeItem {
        std::string name;
        std::string path;
        SourceScope scope{SourceScope::Project};
    };

    /// Passive input rebuilt by the state (`refresh_loaded_resources`).
    struct Data {
        /// Session workspace (pi `sessionManager.getCwd()`) for the compact
        /// Context `formatContextPath` relative display.
        std::filesystem::path cwd;
        /// User home for `formatDisplayPath` (`~` shortening).
        std::filesystem::path home;
        /// Context sources in pi's order: `getSystemPromptSource()`, then
        /// `getAppendSystemPromptSources()`, then `getAgentsFiles()`.
        std::vector<std::string> context_paths;
        std::vector<SkillItem> skills;
        std::vector<TemplateItem> templates;
        /// Custom themes only (builtins have no source path and never render).
        std::vector<ThemeItem> themes;
        std::vector<ResourceDiagnostic> skill_diagnostics;
        std::vector<ResourceDiagnostic> prompt_diagnostics;
        std::vector<ResourceDiagnostic> theme_diagnostics;
    };

    /// The theme must outlive this component.
    explicit LoadedResources(const LiveTheme& theme);

    void set_data(Data data);
    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    /// pi `formatDisplayPath`: home → `~`.
    [[nodiscard]] std::string format_display_path(const std::string& path) const;
    /// pi `formatContextPath`: cwd-relative when possible, else `~`-home.
    [[nodiscard]] std::string format_context_path(const std::string& path) const;
    /// pi `formatPathWithSource`: `label[ (scope)] shortPath`.
    [[nodiscard]] std::string format_path_with_source(
        const std::string& path,
        const std::optional<SourceInfo>& source_info) const;
    /// The synthesized `SourceInfo` for one registered theme (pi
    /// `Theme.sourceInfo`; scope is the only provenance the loader carries).
    [[nodiscard]] static SourceInfo theme_source_info(const ThemeItem& theme_item);
    /// The sourceInfos map for `formatDiagnostics` (pi builds it from
    /// skills + prompts + themes).
    [[nodiscard]] std::map<std::string, SourceInfo> build_source_infos() const;
    /// pi `formatDiagnostics`: collision grouping by name plus
    /// path/message lines for the other diagnostics.
    [[nodiscard]] std::string format_diagnostics(
        const std::vector<ResourceDiagnostic>& diagnostics,
        const std::map<std::string, SourceInfo>& source_infos) const;

    const LiveTheme& theme_; // must outlive this component.
    Data data_;
    bool expanded_{false};
};

/// pi `showLoadedResources` data assembly (#506): collect the
/// loaded-resources block data from the live session (Context sources,
/// skills, templates), the registered custom themes (pi `getThemes().themes`
/// filtered to `sourcePath`), and the per-kind diagnostics (loader read
/// diagnostics plus the theme discovery diagnostics stashed at boot/reload).
[[nodiscard]] LoadedResources::Data collect_loaded_resources_data(
    const coding_agent::AgentSession& session,
    std::span<const RegisteredTheme> registered_themes,
    std::span<const ResourceDiagnostic> theme_discovery_diagnostics);

} // namespace cch::coding_agent::tui
