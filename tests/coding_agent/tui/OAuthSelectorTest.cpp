// pi `oauth-selector.ts`: the login/logout provider selector with auth-status
// labels and fuzzy search (G2 decision 4). Component-level coverage of the
// provider rows, status indicators, auth-type labels, fuzzy filtering, empty
// states, and navigation through the public component surface.

#include "coding_agent/tui/OAuthSelector.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
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

/// Remove SGR (`ESC [ … final`) and OSC hyperlink (`ESC ] … BEL`) sequences
/// so assertions target the visible text.
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
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size() && text[index] != '\a') ++index;
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

[[nodiscard]] coding_agent::tui::AuthSelectorProvider provider(
    std::string id,
    std::string name,
    coding_agent::tui::AuthSelectorType type,
    std::optional<coding_agent::tui::AuthSelectorStatus> status = std::nullopt) {
    return coding_agent::tui::AuthSelectorProvider{
        .id = std::move(id),
        .name = std::move(name),
        .auth_type = type,
        .method_name = std::nullopt,
        .status = std::move(status),
    };
}

void type(cch::tui::InputHandler& handler, std::string text) {
    for (const char ch : text) {
        handler.handle_input(tui::KeyEvent{.key = std::string(1, ch)});
    }
}

} // namespace

TEST_CASE(
    "OAuthSelector renders the login title and provider rows with auth-status indicators",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorProvider;
    using coding_agent::tui::AuthSelectorStatus;
    using coding_agent::tui::AuthSelectorType;
    std::vector<AuthSelectorProvider> providers;
    providers.push_back(provider("openai-codex", "OpenAI Codex", AuthSelectorType::OAuth));
    providers.push_back(provider(
        "kimi-coding",
        "Kimi For Coding",
        AuthSelectorType::OAuth,
        AuthSelectorStatus{.type = AuthSelectorType::OAuth, .source = "OAuth"}));
    providers.push_back(provider(
        "kimi-coding",
        "Kimi For Coding",
        AuthSelectorType::ApiKey,
        AuthSelectorStatus{.type = AuthSelectorType::ApiKey, .source = "KIMI_API_KEY"}));

    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Login,
        std::move(providers),
        [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
        [] {});

    const auto screen = screen_of(selector);
    CHECK(screen.find("Select provider to configure:") != std::string::npos);
    // Unconfigured provider: muted "• unconfigured".
    CHECK(screen.find("→ OpenAI Codex [subscription] • unconfigured") != std::string::npos);
    // Stored OAuth credential: success "✓ configured".
    CHECK(screen.find("Kimi For Coding [subscription] ✓ configured") != std::string::npos);
    // Environment source: success "✓ env: <VAR>".
    CHECK(screen.find("Kimi For Coding [API key] ✓ env: KIMI_API_KEY") != std::string::npos);
}

TEST_CASE(
    "OAuthSelector warns when the stored credential type differs from the row's auth type",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorStatus;
    using coding_agent::tui::AuthSelectorType;
    std::vector<coding_agent::tui::AuthSelectorProvider> providers;
    // An API-key row while the provider currently authenticates via OAuth.
    providers.push_back(provider(
        "kimi-coding",
        "Kimi For Coding",
        AuthSelectorType::ApiKey,
        AuthSelectorStatus{.type = AuthSelectorType::OAuth, .source = "OAuth"}));

    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Login,
        std::move(providers),
        [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
        [] {});

    const auto screen = screen_of(selector);
    CHECK(screen.find("• subscription configured") != std::string::npos);
    // A single auth type across the list: no [subscription]/[API key] labels.
    CHECK(screen.find("[API key]") == std::string::npos);
}

TEST_CASE(
    "OAuthSelector fuzzy-filters providers through the search input",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorType;
    std::vector<coding_agent::tui::AuthSelectorProvider> providers;
    providers.push_back(provider("openai-codex", "OpenAI Codex", AuthSelectorType::OAuth));
    providers.push_back(provider("kimi-coding", "Kimi For Coding", AuthSelectorType::OAuth));
    providers.push_back(provider("deepseek", "DeepSeek", AuthSelectorType::ApiKey));

    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Login,
        std::move(providers),
        [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
        [] {});

    type(selector, "kimi");
    {
        const auto screen = screen_of(selector);
        CHECK(screen.find("Kimi For Coding") != std::string::npos);
        CHECK(screen.find("OpenAI Codex") == std::string::npos);
        CHECK(screen.find("DeepSeek") == std::string::npos);
    }

    type(selector, "zzz");
    {
        const auto screen = screen_of(selector);
        CHECK(screen.find("No matching providers") != std::string::npos);
    }
}

