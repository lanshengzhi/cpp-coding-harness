#include <catch2/catch_test_macros.hpp>

#include "../../include/cch/agent/Agent.hpp"
#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include <cch/ai/RequestOptions.hpp>
#include <cch/ai/StreamEvent.hpp>
#include "ai/providers/StreamTransport.hpp"

#include <memory>
#include <type_traits>

using namespace cch;

TEST_CASE("event sink contracts are move-only", "[architecture][u5]") {
    static_assert(!std::is_copy_constructible_v<agent::Agent>);
    static_assert(!std::is_copy_assignable_v<agent::Agent>);
    static_assert(std::is_move_constructible_v<agent::Agent>);
    static_assert(std::is_move_assignable_v<agent::Agent>);

    static_assert(!std::is_copy_constructible_v<agent::AgentEventSubscription>);
    static_assert(!std::is_copy_assignable_v<agent::AgentEventSubscription>);
    static_assert(std::is_move_constructible_v<agent::AgentEventSubscription>);
    static_assert(std::is_move_assignable_v<agent::AgentEventSubscription>);

    static_assert(!std::is_copy_constructible_v<agent::AgentEventSink>);
    static_assert(!std::is_copy_assignable_v<agent::AgentEventSink>);
    static_assert(std::is_move_constructible_v<agent::AgentEventSink>);
    static_assert(!std::is_copy_constructible_v<agent::ToolUpdateSink>);
    static_assert(!std::is_copy_assignable_v<agent::ToolUpdateSink>);
    static_assert(std::is_move_constructible_v<agent::ToolUpdateSink>);

    static_assert(!std::is_copy_constructible_v<agent::AsyncAgentOptions>);
    static_assert(!std::is_copy_assignable_v<agent::AsyncAgentOptions>);
    static_assert(std::is_move_constructible_v<agent::AsyncAgentOptions>);
    static_assert(std::is_move_assignable_v<agent::AsyncAgentOptions>);

    static_assert(!std::is_copy_constructible_v<agent::TransformContextHook>);
    static_assert(std::is_move_constructible_v<agent::TransformContextHook>);
    static_assert(!std::is_copy_constructible_v<agent::ConvertToLlmHook>);
    static_assert(std::is_move_constructible_v<agent::ConvertToLlmHook>);
    static_assert(!std::is_copy_constructible_v<agent::PrepareNextTurnHook>);
    static_assert(std::is_move_constructible_v<agent::PrepareNextTurnHook>);
    static_assert(!std::is_copy_constructible_v<agent::ShouldStopAfterTurnHook>);
    static_assert(std::is_move_constructible_v<agent::ShouldStopAfterTurnHook>);
    static_assert(!std::is_copy_constructible_v<agent::ValidateTurnUpdateHook>);
    static_assert(std::is_move_constructible_v<agent::ValidateTurnUpdateHook>);

    static_assert(!std::is_copy_constructible_v<ai::AssistantEventSink>);
    static_assert(std::is_move_constructible_v<ai::AssistantEventSink>);
    static_assert(!std::is_copy_constructible_v<ai::TransformHeadersHook>);
    static_assert(std::is_move_constructible_v<ai::TransformHeadersHook>);
    static_assert(!std::is_copy_constructible_v<ai::SimpleStreamOptions>);
    static_assert(std::is_move_constructible_v<ai::SimpleStreamOptions>);

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
    auto emitted = sink(agent::TurnStartEvent{});

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
