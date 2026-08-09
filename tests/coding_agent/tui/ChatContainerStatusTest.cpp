// pi `interactive-mode.ts` `showStatus`: a dim status line in the chat that
// replaces the previous status line when it is still the newest chat item
// (the login/logout flows report through it).

#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() &&
                   !(text[index] >= '@' && text[index] <= '~')) {
                ++index;
            }
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string screen_of(cch::tui::Component& component, std::size_t width = 80) {
    const auto rendered = component.render(width);
    REQUIRE(rendered);
    std::string text;
    for (const auto& line : rendered->lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::size_t count_text(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace

TEST_CASE(
    "ChatContainer status lines replace the newest status like pi showStatus",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, *test_keybindings());

    chat.append_status_message("Logged in to OpenAI Codex. Credentials saved to /tmp/auth.json");
    {
        const auto screen = screen_of(chat);
        CHECK(screen.find("Logged in to OpenAI Codex. Credentials saved to /tmp/auth.json") !=
            std::string::npos);
    }

    // The newest chat item is the status: the next status replaces it (pi
    // keeps one live status line at the tail).
    chat.append_status_message("Logged out of OpenAI Codex");
    {
        const auto screen = screen_of(chat);
        CHECK(screen.find("Logged out of OpenAI Codex") != std::string::npos);
        CHECK(screen.find("Logged in to OpenAI Codex") == std::string::npos);
    }

    // An interleaved item pins the previous status: the next status appends.
    chat.append_diagnostic("something failed");
    chat.append_status_message("Saved API key for DeepSeek. Credentials saved to /tmp/auth.json");
    {
        const auto screen = screen_of(chat);
        CHECK(screen.find("Logged out of OpenAI Codex") != std::string::npos);
        CHECK(screen.find("Saved API key for DeepSeek. Credentials saved to /tmp/auth.json") !=
            std::string::npos);
        CHECK(count_text(screen, "Logged out of OpenAI Codex") == 1);
        // The diagnostic keeps pi's `Error:` presentation.
        CHECK(screen.find("Error: something failed") != std::string::npos);
    }
}

TEST_CASE(
    "ChatContainer trust warning renders pi's untrusted-project line",
    "[coding_agent][tui][issue413]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, *test_keybindings());

    // The warning alone: no leading spacer (pi adds Spacer only when the
    // chat already has children).
    chat.append_trust_warning(
        "This project is not trusted. Project .pi resources are ignored.");
    {
        const auto screen = screen_of(chat);
        CHECK(screen.find(
                  "This project is not trusted. Project .pi resources are ignored.") !=
            std::string::npos);
        // Plain warning text, no `Warning:` prefix.
        CHECK(screen.find("Warning: This project") == std::string::npos);
        CHECK(screen.find("Error:") == std::string::npos);
    }

    // After a message: a spacer row separates it from the warning.
    chat.clear();
    chat.append_frontend_message("hello");
    chat.append_trust_warning("This project is not trusted.");
    {
        const auto screen = screen_of(chat);
        CHECK(screen.find("hello") != std::string::npos);
        CHECK(screen.find("This project is not trusted.") != std::string::npos);
        // The blank spacer row renders between the two lines.
        const auto first = screen.find("hello");
        const auto second = screen.find("This project is not trusted.");
        CHECK(first != std::string::npos);
        CHECK(second > first);
    }
}
