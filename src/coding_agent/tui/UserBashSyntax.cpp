#include "coding_agent/tui/UserBashSyntax.hpp"

#include "coding_agent/BoundedText.hpp"
#include "util/TerminalText.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace cch::coding_agent::tui {

std::string trim_editor_submission(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    if (first >= last) return {};
    return {first, last};
}

std::optional<UserBashInvocation> parse_user_bash_invocation(std::string text) {
    text = trim_editor_submission(std::move(text));
    if (!text.starts_with('!')) return std::nullopt;
    const bool excluded = text.starts_with("!!");
    auto command = trim_editor_submission(text.substr(excluded ? 2 : 1));
    if (command.empty()) return std::nullopt;
    return UserBashInvocation{
        .command = std::move(command),
        .exclude_from_context = excluded,
    };
}

std::string safe_user_bash_invocation(const UserBashInvocation& invocation) {
    auto command = util::strip_terminal_escape_sequences(invocation.command);
    std::erase_if(command, [](unsigned char value) {
        return value < 0x20 || value == 0x7f;
    });
    return bounded_redacted_presentation(std::format(
        "{} {}",
        invocation.exclude_from_context ? "!!" : "!",
        command));
}

bool user_bash_editor_mode(std::string text, bool user_bash_available) {
    return user_bash_available &&
        trim_editor_submission(std::move(text)).starts_with('!');
}

} // namespace cch::coding_agent::tui
