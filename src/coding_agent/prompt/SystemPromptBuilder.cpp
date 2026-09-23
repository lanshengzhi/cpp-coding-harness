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

/// pi's default preamble (`system-prompt.ts` `buildSystemPromptSections`).
constexpr std::string_view kDefaultPreamble =
        "You are an expert coding assistant operating inside pike, a coding agent "
        "harness. You help users by reading files, executing commands, editing "
        "code, and writing new files.";

/// pi `cwd.replace(/\\/g, "/")`.
[[nodiscard]] std::string posix_normalize_cwd(std::string cwd) {
    std::replace(cwd.begin(), cwd.end(), '\\', '/');
    return cwd;
}

/// pi `String.prototype.trim()`.
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

[[nodiscard]] bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

/// pi `renderProjectContext`: the fixed prose line plus one
/// `<project_instructions path="...">` block per file, joined with a blank
/// line.
[[nodiscard]] std::string render_project_context(const std::vector<ProjectContextFile>& context_files) {
    std::string text = "Project-specific instructions and guidelines:";
    for (const auto& file : context_files) {
        text += "\n\n<project_instructions path=\"";
        text += file.path;
        text += "\">\n";
        text += file.content;
        text += "\n</project_instructions>";
    }
    return text;
}

/// pi `buildRules`: the conditional bash/powershell exploration rule, the
/// caller's guideline bullets, then the always-lines; trimmed, deduped in
/// first-occurrence order, and rendered as `- ` bullets.
[[nodiscard]] std::string build_rules(
        const std::vector<std::string>& tools, const std::vector<std::string>& prompt_guidelines) {
    std::vector<std::string> rules;
    const auto add_rule = [&rules](std::string_view rule) {
        const std::string normalized = trim(rule);
        if (normalized.empty()) return;
        if (std::find(rules.begin(), rules.end(), normalized) != rules.end()) return;
        rules.push_back(normalized);
    };

    const bool has_bash = contains(tools, "bash");
    const bool has_powershell = contains(tools, "powershell");
    const bool has_grep = contains(tools, "grep");
    const bool has_find = contains(tools, "find");
    const bool has_ls = contains(tools, "ls");

    if ((has_bash || has_powershell) && !has_grep && !has_find && !has_ls) {
        if (has_bash && has_powershell) {
            add_rule("Use bash or PowerShell for file operations like listing, searching, and finding files");
        } else if (has_powershell) {
            add_rule("Use PowerShell for file operations like listing, searching, and finding files");
        } else {
            add_rule("Use bash for file operations like ls, rg, find");
        }
    }

    for (const auto& guideline : prompt_guidelines) {
        add_rule(guideline);
    }
    add_rule("Be concise in your responses");
    add_rule("Show file paths clearly when working with files");

    std::string text;
    text.reserve(rules.size() * 48);
    for (std::size_t index = 0; index < rules.size(); ++index) {
        if (index > 0) text += "\n";
        text += "- ";
        text += rules[index];
    }
    return text;
}

/// pi's documentation block (`system-prompt.ts` default branch), carrying the
/// C++ binary's own ("pike") identity and docs paths — the only delta from
/// pi's verbatim block (pinned by the differential golden; ADR 0036 G4).
[[nodiscard]] std::string build_docs(const BuildSystemPromptOptions& options) {
    std::string text = "pike documentation (read only when the user asks about pike itself, "
                       "its SDK, extensions, themes, skills, or TUI):\n"
                       "- Main documentation: ";
    text += options.readmePath;
    text += "\n- Additional docs: ";
    text += options.docsPath;
    text += "\n- Examples: ";
    text += options.examplesPath;
    text += " (extensions, custom tools, SDK)\n"
            "- When reading pike docs or examples, resolve docs/... under "
            "Additional docs and examples/... under Examples, not the current "
            "working directory\n"
            "- When asked about: extensions (docs/extensions.md, "
            "examples/extensions/), themes (docs/themes.md), skills "
            "(docs/skills.md), prompt templates (docs/prompt-templates.md), TUI "
            "components (docs/tui.md), keybindings (docs/keybindings.md), SDK "
            "integrations (docs/sdk.md), custom providers "
            "(docs/custom-provider.md), adding models (docs/models.md), pike "
            "packages (docs/packages.md), environment variables "
            "(docs/environment-variables.md)\n"
            "- When working on pike topics, read the docs and examples, and "
            "follow .md cross-references before implementing\n"
            "- Always read pike .md files completely and follow links to related "
            "docs (e.g., tui.md for TUI API details)";
    return text;
}

