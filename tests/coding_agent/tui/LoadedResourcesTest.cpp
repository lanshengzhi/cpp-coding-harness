// The loaded-resources presentation (#418, pi `interactive-mode.ts`
// `showLoadedResources`): Context/Skills/Prompts/Themes sections with pi's
// scope grouping, compact name lists + expanded path lists, and the per-kind
// diagnostics sections (`[Skill conflicts]`/`[Prompt conflicts]`/
// `[Theme conflicts]`). Rendered through a LiveTheme and asserted on the
// ANSI-stripped screen text.

#include "coding_agent/tui/LoadedResources.hpp"
#include "coding_agent/tui/ReloadBox.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

/// Renders the component and strips ANSI escapes for text assertions.
[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && text[index] != 'm') {
                ++index;
            }
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string screen_of(coding_agent::tui::LoadedResources& resources) {
    const auto rendered = resources.render(200);
    REQUIRE(rendered.has_value());
    std::string text;
    for (const auto& line : rendered->lines) {
        text += strip_ansi(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] coding_agent::SourceInfo source_info(
    coding_agent::SourceScope scope,
    std::string source,
    std::optional<std::string> base_dir = std::nullopt) {
    return coding_agent::SourceInfo{
        .path = {},
        .source = std::move(source),
        .scope = scope,
        .origin = coding_agent::SourceOrigin::TopLevel,
        .base_dir = std::move(base_dir),
    };
}

/// A fixture with one resource per source scope plus a skill collision
/// diagnostic.
[[nodiscard]] coding_agent::tui::LoadedResources::Data sample_data() {
    coding_agent::tui::LoadedResources::Data data;
    data.cwd = "/work/proj";
    data.home = "/home/user";
    // Context: SYSTEM source, APPEND sources, and Project Context Files.
    data.context_paths = {
        "/work/proj/.pi/SYSTEM.md",
        "/work/proj/AGENTS.md",
        "/home/user/.pi/agent/AGENTS.md",
    };
    data.skills = {
        {
            .name = "proj-skill",
            .path = "/work/proj/.pi/skills/proj-skill/SKILL.md",
            .source_info = source_info(
                coding_agent::SourceScope::Project, "auto", "/work/proj/.pi"),
        },
        {
            .name = "user-skill",
            .path = "/home/user/.pi/agent/skills/user-skill/SKILL.md",
            .source_info = source_info(
                coding_agent::SourceScope::User, "auto", "/home/user/.pi/agent"),
        },
        {
            .name = "cli-skill",
            .path = "/work/cli-skill/SKILL.md",
            .source_info = source_info(coding_agent::SourceScope::Temporary, "cli"),
        },
    };
    data.templates = {
        {
            .name = "proj-prompt",
            .path = "/work/proj/.pi/prompts/proj-prompt.md",
            .source_info = source_info(
                coding_agent::SourceScope::Project, "auto", "/work/proj/.pi"),
        },
        {
            .name = "user-prompt",
            .path = "/home/user/.pi/agent/prompts/user-prompt.md",
            .source_info = source_info(
                coding_agent::SourceScope::User, "auto", "/home/user/.pi/agent"),
        },
    };
    data.themes = {
        {
            .name = "proj-theme",
            .path = "/work/proj/.pi/themes/proj-theme.json",
            .scope = coding_agent::SourceScope::Project,
        },
        {
            .name = "cli-theme",
            .path = "/work/cli-theme.json",
            .scope = coding_agent::SourceScope::Temporary,
        },
    };
    data.skill_diagnostics = {
        coding_agent::ResourceDiagnostic{
            .type = coding_agent::ResourceDiagnosticType::Collision,
            .message = "name \"dup\" collision",
            .path = "/work/proj/.pi/skills/dup/SKILL.md",
            .collision = coding_agent::ResourceCollision{
                .resource_type = coding_agent::ResourceCollisionResourceType::Skill,
                .name = "dup",
                .winner_path = "/work/proj/.pi/skills/dup/SKILL.md",
                .loser_path = "/work/cli-dup/SKILL.md",
                .winner_source = std::nullopt,
                .loser_source = std::nullopt,
            },
        },
    };
    return data;
}

} // namespace

TEST_CASE(
    "LoadedResources renders compact sections with sorted names and cwd-relative Context",
    "[coding_agent][tui][loaded-resources][issue418]") {
    auto theme = test_theme();
    coding_agent::tui::LoadedResources resources(theme);
    resources.set_data(sample_data());
    resources.set_expanded(false);

    const auto screen = screen_of(resources);
    CHECK(screen.find("[Context]") != std::string::npos);
    CHECK(screen.find("[Skills]") != std::string::npos);
    CHECK(screen.find("[Prompts]") != std::string::npos);
    CHECK(screen.find("[Themes]") != std::string::npos);
    // No Extensions section in the C++ subset.
    CHECK(screen.find("[Extensions]") == std::string::npos);

    // Context compact: cwd-relative when possible, else `~`-home display,
    // UNSORTED in source order.
    CHECK(screen.find(".pi/SYSTEM.md, AGENTS.md, ~/.pi/agent/AGENTS.md") != std::string::npos);

    // Skills/Prompts/Themes compact: sorted name lists (dim `  a, b, c`).
    CHECK(screen.find("cli-skill, proj-skill, user-skill") != std::string::npos);
    CHECK(screen.find("/proj-prompt, /user-prompt") != std::string::npos);
    CHECK(screen.find("cli-theme, proj-theme") != std::string::npos);
    // Compact bodies carry names only, never the expanded path lists or
    // scope group labels (the expanded skill path is absent; the collision
    // diagnostics still render their winner/loser paths).
    CHECK(screen.find("proj-skill/SKILL.md") == std::string::npos);
    CHECK(screen.find("/work/proj/.pi/prompts/proj-prompt.md") == std::string::npos);

    // Diagnostics sections render (warning headers).
    CHECK(screen.find("[Skill conflicts]") != std::string::npos);
    CHECK(screen.find("\"dup\" collision:") != std::string::npos);
    CHECK(screen.find("✓") != std::string::npos);
    CHECK(screen.find("(skipped)") != std::string::npos);
}

TEST_CASE(
    "LoadedResources expands into scope groups with project, user, and path",
    "[coding_agent][tui][loaded-resources][issue418]") {
    auto theme = test_theme();
    coding_agent::tui::LoadedResources resources(theme);
    resources.set_data(sample_data());
    resources.set_expanded(true);

    const auto screen = screen_of(resources);
    CHECK(resources.expanded());

    // Context expanded: flat display paths in source order (no groups).
    CHECK(screen.find("/work/proj/.pi/SYSTEM.md") != std::string::npos);
    CHECK(screen.find("/work/proj/AGENTS.md") != std::string::npos);
    CHECK(screen.find("~/.pi/agent/AGENTS.md") != std::string::npos);

    // Skills expanded: project → user → path group labels with 4-space dim
    // paths.
    const auto project = screen.find("project");
    REQUIRE(project != std::string::npos);
    CHECK(screen.find("user", project) != std::string::npos);
    const auto user = screen.find("user", project);
    CHECK(screen.find("path", user) != std::string::npos);
    CHECK(screen.find("/work/proj/.pi/skills/proj-skill/SKILL.md") != std::string::npos);
    CHECK(screen.find("~/.pi/agent/skills/user-skill/SKILL.md") != std::string::npos);
    CHECK(screen.find("/work/cli-skill/SKILL.md") != std::string::npos);

    // Prompts expanded: `/<name>` labels inside their scope groups.
    CHECK(screen.find("/proj-prompt") != std::string::npos);
    CHECK(screen.find("/user-prompt") != std::string::npos);

    // Themes expanded: grouped by source scope (project + CLI → path group).
    CHECK(screen.find("/work/proj/.pi/themes/proj-theme.json") != std::string::npos);
    CHECK(screen.find("/work/cli-theme.json") != std::string::npos);

    // No compact `  a, b` lists when expanded.
    CHECK(screen.find("cli-skill, proj-skill, user-skill") == std::string::npos);
}

TEST_CASE(
    "LoadedResources renders prompt/theme conflicts and omits empty sections",
    "[coding_agent][tui][loaded-resources][issue418]") {
    auto theme = test_theme();
    coding_agent::tui::LoadedResources resources(theme);

    // Empty data renders zero lines (no sections).
    coding_agent::tui::LoadedResources::Data empty;
    empty.cwd = "/work";
    empty.home = "/home/user";
    resources.set_data(empty);
    const auto empty_screen = screen_of(resources);
    CHECK(empty_screen.empty());

    // Prompt + theme diagnostics render their conflict sections.
    auto data = sample_data();
    data.prompt_diagnostics = {
        coding_agent::ResourceDiagnostic{
            .type = coding_agent::ResourceDiagnosticType::Warning,
            .message = "prompt template file must use a .md extension",
            .path = "/work/bad.txt",
            .collision = std::nullopt,
        },
    };
    data.theme_diagnostics = {
        coding_agent::ResourceDiagnostic{
            .type = coding_agent::ResourceDiagnosticType::Warning,
            .message = "theme path does not exist",
            .path = "/work/missing-theme.json",
            .collision = std::nullopt,
        },
    };
    resources.set_data(data);
    const auto screen = screen_of(resources);
    CHECK(screen.find("[Prompt conflicts]") != std::string::npos);
    CHECK(screen.find("prompt template file must use a .md extension") != std::string::npos);
    CHECK(screen.find("[Theme conflicts]") != std::string::npos);
    CHECK(screen.find("theme path does not exist") != std::string::npos);
    CHECK(screen.find("/work/bad.txt") != std::string::npos);
    CHECK(screen.find("/work/missing-theme.json") != std::string::npos);
}

TEST_CASE(
    "reload box renders the pi-trimmed message with borders",
    "[coding_agent][tui][reload][issue418]") {
    auto theme = test_theme();
    auto box = coding_agent::tui::make_reload_box(theme);
    const auto rendered = box->render(100);
    REQUIRE(rendered.has_value());
    std::string text;
    for (const auto& line : rendered->lines) {
        text += strip_ansi(line);
        text.push_back('\n');
    }
    // The bordered box: a top border rule, the muted message, a bottom rule.
    CHECK(rendered->lines.size() >= 3);
    CHECK(text.find("─") != std::string::npos);
    CHECK(
        text.find(
            "Reloading keybindings, skills, prompts, themes, and context files...") !=
        std::string::npos);
    // "extensions" dropped (AC2).
    CHECK(text.find("extensions") == std::string::npos);
    CHECK(coding_agent::tui::kReloadBoxMessage ==
          "Reloading keybindings, skills, prompts, themes, and context files...");
}
