#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentLoop.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/agent/ToolRegistry.hpp"
#include "../../include/cch/ai/ChatClient.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/ai/ProviderRegistry.hpp"
#include "../../include/cch/ai/StreamEvent.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/ai/Usage.hpp"
#include "../../include/cch/ai/providers/OpenAIChatClient.hpp"
#include "../../include/cch/ai/providers/StreamTransport.hpp"
#include "../../include/cch/coding_agent/AgentConfigDir.hpp"
#include "../../include/cch/coding_agent/Settings.hpp"
#include "../../include/cch/coding_agent/Sdk.hpp"
#include "../../include/cch/harness/ExecutionEnv.hpp"
#include "../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../include/cch/harness/session/SessionEntry.hpp"
#include "../../include/cch/harness/session/SessionStore.hpp"
#include "../../include/cch/tools/ToolFactories.hpp"
#include "../../include/cch/util/Error.hpp"

#include <filesystem>
#include <optional>
#include <type_traits>
#include <variant>

using namespace cch;

TEST_CASE("public headers compile from the include contract surface", "[architecture][u1]") {
    ai::AiContext context;
    context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    context.tools.push_back(ai::Tool{"read_file", "Read", ai::JsonSchema::object()});

    agent::AsyncAgentOptions options;
    options.max_turns = 2;
    options.model = "gpt-test";

    harness::session::SessionMetadata metadata;
    metadata.session_id = "session-1";
    metadata.provider = "fake";
    metadata.model = options.model;

    CHECK(context.messages.size() == 1);
    CHECK(context.tools.size() == 1);
    CHECK(metadata.model == "gpt-test");
}

TEST_CASE("public contracts remain value and interface oriented", "[architecture][u1]") {
    static_assert(std::is_move_constructible_v<ai::MessageVariant>);
    static_assert(std::is_move_constructible_v<ai::Content>);
    static_assert(std::is_abstract_v<ai::StreamingChatClient>);
    static_assert(std::is_abstract_v<ai::providers::StreamTransport>);
    static_assert(std::is_abstract_v<harness::AsyncExecutionEnv>);
    static_assert(std::is_abstract_v<agent::AsyncAgentTool>);
    static_assert(std::is_abstract_v<harness::session::SessionStore>);
    static_assert(std::is_base_of_v<
                  harness::session::SessionStore,
                  harness::session::JsonlSessionStore>);
    static_assert(std::is_same_v<
                  decltype(std::declval<const harness::session::SessionStore&>().path()),
                  std::optional<std::filesystem::path>>);

    ai::AssistantStreamEvent stream_event = ai::TextDeltaEvent{};
    agent::AgentLifecycleEvent agent_event = agent::TurnStartEvent{};

    CHECK(std::holds_alternative<ai::TextDeltaEvent>(stream_event));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(agent_event));
}

TEST_CASE("SDK targets remain one passive variant with optional path results", "[architecture][session][sdk]") {
    static_assert(std::is_aggregate_v<coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitNewSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitResumeSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::InMemorySessionTarget>);
    static_assert(std::variant_size_v<coding_agent::SessionTarget> == 4);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<0, coding_agent::SessionTarget>,
                  coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<3, coding_agent::SessionTarget>,
                  coding_agent::InMemorySessionTarget>);
    static_assert(std::is_same_v<
                  decltype(coding_agent::CreateAgentSessionResult::session_path),
                  std::optional<std::filesystem::path>>);
    static_assert(std::is_same_v<
                  decltype(std::declval<const coding_agent::AgentSession&>().session_path()),
                  const std::optional<std::filesystem::path>&>);

    coding_agent::CreateAgentSessionOptions options;
    CHECK(std::holds_alternative<coding_agent::DefaultPersistedSessionTarget>(options.session_target));
}

TEST_CASE("agent lifecycle advertises only the supported pi event alternatives", "[architecture][agent]") {
    static_assert(std::variant_size_v<agent::AgentLifecycleEvent> == 9);
    static_assert(std::is_same_v<std::variant_alternative_t<0, agent::AgentLifecycleEvent>, agent::AgentStartEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, agent::AgentLifecycleEvent>, agent::AgentEndEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, agent::AgentLifecycleEvent>, agent::TurnStartEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, agent::AgentLifecycleEvent>, agent::TurnEndEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, agent::AgentLifecycleEvent>, agent::MessageStartEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<5, agent::AgentLifecycleEvent>, agent::MessageUpdateEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<6, agent::AgentLifecycleEvent>, agent::MessageEndEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<7, agent::AgentLifecycleEvent>, agent::ToolExecutionStartEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<8, agent::AgentLifecycleEvent>, agent::ToolExecutionEndEvent>);
}
