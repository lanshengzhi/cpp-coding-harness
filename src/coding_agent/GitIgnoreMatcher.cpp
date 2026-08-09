#include "GitIgnoreMatcher.hpp"

#include <cctype>
#include <optional>
#include <sstream>

namespace cch::coding_agent {

namespace {

/// Convert one gitignore glob pattern to an ECMAScript regex body, supporting
/// the strict subset: `*` within a segment, `**` across segments, `?`, `[...]`
/// character classes, and `\` escapes. Every other regex metacharacter is
/// escaped literally. Infallible: unterminated character classes fall back to
/// a literal bracket.
[[nodiscard]] std::string glob_to_regex(std::string_view pattern) {
    std::string body;
    body.reserve(pattern.size() + 8);

    for (std::size_t index = 0; index < pattern.size();) {
        const char ch = pattern[index];
        if (ch == '*') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '*') {
                // `**/` matches zero or more leading directories (pi's ignore
                // package collapses it); a bare `**` spans any characters.
                if (index + 2 < pattern.size() && pattern[index + 2] == '/') {
                    body += "(?:.*/)?";
                    index += 3;
                } else {
                    body += ".*";
                    index += 2;
                }
            } else {
                body += "[^/]*";
                ++index;
            }
            continue;
        }
        if (ch == '?') {
            body += "[^/]";
            ++index;
            continue;
        }
        if (ch == '[') {
            // Character class: copy through the closing `]`. A `!` after the
            // opening bracket is gitignore negation (`[^...]` in regex).
            std::size_t close = index + 1;
            std::string klass;
            if (close < pattern.size() && (pattern[close] == '!' || pattern[close] == '^')) {
                klass = "^";
                ++close;
            }
            bool closed = false;
            for (; close < pattern.size(); ++close) {
                if (pattern[close] == ']' && close > index + 1) {
                    closed = true;
                    break;
                }
                klass.push_back(pattern[close]);
            }
            if (!closed) {
                // Unterminated class: treat the bracket literally.
                body += "\\[";
                ++index;
                continue;
            }
            body += '[';
            body += klass;
            body += ']';
            index = close + 1;
            continue;
        }
        if (ch == '\\' && index + 1 < pattern.size()) {
            // An escaped character matches itself (including `\#`/`\!` and
            // pattern metacharacters).
            const char escaped = pattern[index + 1];
            if (std::string_view{"^$\\.*+?()[]{}|"}.find(escaped) != std::string_view::npos) {
                body += '\\';
            }
            body += escaped;
            index += 2;
            continue;
        }
        if (std::string_view{"^$\\.*+?()[]{}|"}.find(ch) != std::string_view::npos) {
            body += '\\';
        }
        body += ch;
        ++index;
    }

    return body;
}

/// True when the rule's regex matches the candidate (full-string match).
[[nodiscard]] bool regex_matches(const std::regex& pattern, std::string_view candidate) {
    return std::regex_match(candidate.begin(), candidate.end(), pattern);
}

/// The basename of a posix path (everything after the last `/`).
[[nodiscard]] std::string_view basename_of(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

std::optional<std::string> prefix_ignore_pattern(
    std::string_view line,
    std::string_view prefix) {
    const auto trimmed = [](std::string_view text) {
        std::size_t begin = 0;
        while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
            ++begin;
        }
        std::size_t end = text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            --end;
        }
        return text.substr(begin, end - begin);
    };

    const auto trimmed_line = trimmed(line);
    if (trimmed_line.empty()) {
        return std::nullopt;
    }
    if (trimmed_line.starts_with("#") && !trimmed_line.starts_with("\\#")) {
        return std::nullopt;
    }

    // pi keeps the original (untrimmed) line as the pattern; only the leading
    // negation / anchor markers are transformed.
    std::string_view pattern = line;
    bool negated = false;
    if (pattern.starts_with("!")) {
        negated = true;
        pattern.remove_prefix(1);
    } else if (pattern.starts_with("\\!")) {
        pattern.remove_prefix(1);
    }
    if (pattern.starts_with("/")) {
        pattern.remove_prefix(1);
    }

    std::string prefixed;
    prefixed.reserve(prefix.size() + pattern.size() + 1);
    if (!prefix.empty()) {
        prefixed += prefix;
    }
    prefixed += pattern;
    return negated ? "!" + prefixed : prefixed;
}

void IgnoreMatcher::add_rules(std::string_view content, std::string_view prefix) {
    std::istringstream input{std::string{content}};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto transformed = prefix_ignore_pattern(line, prefix);
        if (!transformed) {
            continue;
        }

        std::string_view rule = *transformed;
        bool negated = false;
        if (rule.starts_with("!")) {
            negated = true;
            rule.remove_prefix(1);
        }

        bool dir_only = false;
        std::string_view body = rule;
        if (!body.empty() && body.back() == '/') {
            dir_only = true;
            body.remove_suffix(1);
        }
        if (body.empty()) {
            // A bare `!` or `/` line carries no pattern; the npm `ignore`
            // package treats it as matching nothing.
            continue;
        }

        // A pattern without a slash matches the basename at any depth
        // (gitignore semantics, mirrored by the `ignore` package); the
        // prefixed patterns from subdirectories always contain a slash and
        // stay anchored to the scan root.
        const bool basename_match = body.find('/') == std::string_view::npos;

        // A malformed pattern (e.g. an invalid character-class range) must
        // not abort the walk: the rule is skipped, like the npm `ignore`
        // package's sanitization.
        std::regex compiled;
        try {
            compiled = std::regex{"^" + glob_to_regex(body) + "$", std::regex::ECMAScript};
        } catch (const std::regex_error&) {
            continue;
        }
        rules_.push_back(Rule{
            .pattern = std::move(compiled),
            .negated = negated,
            .dir_only = dir_only,
            .basename_match = basename_match,
        });
    }
}

bool IgnoreMatcher::ignores(std::string_view rel_path, bool is_dir) const {
    if (rules_.empty()) {
        return false;
    }

    // The walk passes paths without a trailing slash; directories are
    // distinguished by `is_dir` (pi checks them as `relPath + "/"`). A
    // trailing slash, when present, is stripped before matching: dir-only
    // rules are compiled from the pattern without its slash.
    const std::string_view subject =
        !rel_path.empty() && rel_path.back() == '/'
        ? rel_path.substr(0, rel_path.size() - 1)
        : rel_path;

    bool ignored = false;
    for (const auto& rule : rules_) {
        if (rule.dir_only && !is_dir) {
            continue;
        }
        const std::string_view match_subject =
            rule.basename_match ? basename_of(subject) : subject;
        if (regex_matches(rule.pattern, match_subject)) {
            ignored = !rule.negated;
        }
    }
    return ignored;
}

} // namespace cch::coding_agent
