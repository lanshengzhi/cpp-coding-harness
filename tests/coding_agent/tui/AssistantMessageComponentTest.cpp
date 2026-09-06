#include "coding_agent/tui/AssistantMessageComponent.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/ai/Message.hpp>
#include <cch/tui/Component.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) {
                ++index;
            }
            if (index < text.size()) ++index;
            continue;
        }
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size() && text[index] != '\a') {
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

} // namespace

TEST_CASE("AssistantMessageComponent renders interleaved thinking and text in exact content order",
        "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent component(theme);

    ai::AssistantMessage message;
    message.content.push_back(ai::TextContent{.text = "alpha paragraph"});
    message.content.push_back(ai::ThinkingContent{.thinking = "first thought"});
    message.content.push_back(ai::TextContent{.text = "beta paragraph"});
    message.content.push_back(ai::ThinkingContent{.thinking = "second thought"});
    message.content.push_back(ai::TextContent{.text = "gamma paragraph"});
    message.stop_reason = ai::AssistantStopReason::Stop;

    component.update_content(message);
    const std::string screen = screen_of(component);

    const auto alpha = screen.find("alpha paragraph");
    const auto first_thought = screen.find("first thought");
    const auto beta = screen.find("beta paragraph");
    const auto second_thought = screen.find("second thought");
    const auto gamma = screen.find("gamma paragraph");
    REQUIRE(alpha != std::string::npos);
    REQUIRE(first_thought != std::string::npos);
    REQUIRE(beta != std::string::npos);
    REQUIRE(second_thought != std::string::npos);
    REQUIRE(gamma != std::string::npos);
    CHECK(alpha < first_thought);
    CHECK(first_thought < beta);
    CHECK(beta < second_thought);
    CHECK(second_thought < gamma);
}

TEST_CASE("AssistantMessageComponent preserves content order when interleaved blocks stream in deltas",
        "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent component(theme);

    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::Pending;

    message.content.push_back(ai::TextContent{.text = "streamed text"});
    component.update_content(message);

    std::get<ai::TextContent>(message.content[0]).text += " grown";
    component.update_content(message);

    // A thinking run that starts after text must render after that text.
    message.content.push_back(ai::ThinkingContent{.thinking = "later thought"});
    component.update_content(message);

    std::get<ai::ThinkingContent>(message.content[1]).thinking += " continued";
    component.update_content(message);

    message.stop_reason = ai::AssistantStopReason::Stop;
    component.update_content(message);

    const std::string screen = screen_of(component);
    const auto text = screen.find("streamed text grown");
    const auto thought = screen.find("later thought continued");
    REQUIRE(text != std::string::npos);
    REQUIRE(thought != std::string::npos);
    CHECK(text < thought);
}

TEST_CASE("AssistantMessageComponent freezes closed fences and preserves following text order",
        "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent component(theme);
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::Pending;
    message.content.push_back(ai::TextContent{.text = "before\n\n```cpp\nint value = 1;\n"});
    component.update_content(message);
    CHECK(component.frozen_block_count() == 1);
    CHECK(component.has_open_tail());

    std::get<ai::TextContent>(message.content[0]).text += "```\n\nafter";
    component.update_content(message);
    CHECK(component.frozen_block_count() == 2);
    CHECK(component.has_open_tail());

    message.stop_reason = ai::AssistantStopReason::Stop;
    component.update_content(message);
    CHECK(component.frozen_block_count() == 3);
    CHECK_FALSE(component.has_open_tail());
    const auto screen = screen_of(component);
    CHECK(screen.find("before") < screen.find("int value = 1;"));
    CHECK(screen.find("int value = 1;") < screen.find("after"));
}

TEST_CASE("AssistantMessageComponent freezes settled list items without reparsing prior items",
        "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent component(theme);
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::Pending;
    message.content.push_back(ai::TextContent{.text = "- first\n"});
    component.update_content(message);
    CHECK(component.frozen_block_count() == 0);

    std::get<ai::TextContent>(message.content[0]).text += "- second\n";
    component.update_content(message);
    CHECK(component.frozen_block_count() == 1);
    CHECK(component.open_tail_text() == "- second\n");

    std::get<ai::TextContent>(message.content[0]).text += "- third\n";
    component.update_content(message);
    CHECK(component.frozen_block_count() == 2);
    CHECK(component.open_tail_text() == "- third\n");
}

TEST_CASE("AssistantMessageComponent streamed rendering matches one-shot rendering", "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    const std::string full_text =
            "Opening paragraph\n\n- first item\n- second item\n\n```cpp\nint answer = 42;\n```\n\nClosing paragraph";

    ai::AssistantMessage one_shot_message;
    one_shot_message.content.push_back(ai::TextContent{.text = full_text});
    one_shot_message.stop_reason = ai::AssistantStopReason::Stop;
    coding_agent::tui::AssistantMessageComponent one_shot(theme);
    one_shot.update_content(one_shot_message);

    ai::AssistantMessage streamed_message;
    streamed_message.stop_reason = ai::AssistantStopReason::Pending;
    streamed_message.content.push_back(ai::TextContent{});
    coding_agent::tui::AssistantMessageComponent streamed(theme);
    for (std::size_t offset = 0; offset < full_text.size(); offset += 3) {
        std::get<ai::TextContent>(streamed_message.content[0]).text.append(full_text, offset, 3);
        streamed.update_content(streamed_message);
    }
    streamed_message.stop_reason = ai::AssistantStopReason::Stop;
    streamed.update_content(streamed_message);

    CHECK(screen_of(streamed) == screen_of(one_shot));
}

TEST_CASE("AssistantMessageComponent append processing stays bounded across a hundred chunks",
        "[coding_agent][tui][issue603][benchmark]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent component(theme);
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::Pending;
    message.content.push_back(ai::TextContent{});

    constexpr std::size_t kChunks = 100;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t chunk = 0; chunk < kChunks; ++chunk) {
        std::get<ai::TextContent>(message.content[0]).text += " token";
        component.update_content(message);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    CHECK(elapsed_us < 200'000);
    CHECK(component.has_open_tail());
}
