#include "coding_agent/tui/ChatContainer.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

[[nodiscard]] std::shared_ptr<coding_agent::tui::SharedKeybindings> test_keybinding_slot() {
    return std::make_shared<coding_agent::tui::SharedKeybindings>(test_keybindings());
}

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

TEST_CASE("ChatContainer completed messages transition to Committed state with cached lines",
        "[coding_agent][tui][issue602]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());

    // 1. User message is committed immediately upon completion.
    chat.append_committed_message(ai::user_text_message("Explain quantum computing"));
    CHECK(chat.item_count() == 1);
    CHECK(chat.is_item_committed(0));
    CHECK_FALSE(chat.is_item_cache_valid(0)); // Unrendered so far

    // First render pass: cold render populates the cache.
    auto render1 = chat.render(80);
    REQUIRE(render1);
    CHECK(chat.is_item_cache_valid(0));
    CHECK(chat.cached_line_count(0) > 0);
    CHECK(chat.cold_render_count() == 1);
    CHECK(chat.cache_hit_count() == 0);

    // 2. Assistant message streaming lifecycle: Active -> Finalized -> Committed.
    ai::AssistantMessage streaming_msg;
    streaming_msg.content.push_back(ai::TextContent{.text = "Quantum computing "});

    chat.apply_event(agent::MessageStartEvent{
            .message = streaming_msg,
    });
    CHECK(chat.item_count() == 2);
    // Active streaming assistant item is NOT committed!
    CHECK_FALSE(chat.is_item_committed(1));
    CHECK_FALSE(chat.is_item_cache_valid(1));

    // Chunk arrives during streaming
    streaming_msg.content.push_back(ai::TextContent{.text = "uses qubits."});
    chat.apply_event(agent::MessageUpdateEvent{
            .message = streaming_msg,
            .assistant_event = ai::TextDeltaEvent{.delta = "uses qubits."},
    });
    // Assistant finishes turn (MessageEndEvent without tool calls) -> Committed!
    streaming_msg.stop_reason = ai::AssistantStopReason::Stop;
    chat.apply_event(agent::MessageEndEvent{
            .message = streaming_msg,
    });
    CHECK(chat.is_item_committed(1));
    CHECK_FALSE(chat.is_item_cache_valid(1));

    // Render pass: item 0 reuses cache (hit), item 1 renders cold and freezes lines into cache.
    auto render2 = chat.render(80);
    REQUIRE(render2);
    CHECK(chat.cold_render_count() == 2); // 1 cold for item 0, 1 cold for item 1
    CHECK(chat.cache_hit_count() == 1);   // item 0 hit cache
    CHECK(chat.is_item_cache_valid(0));
    CHECK(chat.is_item_cache_valid(1));
    CHECK(chat.cached_line_count(1) > 0);

    // 3. Assistant message with tool execution lifecycle.
    ai::AssistantMessage tool_assistant;
    tool_assistant.content.push_back(ai::TextContent{.text = "Running bash tool..."});
    tool_assistant.content.push_back(ai::ToolCallContent{
            .id = "call_bash_001",
            .name = "bash",
            .raw_arguments = "{\"command\":\"ls -la\"}",
    });

    chat.apply_event(agent::MessageStartEvent{
            .message = tool_assistant,
    });
    CHECK(chat.item_count() == 3);
    CHECK_FALSE(chat.is_item_committed(2));

    // Message stream ends, but tool execution is still pending!
    tool_assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    chat.apply_event(agent::MessageEndEvent{
            .message = tool_assistant,
    });
    // Tool is still pending, so message item is not yet committed.
    CHECK_FALSE(chat.is_item_committed(2));

    // Tool execution starts
    chat.apply_event(agent::ToolExecutionStartEvent{
            .tool_call_id = "call_bash_001",
            .tool_name = "bash",
            .args = support::JsonValue{},
    });
    CHECK_FALSE(chat.is_item_committed(2));

    // Tool execution ends -> tool settles -> item transitions to Committed!
    chat.apply_event(agent::ToolExecutionEndEvent{
            .tool_call_id = "call_bash_001",
            .tool_name = "bash",
            .result =
                    agent::AsyncToolExecutionResult{
                            .content = {ai::TextContent{.text = "total 0\n-rw-r--r-- file.txt"}},
                            .details = std::nullopt,
                            .is_error = false,
                    },
            .is_error = false,
    });
    CHECK(chat.is_item_committed(2));
    CHECK_FALSE(chat.is_item_cache_valid(2)); // not rendered at width 80 yet

    // Render pass: items 0 and 1 hit cache, item 2 renders cold and commits cache.
    auto render3 = chat.render(80);
    REQUIRE(render3);
    CHECK(chat.cold_render_count() == 3);
    CHECK(chat.cache_hit_count() == 3); // item 0 hit (2nd time), item 1 hit (1st time)
    CHECK(chat.is_item_cache_valid(2));
    CHECK(chat.committed_item_count() == 3);
}

