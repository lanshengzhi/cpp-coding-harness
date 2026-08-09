#include "coding_agent/prompt/SystemPromptBuilder.hpp"

#include "coding_agent/SkillFormatting.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cch::coding_agent::prompt {
namespace {

/// pi's default active tool set (`agent-session.ts`
/// `defaultActiveToolNames`), applied when `selectedTools` is absent.
const std::vector<std::string> kDefaultToolNames{"read", "bash", "edit", "write"};

/// pi `buildSystemPrompt`: `cwd.replace(/\\/g, "/")`.
[[nodiscard]] std::string posix_normalize_cwd(std::string cwd) {
    std::replace(cwd.begin(), cwd.end(), '\\', '/');
    return cwd;
}

/// pi `String.prototype.trim()` on a guideline bullet.
[[nodiscard]] std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

/// Appends the `<project_context>` section (pi `buildSystemPrompt`): the
/// fixed prose line plus one `<project_instructions path="...">` block per
/// file, each followed by a blank line, then the closing tag.
void append_project_context(
    std::string& prompt,
    const std::vector<ProjectContextFile>& context_files) {
    prompt += "\n\n<project_context>\n\n";
    prompt += "Project-specific instructions and guidelines:\n\n";
    for (const auto& file : context_files) {
        prompt += "<project_instructions path=\"";
        prompt += file.path;
        prompt += "\">\n";
        prompt += file.content;
        prompt += "\n</project_instructions>\n\n";
    }
    prompt += "</project_context>\n";
}

/// Appends the skills section (pi `buildSystemPrompt` → `formatSkillsForPrompt`).
void append_skills_section(
    std::string& prompt,
    const std::vector<Skill>& skills) {
    const std::string block = formatSkillsForPrompt(skills);
    if (!block.empty()) {
        prompt += block;
    }
}

} // namespace

