#pragma once

#include <cch/coding_agent/Skill.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::prompt {

/// One project context file rendered into the System Prompt (pi
/// `BuildSystemPromptOptions.contextFiles` entry: `{ path, content }`).
struct ProjectContextFile {
    std::string path;
    std::string content;
};

/// One ordered System Prompt section (pi `SystemPromptSections`): `preamble`
/// carries untagged text, every other section's `text` is already the rendered
/// `<name>\n...\n</name>` block, so the transcript can replay and diff
/// sections verbatim.
struct SystemPromptSection {
    std::string name;
    std::string text;
};

/// Options mirroring pi's `BuildSystemPromptOptions` (`core/system-prompt.ts`
/// at baseline f07218c4, tag `v0.87.1`), plus the identity delta: the C++
/// binary's own docs paths, which pi resolves internally through `config.ts`
/// `getReadmePath()`/`getDocsPath()`/`getExamplesPath()`.
struct BuildSystemPromptOptions {
    /// Custom system prompt (replaces the default prefix). pi checks the
    /// JS-truthy value: an absent **or empty** prompt takes the default
    /// branch.
    std::optional<std::string> customPrompt;
    /// Active tool names in pi's order. `std::nullopt` (absent) applies the
    /// pi default set (read, bash, edit, write); an explicitly provided
    /// vector — even an empty one — is used as-is (pi `selectedTools ||
    /// [...]` treats `[]` as truthy).
    std::optional<std::vector<std::string>> selectedTools;
    /// One-line tool snippets keyed by tool name (pi `toolSnippets`). A tool
    /// appears in the `<tools>` section only when a snippet is present.
    std::map<std::string, std::string> toolSnippets;
    /// Additional guideline bullets appended after the tool guidelines
    /// (pi `promptGuidelines`).
    std::vector<std::string> promptGuidelines;
    /// Text appended from user configuration (pi `appendSystemPrompt`),
    /// rendered as the `<addendum>` section before project context, skills,
    /// and cwd.
    std::optional<std::string> appendSystemPrompt;
    /// Working directory (pi `cwd`); backslashes are posix-normalized.
    std::string cwd;
    /// Pre-loaded project context files (pi `contextFiles`).
    std::vector<ProjectContextFile> contextFiles;
    /// Pre-loaded skills (pi `skills`).
    std::vector<Skill> skills;
    /// Identity delta: the C++ binary's own documentation paths (pi
    /// `getReadmePath()`/`getDocsPath()`/`getExamplesPath()`).
    std::string readmePath;
    std::string docsPath;
    std::string examplesPath;
};

/// Build the ordered, independently replaceable sections of the structured
/// System Prompt (pi `buildSystemPromptSections` at baseline f07218c4, tag
/// `v0.87.1`): `preamble` (custom prompt or the default identity line),
/// `<tools>`/`<rules>`/`<docs>` on the default branch, `<addendum>`,
/// `<project_context>`, `<skills>` (gated on a read-capable tool), and
/// `<cwd>`. These are the sections a session's system message records.
[[nodiscard]] std::vector<SystemPromptSection> buildSystemPromptSections(
    const BuildSystemPromptOptions& options);

/// Render built sections exactly as the transcript's system message replays
/// them (pi `getSystemMessageText`): the non-empty section texts joined with a
/// blank line, `preamble` first.
[[nodiscard]] std::string renderSystemPromptSections(
    const std::vector<SystemPromptSection>& sections);

/// Build the System Prompt text (`buildSystemPrompt` at the same baseline):
/// the rendered sections, with the identity delta confined to the identity
/// line and the `<docs>` block (pinned by the differential golden).
[[nodiscard]] std::string buildSystemPrompt(
    const BuildSystemPromptOptions& options);

} // namespace cch::coding_agent::prompt
