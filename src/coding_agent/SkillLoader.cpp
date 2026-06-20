#include "../../include/cch/coding_agent/SkillLoader.hpp"

#include "SkillFrontmatterParser.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

namespace cch::coding_agent {

namespace {

constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxDescriptionLength = 1024;

/// Validate skill name against Agent Skills standard (with pi leniency).
/// Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateName(
    const std::string& name,
    const std::string& parentDirName) {
    std::vector<std::string> errors;

    if (name != parentDirName) {
        errors.push_back("name \"" + name + "\" does not match parent directory \"" +
                         parentDirName + "\"");
    }
    if (name.size() > kMaxNameLength) {
        errors.push_back("name exceeds " + std::to_string(kMaxNameLength) +
                         " characters (" + std::to_string(name.size()) + ")");
    }
    // Must be lowercase a-z, 0-9, hyphens only.
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
        errors.push_back(
            "name contains invalid characters "
            "(must be lowercase a-z, 0-9, hyphens only)");
    }
    if (!name.empty() && name.front() == '-') {
        errors.push_back("name must not start with a hyphen");
    }
    if (!name.empty() && name.back() == '-') {
        errors.push_back("name must not end with a hyphen");
    }
    if (name.find("--") != std::string::npos) {
        errors.push_back("name must not contain consecutive hyphens");
    }

    return errors;
}

/// Validate skill description. Empty/missing → reject; >1024 chars → warn.
/// Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateDescription(
    const std::string& description) {
    std::vector<std::string> errors;
    if (description.empty()) {
        errors.push_back("description is required");
    } else if (description.size() > kMaxDescriptionLength) {
        errors.push_back("description exceeds " +
                         std::to_string(kMaxDescriptionLength) +
                         " characters (" + std::to_string(description.size()) + ")");
    }
    return errors;
}

/// Extract parent directory basename from a file path.
[[nodiscard]] std::string parentDirName(const std::string& filePath) {
    // Find the last '/' to get the parent directory.
    std::size_t lastSlash = filePath.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        return "";
    }
    // The parent directory name is between the second-to-last '/' and last '/'.
    std::size_t secondLastSlash = filePath.rfind('/', lastSlash - 1);
    std::size_t start = (secondLastSlash == std::string::npos) ? 0 : secondLastSlash + 1;
    return filePath.substr(start, lastSlash - start);
}

} // namespace

SkillLoadResult loadSkillFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath) {
    SkillLoadResult result;

    auto readResult = fs.readTextFile(filePath);
    if (!readResult.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::read_failed,
            .message = readResult.error().message,
            .path = filePath,
        });
        return result;
    }

    // Resolve absolute path for storage.
    std::string absolutePath = (fs.root() / filePath).lexically_normal().string();

    auto parsed = parseFrontmatter(readResult.value());
    if (!parsed.has_value()) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::parse_failed,
            .message = parsed.error().message,
            .path = filePath,
        });
        return result;
    }

    const auto& fields = parsed->fields;

    // Resolve name: use frontmatter name, or derive from parent dir.
    std::string name;
    auto nameIt = fields.find("name");
    if (nameIt != fields.end()) {
        name = nameIt->second;
    } else {
        name = parentDirName(filePath);
    }

    // Validate name.
    std::string dirName = parentDirName(filePath);
    for (const auto& err : validateName(name, dirName)) {
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::invalid_metadata,
            .message = err,
            .path = filePath,
        });
    }

    // Resolve description and trim whitespace.
    std::string description;
    auto descIt = fields.find("description");
    if (descIt != fields.end()) {
        description = descIt->second;
        // Trim leading/trailing whitespace.
        while (!description.empty() && (description.front() == ' ' || description.front() == '\t')) {
            description.erase(0, 1);
        }
        while (!description.empty() && (description.back() == ' ' || description.back() == '\t')) {
            description.pop_back();
        }
    }

    // Validate description. Empty/missing → reject skill.
    auto descErrors = validateDescription(description);
    bool descriptionEmpty = false;
    for (const auto& err : descErrors) {
        if (err == "description is required") {
            descriptionEmpty = true;
        }
        result.diagnostics.push_back(SkillDiagnostic{
            .type = "warning",
            .code = SkillDiagnosticCode::invalid_metadata,
            .message = err,
            .path = filePath,
        });
    }

    if (descriptionEmpty) {
        // Skill rejected — return diagnostics only.
        return result;
    }

    // Resolve disableModelInvocation.
    bool disableModelInvocation = false;
    auto dmiIt = fields.find("disable-model-invocation");
    if (dmiIt != fields.end()) {
        disableModelInvocation = (dmiIt->second == "true");
    }

    result.skills.push_back(Skill{
        .name = std::move(name),
        .description = std::move(description),
        .content = std::move(parsed->body),
        .filePath = absolutePath,
        .disableModelInvocation = disableModelInvocation,
    });

    return result;
}

SkillLoadResult loadSkills(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<SkillDirSpec>& dirs) {
    // Deferred to U4.
    SkillLoadResult result;
    for (const auto& dir : dirs) {
        // Stub: for now just load each directory path as a single file.
        // The recursive walk will be implemented in U4.
        auto single = loadSkillFromFile(fs, dir.path);
        result.skills.insert(result.skills.end(),
                             std::make_move_iterator(single.skills.begin()),
                             std::make_move_iterator(single.skills.end()));
        result.diagnostics.insert(result.diagnostics.end(),
                                  std::make_move_iterator(single.diagnostics.begin()),
                                  std::make_move_iterator(single.diagnostics.end()));
    }
    return result;
}

} // namespace cch::coding_agent
