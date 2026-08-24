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

/// Options mirroring pi's `BuildSystemPromptOptions` (`core/system-prompt.ts`
/// at baseline 83114817), plus the identity delta: the C++ binary's own docs
/// paths, which pi resolves internally through `config.ts`
/// `getReadmePath()`/`getDocsPath()`/`getExamplesPath()`.
struct BuildSystemPromptOptions {
    /// Custom system prompt (replaces the default branch). pi checks the
    /// JS-truthy value: an absent **or empty** prompt takes the default
    /// branch.
    std::optional<std::string> customPrompt;
    /// Active tool names in pi's order. `std::nullopt` (absent) applies the
    /// pi default set (read, bash, edit, write); an explicitly provided
    /// vector — even an empty one — is used as-is (pi `selectedTools ||
    /// [...]` treats `[]` as truthy).
    std::optional<std::vector<std::string>> selectedTools;
    /// One-line tool snippets keyed by tool name (pi `toolSnippets`). A tool
    /// appears in `Available tools` only when a snippet is present.
    std::map<std::string, std::string> toolSnippets;
    /// Additional guideline bullets appended to the default guidelines
    /// (pi `promptGuidelines`).
    std::vector<std::string> promptGuidelines;
    /// Text appended to the System Prompt (pi `appendSystemPrompt`).
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

/// Build the System Prompt in pi's exact shape (`core/system-prompt.ts`
/// `buildSystemPrompt` at baseline 83114817): default/custom branches,
/// `Available tools` from tool snippets (`(none)` when empty), deduped
/// guidelines (tool guidelines + the auto bash-exploration rule + the
/// always-lines), the identity-adjusted documentation block (the C++ binary's
/// own "pike" identity and docs paths — the only delta from pi, pinned by the
/// differential golden), the append section, `<project_context>`, the skills
/// section (gated on the read tool), and the trailing
/// `Current working directory:` line.
[[nodiscard]] std::string buildSystemPrompt(const BuildSystemPromptOptions& options);

} // namespace cch::coding_agent::prompt
