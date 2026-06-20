#pragma once

#include <cch/util/Error.hpp>

#include <map>
#include <string>
#include <string_view>

namespace cch::coding_agent {

/// Parsed YAML frontmatter from a SKILL.md file.
struct SkillFrontmatter {
    /// Flat key-value pairs from the YAML block between --- delimiters.
    /// Values are stored as strings; boolean fields ("true"/"false") are
    /// left as strings for the caller to interpret.
    std::map<std::string, std::string> fields;
    /// Body content after the closing --- delimiter, with leading/trailing
    /// whitespace trimmed.
    std::string body;
};

/// Parse a SKILL.md file's YAML frontmatter.
///
/// Normalizes \r\n → \n, splits on --- delimiters, and parses the YAML
/// block as flat key: value pairs. Returns empty frontmatter when no
/// frontmatter block is present.
[[nodiscard]] util::Expected<SkillFrontmatter> parseFrontmatter(std::string_view content);

} // namespace cch::coding_agent
