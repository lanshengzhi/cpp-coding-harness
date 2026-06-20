#include <cch/coding_agent/SkillFormatting.hpp>

#include <algorithm>
#include <filesystem>
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
    const std::string& content,
    std::string_view additional_instructions) {
    std::string result;
    result.reserve(256 + content.size()
        + (additional_instructions.empty() ? 0 : additional_instructions.size() + 2));

    std::filesystem::path fp(skill.filePath);
    std::string dirname = fp.parent_path().string();

    result += "<skill name=\"";
    result += escapeXml(skill.name);
    result += "\" location=\"";
    result += escapeXml(skill.filePath);
    result += "\">\n";
    result += "References are relative to ";
    result += dirname;
    result += ".\n\n";
    result += content;
    result += "\n</skill>";

    if (!additional_instructions.empty()) {
        result += "\n\n";
        result += additional_instructions;
    }

    return result;
}

} // namespace cch::coding_agent
