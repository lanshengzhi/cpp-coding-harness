#include "SlashCommandParser.hpp"

namespace cch::coding_agent::prompt {
namespace {

[[nodiscard]] bool is_ascii_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

} // namespace

std::optional<std::pair<std::string_view, std::string_view>> try_parse_slash_command(
    std::string_view input) {
    if (input.empty() || input.front() != '/') {
        return std::nullopt;
    }

    const auto body = input.substr(1);
    std::size_t delimiter = 0;
    while (delimiter < body.size() && !is_ascii_whitespace(body[delimiter])) {
        ++delimiter;
    }

    auto arguments = std::string_view{};
    if (delimiter < body.size()) {
        arguments = body.substr(delimiter + 1);
    }
    return std::pair<std::string_view, std::string_view>{body.substr(0, delimiter), arguments};
}

} // namespace cch::coding_agent::prompt