TEST_CASE(
    "OAuthSelector submits the highlighted provider with id and auth type",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorType;
    std::optional<std::pair<std::string, AuthSelectorType>> selected;
    std::vector<coding_agent::tui::AuthSelectorProvider> providers;
    providers.push_back(provider("openai-codex", "OpenAI Codex", AuthSelectorType::OAuth));
    providers.push_back(provider("deepseek", "DeepSeek", AuthSelectorType::ApiKey));

    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Login,
        std::move(providers),
        [&selected](std::string id, AuthSelectorType type) {
            selected = std::make_pair(std::move(id), type);
        },
        [] {});

    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(selected.has_value());
    CHECK(selected->first == "deepseek");
    CHECK(selected->second == AuthSelectorType::ApiKey);

    // Navigation now wraps around like the delegated SelectList and the
    // sibling model selectors: one "up" from the bottom row reaches the top.
    selector.handle_input(tui::KeyEvent{.key = "up"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(selected.has_value());
    CHECK(selected->first == "openai-codex");
    CHECK(selected->second == AuthSelectorType::OAuth);

    // And one more "up" from the top row wraps back to the bottom row.
    selector.handle_input(tui::KeyEvent{.key = "up"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(selected.has_value());
    CHECK(selected->first == "deepseek");
    CHECK(selected->second == AuthSelectorType::ApiKey);
}

TEST_CASE(
    "OAuthSelector renders logout empty states and cancels",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    std::size_t cancellations = 0;
    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Logout,
        {},
        [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
        [&cancellations] { ++cancellations; });

    const auto screen = screen_of(selector);
    CHECK(screen.find("Select provider to logout:") != std::string::npos);
    CHECK(screen.find("No providers logged in. Use /login first.") != std::string::npos);

    selector.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

TEST_CASE(
    "OAuthSelector renders the login empty state and windows long lists with scroll info",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorType;
    {
        coding_agent::tui::OAuthSelectorComponent empty(
            theme,
            test_keybindings(),
            coding_agent::tui::AuthSelectorMode::Login,
            {},
            [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
            [] {});
        CHECK(screen_of(empty).find("No providers available") != std::string::npos);
    }

    std::vector<coding_agent::tui::AuthSelectorProvider> providers;
    for (std::size_t index = 0; index < 10; ++index) {
        providers.push_back(provider(
            "provider-" + std::to_string(index),
            "Provider " + std::to_string(index),
            AuthSelectorType::OAuth));
    }
    coding_agent::tui::OAuthSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::AuthSelectorMode::Login,
        std::move(providers),
        [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
        [] {});

    // pi's maxVisible is 8: navigating to the last row windows the list and
    // appends the muted scroll info.
    for (std::size_t index = 0; index < 9; ++index) {
        selector.handle_input(tui::KeyEvent{.key = "down"});
    }
    const auto screen = screen_of(selector);
    CHECK(screen.find("→ Provider 9") != std::string::npos);
    CHECK(screen.find("Provider 0\n") == std::string::npos);
    CHECK(screen.find("(10/10)") != std::string::npos);
}

TEST_CASE("OAuthSelector reports the search cursor on the real search row of its chrome",
        "[coding_agent][tui][login][issue588]") {
    auto theme = test_theme();
    using coding_agent::tui::AuthSelectorType;
    coding_agent::tui::OAuthSelectorComponent selector(
            theme,
            test_keybindings(),
            coding_agent::tui::AuthSelectorMode::Login,
            std::vector<coding_agent::tui::AuthSelectorProvider>{
                    provider("openai-codex", "OpenAI Codex", AuthSelectorType::OAuth)},
            [](std::string, coding_agent::tui::AuthSelectorType) -> support::ExpectedVoid { return {}; },
            [] {});

    selector.set_focused(true);
    REQUIRE(selector.render(80));
    const auto cursor = selector.cursor_location();
    REQUIRE(cursor.has_value());
    // The SelectList's chrome places the search input at row 4 (border,
    // spacer, title, spacer); the old hard-coded `cursor->row += 4` is gone
    // but the reported row must match.
    CHECK(cursor->row == 4);
    CHECK(cursor->column == 2);
}
