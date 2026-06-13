#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/ai/ChatClient.hpp"
#include "../../include/cch/ai/providers/StreamTransport.hpp"

#include <memory>
#include <type_traits>

using namespace cch;

TEST_CASE("event sink contracts are move-only", "[architecture][u5]") {
    static_assert(!std::is_copy_constructible_v<agent::AgentEventSink>);
    static_assert(!std::is_copy_assignable_v<agent::AgentEventSink>);
    static_assert(std::is_move_constructible_v<agent::AgentEventSink>);

    static_assert(!std::is_copy_constructible_v<ai::AssistantEventSink>);
    static_assert(std::is_move_constructible_v<ai::AssistantEventSink>);

    static_assert(!std::is_copy_constructible_v<ai::providers::BodyChunkHandler>);
    static_assert(std::is_move_constructible_v<ai::providers::BodyChunkHandler>);
}

TEST_CASE("move-only event sinks can own unique state", "[architecture][u5]") {
    int observed = 0;
    agent::AgentEventSink sink = [state = std::make_unique<int>(41), &observed](const agent::AgentLifecycleEvent&) mutable {
        observed = *state + 1;
        return util::ExpectedVoid{};
    };

    REQUIRE(sink);
    auto emitted = sink(agent::TurnStartEvent{1});

    REQUIRE(emitted);
    CHECK(observed == 42);
}

TEST_CASE("move-only body handlers can own unique buffers", "[architecture][u5]") {
    int observed = 0;
    ai::providers::BodyChunkHandler handler = [buffer = std::make_unique<std::string>(), &observed](std::string_view chunk) mutable {
        *buffer += chunk;
        observed = static_cast<int>(buffer->size());
        return util::ExpectedVoid{};
    };

    REQUIRE(handler);
    auto handled = handler("abc");

    REQUIRE(handled);
    CHECK(observed == 3);
}
