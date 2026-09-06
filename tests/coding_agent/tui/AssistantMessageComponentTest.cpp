#include "coding_agent/tui/AssistantMessageComponent.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/RenderedScreen.hpp"

#include <cch/ai/Message.hpp>

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
    const std::string screen = tests::rendered_screen(component);

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

    const std::string screen = tests::rendered_screen(component);
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
    const auto screen = tests::rendered_screen(component);
    CHECK(screen.find("before") < screen.find("int value = 1;"));
    CHECK(screen.find("int value = 1;") < screen.find("after"));
}

TEST_CASE("AssistantMessageComponent renders consecutive thinking blocks as one run", "[coding_agent][tui][issue603]") {
    auto theme = test_theme();
    coding_agent::tui::AssistantMessageComponent split(theme);
    ai::AssistantMessage split_message;
    split_message.content.push_back(ai::ThinkingContent{.thinking = "thought one"});
    split_message.content.push_back(ai::ThinkingContent{.thinking = "thought two"});
    split_message.stop_reason = ai::AssistantStopReason::Stop;
    split.update_content(split_message);
    CHECK(split.frozen_block_count() == 1);

    coding_agent::tui::AssistantMessageComponent joined(theme);
    ai::AssistantMessage joined_message;
    joined_message.content.push_back(ai::ThinkingContent{.thinking = "thought one\n\nthought two"});
    joined_message.stop_reason = ai::AssistantStopReason::Stop;
    joined.update_content(joined_message);

    CHECK(tests::rendered_screen(split) == tests::rendered_screen(joined));
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

    const auto streamed_screen = tests::rendered_screen(streamed);
    const auto one_shot_screen = tests::rendered_screen(one_shot);
    CHECK(streamed_screen == one_shot_screen);
}

TEST_CASE("AssistantMessageComponent append processing stays bounded across a hundred chunks",
        "[coding_agent][tui][issue603][benchmark]") {
    auto theme = test_theme();
    // Structured stream: paragraphs, a settled list run, and a closed fence
    // recur so freeze boundaries fire throughout the chunk sequence.
    const std::string unit =
            "Paragraph text about streaming.\n\n- item one\n- item two\n\n```cpp\nint value = 1;\n```\n\n";
    std::string full;
    for (std::size_t repeat = 0; repeat < 10; ++repeat)
        full += unit;

    coding_agent::tui::AssistantMessageComponent component(theme);
    ai::AssistantMessage message;
    message.stop_reason = ai::AssistantStopReason::Pending;
    message.content.push_back(ai::TextContent{});

    constexpr std::size_t kChunks = 100;
    constexpr auto kMaxChunkTime = std::chrono::milliseconds(2);
    const std::size_t slice = (full.size() + kChunks - 1) / kChunks;
    for (std::size_t chunk = 0; chunk < kChunks; ++chunk) {
        if (const auto pos = chunk * slice; pos < full.size()) {
            std::get<ai::TextContent>(message.content[0]).text.append(full, pos, slice);
        }
        const auto started = std::chrono::steady_clock::now();
        component.update_content(message);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed < kMaxChunkTime);
    }
    // Closed blocks froze mid-stream; only the trailing segment stays dynamic.
    CHECK(component.frozen_block_count() > 0);

    message.stop_reason = ai::AssistantStopReason::Stop;
    component.update_content(message);
    CHECK_FALSE(component.has_open_tail());

    coding_agent::tui::AssistantMessageComponent one_shot(theme);
    ai::AssistantMessage one_shot_message;
    one_shot_message.content.push_back(ai::TextContent{.text = full});
    one_shot_message.stop_reason = ai::AssistantStopReason::Stop;
    one_shot.update_content(one_shot_message);
    CHECK(tests::rendered_screen(component) == tests::rendered_screen(one_shot));
}
