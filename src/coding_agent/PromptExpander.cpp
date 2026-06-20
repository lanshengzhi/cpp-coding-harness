#include "../../include/cch/coding_agent/PromptProcessing.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace cch::coding_agent {
namespace {

// ── Argument parsing (bash-style quote-aware) ──

std::vector<std::string> parse_command_args(std::string_view input) {
    std::vector<std::string> args;
    if (input.empty()) return args;

    std::string current;
    enum { Normal, SingleQuote, DoubleQuote } state = Normal;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        switch (state) {
        case Normal:
            if (c == '\'') {
                state = SingleQuote;
            } else if (c == '"') {
                state = DoubleQuote;
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    args.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
            break;
        case SingleQuote:
            if (c == '\'') {
                state = Normal;
            } else {
                current += c;
            }
            break;
        case DoubleQuote:
            if (c == '"') {
                state = Normal;
            } else if (c == '\\' && i + 1 < input.size() && input[i + 1] == '"') {
                current += '"';
                ++i; // skip escaped quote
            } else {
                current += c;
            }
            break;
        }
    }
    if (!current.empty()) {
        args.push_back(std::move(current));
    }
    return args;
}

// ── Argument substitution ──

/// Parse a number from string view, return -1 on failure.
int parse_number(std::string_view sv) {
    int result = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return -1;
        result = result * 10 + (c - '0');
    }
    return result;
}

std::string substitute_args(std::string_view body, const std::vector<std::string>& args) {
    std::string result;
    result.reserve(body.size());

    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '$' && i + 1 < body.size()) {
            // $@ — all arguments
            if (body[i + 1] == '@') {
                for (size_t a = 0; a < args.size(); ++a) {
                    if (a > 0) result += ' ';
                    result += args[a];
                }
                ++i; // skip '@'
                continue;
            }

            // $ARGUMENTS — alias for $@
            if (i + 9 < body.size() && body.substr(i, 10) == "$ARGUMENTS") {
                for (size_t a = 0; a < args.size(); ++a) {
                    if (a > 0) result += ' ';
                    result += args[a];
                }
                i += 9; // skip 'ARGUMENTS'
                continue;
            }

            // $N — positional argument
            if (body[i + 1] >= '1' && body[i + 1] <= '9') {
                int pos = body[i + 1] - '1';
                ++i;
                if (static_cast<size_t>(pos) < args.size()) {
                    result += args[pos];
                }
                continue;
            }

            // ${...} — extended substitution
            if (body[i + 1] == '{') {
                auto close = body.find('}', i + 2);
                if (close == std::string_view::npos) {
                    result += "${";
                    ++i;
                    continue;
                }
                auto inner = body.substr(i + 2, close - (i + 2));

                // ${N:-default}
                auto colon = inner.find(":-");
                if (colon != std::string_view::npos && colon > 0) {
                    auto num_str = inner.substr(0, colon);
                    auto def = inner.substr(colon + 2);
                    int pos = parse_number(num_str);
                    if (pos >= 1 && static_cast<size_t>(pos - 1) < args.size()) {
                        result += args[pos - 1];
                    } else {
                        result += def;
                    }
                    i = close;
                    continue;
                }

                // ${@:N} or ${@:N:L}
                if (inner.size() > 2 && inner[0] == '@' && inner[1] == ':') {
                    auto rest = inner.substr(2);
                    auto colon2 = rest.find(':');
                    if (colon2 == std::string_view::npos) {
                        // ${@:N}
                        int start = parse_number(rest);
                        if (start >= 1) {
                            for (size_t a = static_cast<size_t>(start - 1); a < args.size(); ++a) {
                                if (a > static_cast<size_t>(start - 1)) result += ' ';
                                result += args[a];
                            }
                        }
                    } else {
                        // ${@:N:L}
                        int start = parse_number(rest.substr(0, colon2));
                        int len = parse_number(rest.substr(colon2 + 1));
                        if (start >= 1 && len > 0) {
                            size_t begin = static_cast<size_t>(start - 1);
                            size_t end = std::min(begin + static_cast<size_t>(len), args.size());
                            for (size_t a = begin; a < end; ++a) {
                                if (a > begin) result += ' ';
                                result += args[a];
                            }
                        }
                    }
                    i = close;
                    continue;
                }

                // Unrecognized ${...} — pass through
                result += body.substr(i, close - i + 1);
                i = close;
                continue;
            }
        }
        result += body[i];
    }
    return result;
}

} // namespace

// ── Public expand_prompt_template ──

std::string expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates) {

    // Only process inputs starting with '/'
    auto trimmed = trim_left(input);
    if (trimmed.empty() || trimmed.front() != '/') {
        return std::string{input};
    }

    // Extract template name (first token after '/')
    auto name = extract_command_name(trimmed);
    if (name.empty()) return std::string{input};

    // Find matching template
    const PromptTemplate* match = nullptr;
    for (const auto& tmpl : templates) {
        if (tmpl.name == name) {
            match = &tmpl;
            break;
        }
    }
    if (match == nullptr) return std::string{input};

    // Extract args and substitute
    auto args_str = extract_args(trimmed);
    auto args = parse_command_args(args_str);
    return substitute_args(match->content, args);
}

} // namespace cch::coding_agent