std::string buildSystemPrompt(const BuildSystemPromptOptions& options) {
    const std::string prompt_cwd = posix_normalize_cwd(options.cwd);
    // pi `appendSystemPrompt ? "\n\n" + appendSystemPrompt : ""` — an
    // absent or empty append adds no section.
    const bool has_append =
        options.appendSystemPrompt && !options.appendSystemPrompt->empty();
    const std::string append_section =
        has_append ? "\n\n" + *options.appendSystemPrompt : "";

    const auto& context_files = options.contextFiles;
    const auto& skills = options.skills;

    // pi `if (customPrompt)`: the JS-truthy check — an absent or empty custom
    // prompt takes the default branch.
    const bool has_custom_prompt =
        options.customPrompt && !options.customPrompt->empty();
    if (has_custom_prompt) {
        std::string prompt = *options.customPrompt;

        if (!append_section.empty()) {
            prompt += append_section;
        }

        // Append project context files
        if (!context_files.empty()) {
            append_project_context(prompt, context_files);
        }

        // Append skills section (only if read tool is available; pi:
        // `!selectedTools || selectedTools.includes("read")`)
        const bool custom_prompt_has_read =
            !options.selectedTools ||
            std::find(
                options.selectedTools->begin(),
                options.selectedTools->end(),
                "read") != options.selectedTools->end();
        if (custom_prompt_has_read && !skills.empty()) {
            append_skills_section(prompt, skills);
        }

        prompt += "\nCurrent working directory: ";
        prompt += prompt_cwd;
        return prompt;
    }

    // pi: `selectedTools || ["read", "bash", "edit", "write"]`. The default
    // names apply only when the caller selected none (absent); an explicitly
    // empty list keeps no tools, even though no snippets are known for the
    // default names either.
    const std::vector<std::string>& tools = options.selectedTools
        ? *options.selectedTools
        : kDefaultToolNames;

    // A tool appears in Available tools only when a one-line snippet exists.
    std::string tools_list;
    {
        std::vector<std::string> visible_tools;
        visible_tools.reserve(tools.size());
        for (const auto& name : tools) {
            const auto snippet = options.toolSnippets.find(name);
            if (snippet != options.toolSnippets.end()) {
                visible_tools.push_back("- " + name + ": " + snippet->second);
            }
        }
        if (visible_tools.empty()) {
            tools_list = "(none)";
        } else {
            tools_list.reserve(visible_tools.size() * 32);
            for (std::size_t index = 0; index < visible_tools.size(); ++index) {
                if (index > 0) {
                    tools_list += "\n";
                }
                tools_list += visible_tools[index];
            }
        }
    }

    // Build guidelines based on which tools are actually available. pi
    // dedupes through a Set while preserving first-occurrence order.
    std::vector<std::string> guidelines_list;
    guidelines_list.reserve(tools.size() + 4);
    const auto add_guideline = [&guidelines_list](std::string guideline) {
        for (const auto& existing : guidelines_list) {
            if (existing == guideline) {
                return;
            }
        }
        guidelines_list.push_back(std::move(guideline));
    };

    const bool has_bash =
        std::find(tools.begin(), tools.end(), "bash") != tools.end();
    const bool has_grep =
        std::find(tools.begin(), tools.end(), "grep") != tools.end();
    const bool has_find =
        std::find(tools.begin(), tools.end(), "find") != tools.end();
    const bool has_ls =
        std::find(tools.begin(), tools.end(), "ls") != tools.end();
    const bool has_read =
        std::find(tools.begin(), tools.end(), "read") != tools.end();

    // File exploration guidelines
    if (has_bash && !has_grep && !has_find && !has_ls) {
        add_guideline("Use bash for file operations like ls, rg, find");
    }

    for (const auto& guideline : options.promptGuidelines) {
        const auto normalized = trim(guideline);
        if (!normalized.empty()) {
            add_guideline(normalized);
        }
    }

    // Always include these
    add_guideline("Be concise in your responses");
    add_guideline("Show file paths clearly when working with files");

    std::string guidelines;
    guidelines.reserve(guidelines_list.size() * 48);
    for (std::size_t index = 0; index < guidelines_list.size(); ++index) {
        if (index > 0) {
            guidelines += "\n";
        }
        guidelines += "- ";
        guidelines += guidelines_list[index];
    }

    // The identity line and documentation block carry the C++ binary's own
    // ("cch") identity and docs paths — the only delta from pi's verbatim
    // block (pinned by the differential golden; ADR 0036 G4).
    std::string prompt =
        "You are an expert coding assistant operating inside cch, a coding "
        "agent harness. You help users by reading files, executing commands, "
        "editing code, and writing new files.\n"
        "\n"
        "Available tools:\n";
    prompt += tools_list;
    prompt +=
        "\n"
        "\n"
        "In addition to the tools above, you may have access to other custom "
        "tools depending on the project.\n"
        "\n"
        "Guidelines:\n";
    prompt += guidelines;
    prompt +=
        "\n"
        "\n"
        "cch documentation (read only when the user asks about cch itself, "
        "its SDK, extensions, themes, skills, or TUI):\n"
        "- Main documentation: ";
    prompt += options.readmePath;
    prompt +=
        "\n"
        "- Additional docs: ";
    prompt += options.docsPath;
    prompt +=
        "\n"
        "- Examples: ";
    prompt += options.examplesPath;
    prompt +=
        " (extensions, custom tools, SDK)\n"
        "- When reading cch docs or examples, resolve docs/... under "
        "Additional docs and examples/... under Examples, not the current "
        "working directory\n"
        "- When asked about: extensions (docs/extensions.md, "
        "examples/extensions/), themes (docs/themes.md), skills "
        "(docs/skills.md), prompt templates (docs/prompt-templates.md), TUI "
        "components (docs/tui.md), keybindings (docs/keybindings.md), SDK "
        "integrations (docs/sdk.md), custom providers "
        "(docs/custom-provider.md), adding models (docs/models.md), cch "
        "packages (docs/packages.md), environment variables "
        "(docs/environment-variables.md)\n"
        "- When working on cch topics, read the docs and examples, and "
        "follow .md cross-references before implementing\n"
        "- Always read cch .md files completely and follow links to related "
        "docs (e.g., tui.md for TUI API details)";

    if (!append_section.empty()) {
        prompt += append_section;
    }

    // Append project context files
    if (!context_files.empty()) {
        append_project_context(prompt, context_files);
    }

    // Append skills section (only if read tool is available)
    if (has_read && !skills.empty()) {
        append_skills_section(prompt, skills);
    }

    prompt += "\nCurrent working directory: ";
    prompt += prompt_cwd;
    return prompt;
}

} // namespace cch::coding_agent::prompt