TEST_CASE("ChatContainer subsequent render passes reuse cached lines without re-parsing",
        "[coding_agent][tui][issue602]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());

    chat.append_committed_message(ai::user_text_message("Hello from user"));
    ai::AssistantMessage assistant;
    assistant.content.push_back(ai::TextContent{.text = "Hello! I am Pike, your coding assistant."});
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    chat.append_committed_message(assistant);

    REQUIRE(chat.item_count() == 2);
    REQUIRE(chat.is_item_committed(0));
    REQUIRE(chat.is_item_committed(1));

    // Pass 1: Cold render. Both items are rendered and cached.
    const auto screen1 = screen_of(chat, 80);
    CHECK(screen1.find("Hello from user") != std::string::npos);
    CHECK(screen1.find("Hello! I am Pike") != std::string::npos);
    CHECK(chat.cold_render_count() == 2);
    CHECK(chat.cache_hit_count() == 0);

    // Pass 2: Warm render. Both items must be reused directly from the line cache.
    const auto screen2 = screen_of(chat, 80);
    CHECK(screen2 == screen1);
    CHECK(chat.cold_render_count() == 2); // Unchanged!
    CHECK(chat.cache_hit_count() == 2);   // 2 hits!

    // Pass 3: Another warm render.
    const auto screen3 = screen_of(chat, 80);
    CHECK(screen3 == screen1);
    CHECK(chat.cold_render_count() == 2); // Still unchanged!
    CHECK(chat.cache_hit_count() == 4);   // 4 cumulative hits!
}

TEST_CASE("ChatContainer width resize invalidates cache and cleanly reflows", "[coding_agent][tui][issue602]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());

    // Long single line of text that wraps differently at width 80 vs width 40.
    const std::string long_text = "The Pike runtime decouples the Authoritative Core and all presentation surfaces "
                                  "into an asynchronous read-only projection architecture with zero-mutex sampling.";
    chat.append_committed_message(ai::user_text_message(long_text));
    REQUIRE(chat.is_item_committed(0));

    // Render at width 80
    const auto render80_1 = chat.render(80);
    REQUIRE(render80_1);
    const auto lines_at_80 = chat.cached_line_count(0);
    CHECK(chat.cold_render_count() == 1);
    CHECK(chat.cache_hit_count() == 0);

    // Render again at width 80: Cache hit
    const auto render80_2 = chat.render(80);
    REQUIRE(render80_2);
    CHECK(chat.cold_render_count() == 1);
    CHECK(chat.cache_hit_count() == 1);
    CHECK(chat.cached_line_count(0) == lines_at_80);

    // Resize terminal to width 40: Cache must invalidate and reflow cleanly!
    const auto render40_1 = chat.render(40);
    REQUIRE(render40_1);
    const auto lines_at_40 = chat.cached_line_count(0);
    CHECK(lines_at_40 > lines_at_80);     // Text wrapped to more lines
    CHECK(chat.cold_render_count() == 2); // Cold render at width 40
    CHECK(chat.cache_hit_count() == 1);   // No new cache hit

    // Render again at width 40: Cache hit for width 40
    const auto render40_2 = chat.render(40);
    REQUIRE(render40_2);
    CHECK(chat.cold_render_count() == 2); // Unchanged!
    CHECK(chat.cache_hit_count() == 2);   // Hit!
    CHECK(chat.cached_line_count(0) == lines_at_40);
}

TEST_CASE("ChatContainer benchmark confirms rendering 50 historical messages is sub-millisecond on repeated frames",
        "[coding_agent][tui][issue602][benchmark]") {
    auto theme = test_theme();
    coding_agent::tui::ChatContainer chat(theme, test_keybinding_slot());

    // Populate 50 historical turns with markdown and code blocks
    for (std::size_t i = 0; i < 25; ++i) {
        chat.append_committed_message(ai::user_text_message(
                std::format("User query #{}: How do I optimize the database indexing for query pattern {}?", i, i)));

        ai::AssistantMessage assistant;
        assistant.content.push_back(ai::TextContent{
                .text = std::format(
                        "### Optimization Strategy for Pattern {}\n\n"
                        "To optimize query pattern {}, ensure composite index `(tenant_id, created_at)` is present:\n"
                        "```sql\n"
                        "CREATE INDEX idx_tenant_created_{} ON transactions (tenant_id, created_at DESC);\n"
                        "```\n\n"
                        "This guarantees index-only scans for high-throughput pagination.",
                        i,
                        i,
                        i),
        });
        assistant.stop_reason = ai::AssistantStopReason::Stop;
        chat.append_committed_message(assistant);
    }

    REQUIRE(chat.item_count() == 50);
    REQUIRE(chat.committed_item_count() == 50);

    // Warm-up render: renders all 50 items and freezes lines in cache.
    const auto warmup = chat.render(100);
    REQUIRE(warmup);
    CHECK(chat.cold_render_count() == 50);
    CHECK(chat.cache_hit_count() == 0);

    // Benchmark 100 consecutive frames
    constexpr int kFrames = 100;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto rendered = chat.render(100);
        REQUIRE(rendered);
    }
    const auto duration = std::chrono::steady_clock::now() - start;
    const auto total_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    const double avg_microseconds_per_frame = static_cast<double>(total_microseconds) / kFrames;

    // Issue #602 acceptance criterion: <0.5ms (<500 microseconds) on repeated frames!
    CHECK(avg_microseconds_per_frame < 500.0);
    // Cold render count must NOT have increased at all!
    CHECK(chat.cold_render_count() == 50);
    // Exactly 50 items * 100 frames = 5000 cache hits!
    CHECK(chat.cache_hit_count() == 50 * kFrames);
}
