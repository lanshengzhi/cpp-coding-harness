#include "coding_agent/SkillFormatting.hpp"

#include "coding_agent/SkillFrontmatterParser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace cch::coding_agent {
namespace {

/// XML-escape special characters: & < > " '
[[nodiscard]] std::string escapeXml(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        switch (c) {
            case '&':  result += "&amp;";  break;
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '"':  result += "&quot;";  break;
            case '\'': result += "&apos;";  break;
            default:   result += c;        break;
        }
    }
    return result;
}

/// Read one skill file's content at invocation time (pi `readFileSync`). The
/// path was vetted by the loader; unreadable files pass the invocation
/// through unchanged, like pi's error path.
[[nodiscard]] std::optional<std::string> read_skill_file(
    const std::string& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string content;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size > 0) {
        content.resize(static_cast<std::size_t>(size));
    }
    input.seekg(0, std::ios::beg);
    if (size > 0 && !content.empty()) {
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
    }
    return content;
}

/// pi `_expandSkillCommand` name/args split: the skill name runs to the
/// first space; the remaining text is the invocation args with pi's
/// `.trim()` (whitespace stripped from both ends).
struct SkillInvocation {
    std::string_view name;
    std::string_view args;
};

[[nodiscard]] std::optional<SkillInvocation> parse_skill_invocation(
    std::string_view text) {
    if (!text.starts_with("/skill:")) {
        return std::nullopt;
    }
    const auto space_index = text.find(' ');
    const std::string_view name = space_index == std::string_view::npos
        ? text.substr(7)
        : text.substr(7, space_index - 7);
    if (name.empty()) {
        return std::nullopt;
    }
    if (space_index == std::string_view::npos) {
        return SkillInvocation{.name = name, .args = std::string_view{}};
    }
    auto args = text.substr(space_index + 1);
    while (!args.empty() && std::isspace(static_cast<unsigned char>(args.front()))) {
        args.remove_prefix(1);
    }
    while (!args.empty() && std::isspace(static_cast<unsigned char>(args.back()))) {
        args.remove_suffix(1);
    }
    return SkillInvocation{.name = name, .args = args};
}

} // namespace

std::string formatSkillsForPrompt(const std::vector<Skill>& skills) {
    // Filter out disabled skills
    std::vector<const Skill*> visible;
    for (const auto& s : skills) {
        if (!s.disableModelInvocation) {
            visible.push_back(&s);
        }
    }

    if (visible.empty()) {
        return {};
    }

    std::string result;
    result.reserve(512 + visible.size() * 256);

    // Prose intro — matches pi's formatSkillsForPrompt exactly
    result += "\n\nThe following skills provide specialized instructions for specific tasks.\n";
    result += "Use the read tool to load a skill's file when the task matches its description.\n";
    result += "When a skill file references a relative path, resolve it against the skill";
    result += " directory (parent of SKILL.md / dirname of the path) and use that absolute";
    result += " path in tool commands.\n";
    result += "\n";
    result += "<available_skills>\n";

    for (const auto* skill : visible) {
        result += "  <skill>\n";
        result += "    <name>";
        result += escapeXml(skill->name);
        result += "</name>\n";
        result += "    <description>";
        result += escapeXml(skill->description);
        result += "</description>\n";
        result += "    <location>";
        result += escapeXml(skill->filePath);
        result += "</location>\n";
        result += "  </skill>\n";
    }

    result += "</available_skills>";
    return result;
}

std::string formatSkillInvocation(
    const Skill& skill,
    std::string_view content,
    std::string_view additional_instructions) {
    std::string result;
    result.reserve(256 + content.size()
        + (additional_instructions.empty() ? 0 : additional_instructions.size() + 2));

    result += "<skill name=\"";
    result += escapeXml(skill.name);
    result += "\" location=\"";
    result += escapeXml(skill.filePath);
    result += "\">\n";
    result += "References are relative to ";
    result += skill.baseDir;
    result += ".\n\n";
    result += content;
    result += "\n</skill>";

    if (!additional_instructions.empty()) {
        result += "\n\n";
        result += additional_instructions;
    }

    return result;
}

std::string expandSkillCommand(
    std::string_view text,
    const std::vector<Skill>& skills) {
    const auto invocation = parse_skill_invocation(text);
    if (!invocation) {
        return std::string{text};
    }

    const auto found = std::find_if(
        skills.begin(),
        skills.end(),
        [name = invocation->name](const Skill& skill) { return skill.name == name; });
    if (found == skills.end()) {
        // Unknown skill, pass through.
        return std::string{text};
    }

    const auto content = read_skill_file(found->filePath);
    if (!content) {
        // Read failure: pass the original through (pi emits the extension
        // error and returns the original text).
        return std::string{text};
    }

    const auto parsed = parseFrontmatter(*content);
    if (!parsed) {
        return std::string{text};
    }
    // pi `stripFrontmatter(content).trim()` — the parser already trims.
    const auto& body = parsed->body;

    return formatSkillInvocation(*found, body, invocation->args);
}

} // namespace cch::coding_agent
