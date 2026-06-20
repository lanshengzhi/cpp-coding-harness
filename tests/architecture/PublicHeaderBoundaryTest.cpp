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
#include "../../include/cch/ai/providers/BoostBeastStreamTransport.hpp"
#include "../../include/cch/ai/providers/OpenAIChatClient.hpp"
#include "../../include/cch/ai/providers/SseParser.hpp"
#include "../../include/cch/ai/providers/StreamTransport.hpp"
#include "../../include/cch/coding_agent/Config.hpp"
#include "../../include/cch/harness/ExecutionEnv.hpp"
#include "../../include/cch/harness/LocalExecutionEnv.hpp"
#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../include/cch/harness/session/SessionEntry.hpp"
#include "../../include/cch/tools/ToolFactories.hpp"
#include "../../include/cch/util/Error.hpp"

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

    ai::AssistantStreamEvent stream_event = ai::TextDeltaEvent{};
    agent::AgentLifecycleEvent agent_event = agent::TurnStartEvent{};

    CHECK(std::holds_alternative<ai::TextDeltaEvent>(stream_event));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(agent_event));
}