/// pi's tools section body: one `- name: snippet` line per visible tool,
/// `(none)` when none carry a snippet.
[[nodiscard]] std::string build_tools_list(
        const std::vector<std::string>& tools, const std::map<std::string, std::string>& tool_snippets) {
    std::string list;
    bool first = true;
    for (const auto& name : tools) {
        const auto snippet = tool_snippets.find(name);
        if (snippet == tool_snippets.end()) continue;
        if (!first) list += "\n";
        first = false;
        list += "- ";
        list += name;
        list += ": ";
        list += snippet->second;
    }
    return list.empty() ? "(none)" : list;
}

/// pi `formatSkillsForPrompt`'s read-capable tool choice: `read` first, then
/// `bash`.
[[nodiscard]] std::optional<std::string> skill_file_read_tool(const std::vector<std::string>& tools) {
    if (contains(tools, "read")) return std::string{"read"};
    if (contains(tools, "bash")) return std::string{"bash"};
    return std::nullopt;
}

/// Wrap a section body in its own tag (pi `buildSystemPromptSections`:
/// `<${name}>\n${content}\n</${name}>`).
[[nodiscard]] SystemPromptSection wrap_section(std::string name, std::string content) {
    std::string text;
    text.reserve(name.size() * 2 + content.size() + 5);
    text += "<";
    text += name;
    text += ">\n";
    text += content;
    text += "\n</";
    text += name;
    text += ">";
    return SystemPromptSection{.name = std::move(name), .text = std::move(text)};
}

} // namespace

std::vector<SystemPromptSection> buildSystemPromptSections(const BuildSystemPromptOptions& options) {
    std::vector<SystemPromptSection> sections;

    // pi `selectedTools ?? ["read", "bash", "edit", "write"]`; an explicitly
    // empty list keeps no tools.
    const std::vector<std::string>& tools = options.selectedTools ? *options.selectedTools : kDefaultToolNames;

    // pi `if (customPrompt)`: the JS-truthy check — an absent or empty custom
    // prompt takes the default branch.
    const bool has_custom_prompt = options.customPrompt && !options.customPrompt->empty();
    if (has_custom_prompt) {
        sections.push_back(SystemPromptSection{.name = "preamble", .text = *options.customPrompt});
    } else {
        sections.push_back(SystemPromptSection{.name = "preamble", .text = std::string{kDefaultPreamble}});
        sections.push_back(wrap_section("tools",
                build_tools_list(tools, options.toolSnippets) +
                        "\n\nIn addition to the tools above, you may have access to "
                        "other custom tools depending on the project."));
        sections.push_back(wrap_section("rules", build_rules(tools, options.promptGuidelines)));
        sections.push_back(wrap_section("docs", build_docs(options)));
    }

    // pi `appendSystemPrompt ? { addendum } : ...` — an absent or empty
    // append adds no section.
    if (options.appendSystemPrompt && !options.appendSystemPrompt->empty()) {
        sections.push_back(wrap_section("addendum", *options.appendSystemPrompt));
    }

    if (!options.contextFiles.empty()) {
        sections.push_back(wrap_section("project_context", render_project_context(options.contextFiles)));
    }

    // pi: the skills section appears only when a read-capable tool is active
    // and the formatted block is non-empty after trimming.
    if (const auto read_tool = skill_file_read_tool(tools); read_tool && !options.skills.empty()) {
        const std::string block = trim(formatSkillsForPrompt(options.skills, *read_tool));
        if (!block.empty()) {
            sections.push_back(wrap_section("skills", block));
        }
    }

    sections.push_back(wrap_section("cwd", posix_normalize_cwd(options.cwd)));
    return sections;
}

std::vector<SystemPromptSection> replaySystemPromptSections(const std::vector<ai::SystemMessage>& messages) {
    std::vector<SystemPromptSection> replayed;
    for (const auto& message : messages) {
        for (const auto& delta : message.sections) {
            const auto current = std::find_if(
                    replayed.begin(), replayed.end(), [&](const auto& section) { return section.name == delta.name; });
            if (!delta.text) {
                if (current != replayed.end()) {
                    replayed.erase(current);
                }
            } else if (current == replayed.end()) {
                replayed.push_back(SystemPromptSection{.name = delta.name, .text = *delta.text});
            } else {
                current->text = *delta.text;
            }
        }
    }
    return replayed;
}

std::string renderSystemPromptSections(const std::vector<SystemPromptSection>& sections) {
    std::string rendered;
    for (const auto& section : sections) {
        if (section.text.empty()) continue;
        if (!rendered.empty()) rendered += "\n\n";
        rendered += section.text;
    }
    return rendered;
}

std::string buildSystemPrompt(const BuildSystemPromptOptions& options) {
    return renderSystemPromptSections(buildSystemPromptSections(options));
}

} // namespace cch::coding_agent::prompt
