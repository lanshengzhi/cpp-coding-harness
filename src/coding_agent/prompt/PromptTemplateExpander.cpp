#include "coding_agent/prompt/PromptTemplateExpander.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::prompt {
namespace {

struct DecodedCodePoint {
    std::uint32_t value;
    std::size_t length;
};

[[nodiscard]] DecodedCodePoint decode_first(std::string_view text) {
    const auto first = static_cast<unsigned char>(text.front());
    if (first < 0x80) {
        return {first, 1};
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    } else {
        return {first, 1};
    }

    if (text.size() < length) {
        return {first, 1};
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xC0) != 0x80) {
            return {first, 1};
        }
        value = (value << 6) | (byte & 0x3F);
    }

    const bool overlong = (length == 2 && value < 0x80) ||
        (length == 3 && value < 0x800) ||
        (length == 4 && value < 0x10000);
    if (overlong || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return {first, 1};
    }
    return {value, length};
}

[[nodiscard]] bool is_unicode_whitespace(std::uint32_t code_point) {
    return (code_point >= 0x0009 && code_point <= 0x000D) ||
        code_point == 0x0020 || code_point == 0x00A0 || code_point == 0x1680 ||
        (code_point >= 0x2000 && code_point <= 0x200A) ||
        code_point == 0x2028 || code_point == 0x2029 || code_point == 0x202F ||
        code_point == 0x205F || code_point == 0x3000 || code_point == 0xFEFF;
}

[[nodiscard]] std::size_t whitespace_prefix_length(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    const auto decoded = decode_first(text);
    return is_unicode_whitespace(decoded.value) ? decoded.length : 0;
}

struct TemplateInvocation {
    std::string_view name;
    std::string_view arguments;
};

[[nodiscard]] std::optional<TemplateInvocation> parse_template_invocation(std::string_view input) {
    if (input.empty() || input.front() != '/') {
        return std::nullopt;
    }

    const auto body = input.substr(1);
    std::size_t delimiter = 0;
    while (delimiter < body.size()) {
        if (whitespace_prefix_length(body.substr(delimiter)) > 0) {
            break;
        }
        delimiter += decode_first(body.substr(delimiter)).length;
    }
    if (delimiter == 0) {
        return std::nullopt;
    }

    std::size_t arguments_start = delimiter;
    while (arguments_start < body.size()) {
        const auto whitespace_length = whitespace_prefix_length(body.substr(arguments_start));
        if (whitespace_length == 0) {
            break;
        }
        arguments_start += whitespace_length;
    }
    return TemplateInvocation{body.substr(0, delimiter), body.substr(arguments_start)};
}

[[nodiscard]] std::vector<std::string> parse_command_args(std::string_view input) {
    std::vector<std::string> args;
    std::string current;
    char quote = '\0';

    std::size_t index = 0;
    while (index < input.size()) {
        const char ch = input[index];
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
                ++index;
            } else {
                const auto decoded = decode_first(input.substr(index));
                current.append(input.substr(index, decoded.length));
                index += decoded.length;
            }
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            ++index;
        } else if (const auto whitespace_length = whitespace_prefix_length(input.substr(index));
                   whitespace_length > 0) {
            if (!current.empty()) {
                args.push_back(std::move(current));
                current.clear();
            }
            index += whitespace_length;
        } else {
            const auto decoded = decode_first(input.substr(index));
            current.append(input.substr(index, decoded.length));
            index += decoded.length;
        }
    }

    if (!current.empty()) {
        args.push_back(std::move(current));
    }
    return args;
}

[[nodiscard]] std::optional<std::size_t> parse_unsigned(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return std::numeric_limits<std::size_t>::max();
        }
        value = value * 10 + digit;
    }
    return value;
}

void append_joined(
    std::string& output,
    const std::vector<std::string>& args,
    std::size_t begin,
    std::size_t end) {
    begin = std::min(begin, args.size());
    end = std::min(end, args.size());
    for (std::size_t index = begin; index < end; ++index) {
        if (index > begin) {
            output += ' ';
        }
        output += args[index];
    }
}

[[nodiscard]] std::string substitute_args(
    std::string_view content,
    const std::vector<std::string>& args) {
    std::string output;
    output.reserve(content.size());

    std::size_t index = 0;
    while (index < content.size()) {
        if (content[index] != '$' || index + 1 >= content.size()) {
            output += content[index++];
            continue;
        }

        if (content.substr(index).starts_with("$ARGUMENTS")) {
            append_joined(output, args, 0, args.size());
            index += 10;
            continue;
        }
        if (content[index + 1] == '@') {
            append_joined(output, args, 0, args.size());
            index += 2;
            continue;
        }

        if (content[index + 1] >= '0' && content[index + 1] <= '9') {
            std::size_t end = index + 1;
            while (end < content.size() && content[end] >= '0' && content[end] <= '9') {
                ++end;
            }
            const auto position = parse_unsigned(content.substr(index + 1, end - index - 1));
            if (position && *position >= 1 && *position <= args.size()) {
                output += args[*position - 1];
            }
            index = end;
            continue;
        }

        if (content[index + 1] == '{') {
            const auto close = content.find('}', index + 2);
            if (close != std::string_view::npos) {
                const auto inner = content.substr(index + 2, close - index - 2);
                const auto default_separator = inner.find(":-");
                if (default_separator != std::string_view::npos) {
                    const auto position = parse_unsigned(inner.substr(0, default_separator));
                    if (position) {
                        const auto argument_index = *position >= 1 ? *position - 1 : args.size();
                        if (argument_index < args.size() && !args[argument_index].empty()) {
                            output += args[argument_index];
                        } else {
                            output += inner.substr(default_separator + 2);
                        }
                        index = close + 1;
                        continue;
                    }
                }

                if (inner.starts_with("@:")) {
                    const auto slice = inner.substr(2);
                    const auto length_separator = slice.find(':');
                    const auto start_text = length_separator == std::string_view::npos
                        ? slice
                        : slice.substr(0, length_separator);
                    const auto start_value = parse_unsigned(start_text);
                    const auto length_value = length_separator == std::string_view::npos
                        ? std::optional<std::size_t>{}
                        : parse_unsigned(slice.substr(length_separator + 1));
                    if (start_value && (length_separator == std::string_view::npos || length_value)) {
                        const auto begin = *start_value >= 1 ? *start_value - 1 : 0;
                        const auto end = length_value && *length_value <= std::numeric_limits<std::size_t>::max() - begin
                            ? begin + *length_value
                            : args.size();
                        append_joined(output, args, begin, end);
                        index = close + 1;
                        continue;
                    }
                }
            }
        }

        output += '$';
        ++index;
    }

    return output;
}

} // namespace

std::optional<std::string> try_expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates) {
    const auto invocation = parse_template_invocation(input);
    if (!invocation) {
        return std::nullopt;
    }

    const auto match = std::find_if(
        templates.begin(),
        templates.end(),
        [name = invocation->name](const PromptTemplate& candidate) {
            return candidate.name == name;
        });
    if (match == templates.end()) {
        return std::nullopt;
    }

    return substitute_args(match->content, parse_command_args(invocation->arguments));
}

} // namespace cch::coding_agent::prompt
