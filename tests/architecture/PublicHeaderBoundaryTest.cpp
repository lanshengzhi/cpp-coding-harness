#include <cch/agent/Agent.hpp>
#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/ChatClient.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/ProviderRegistry.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/ai/Usage.hpp>
#include <cch/ai/providers/OpenAIChatClient.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/harness/ExecutionEnv.hpp>
#include <cch/harness/LocalExecutionEnv.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionEntry.hpp>
#include <cch/harness/session/SessionStore.hpp>
#include <cch/tools/ToolFactories.hpp>
#include <cch/tui/CancellableLoader.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Style.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/TruncatedText.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/util/Error.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <filesystem>
#include <optional>
#include <type_traits>
#include <variant>

using namespace cch;

TEST_CASE("public headers compile from the include contract surface", "[architecture][u1]") {
    ai::AiContext context;
    context.messages.push_back(ai::MessageVariant{ai::user_text_message("hello")});
    context.tools.push_back(ai::Tool{
        "read_file",
        "Read",
        util::JsonValue::object_t{{"type", "object"}, {"additionalProperties", false}},
    });
    static_assert(std::is_same_v<decltype(ai::Tool::parameters), util::JsonValue>);

    agent::AsyncAgentOptions options;
    options.max_turns = 2;
    options.model = ai::Model{"gpt-test"};

    harness::session::SessionMetadata metadata;
    metadata.session_id = "session-1";
    metadata.provider = "fake";
    metadata.model = options.model.id;

    CHECK(context.messages.size() == 1);
    CHECK(context.tools.size() == 1);
    CHECK(metadata.model == "gpt-test");
}

TEST_CASE("public contracts remain value and interface oriented", "[architecture][u1]") {
    static_assert(std::is_aggregate_v<agent::AgentInitialState>);
    static_assert(std::is_aggregate_v<ai::Model>);
    static_assert(std::is_copy_constructible_v<agent::AgentState>);
    static_assert(std::is_same_v<
                  decltype(std::declval<const agent::Agent&>().state()),
                  agent::AgentState>);
    static_assert(std::is_aggregate_v<coding_agent::AgentSessionSnapshot>);
    static_assert(std::is_copy_constructible_v<coding_agent::AgentSessionSnapshot>);
    static_assert(std::is_same_v<
                  decltype(std::declval<const coding_agent::AgentSession&>().snapshot()),
                  coding_agent::AgentSessionSnapshot>);
    static_assert(std::is_move_constructible_v<ai::MessageVariant>);
    static_assert(std::is_move_constructible_v<ai::Content>);
    static_assert(std::is_abstract_v<ai::StreamingChatClient>);
    static_assert(std::is_abstract_v<ai::providers::StreamTransport>);
    static_assert(std::is_abstract_v<harness::AsyncExecutionEnv>);
    using ReadTextFileMethod = boost::asio::awaitable<std::expected<std::string, harness::FileError>>
        (harness::AsyncExecutionEnv::*)(std::string, std::stop_token);
    using CleanupMethod = boost::asio::awaitable<void> (harness::AsyncExecutionEnv::*)();
    using ToolExecuteMethod = boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>>
        (agent::AsyncAgentTool::*)(agent::ToolInvocation, std::stop_token);
    static_assert(std::is_same_v<
                  decltype(&harness::AsyncExecutionEnv::readTextFile),
                  ReadTextFileMethod>);
    static_assert(std::is_same_v<decltype(&harness::AsyncExecutionEnv::cleanup), CleanupMethod>);
    static_assert(std::is_same_v<decltype(&agent::AsyncAgentTool::execute), ToolExecuteMethod>);
    static_assert(std::is_abstract_v<agent::AsyncAgentTool>);
    static_assert(std::is_abstract_v<harness::session::SessionStore>);
    static_assert(std::is_abstract_v<tui::Component>);
    static_assert(std::is_aggregate_v<tui::RenderResult>);
    static_assert(std::is_aggregate_v<tui::InlineImageRenderRegion>);
    static_assert(std::is_aggregate_v<tui::ImageContent>);
    static_assert(std::is_aggregate_v<tui::ImageCellConstraints>);
    static_assert(std::is_aggregate_v<tui::ImageOptions>);
    static_assert(std::is_aggregate_v<tui::CellRegion>);
    static_assert(std::is_aggregate_v<tui::CellPixelDimensions>);
    static_assert(std::is_aggregate_v<tui::TerminalCapabilities>);
    static_assert(std::is_enum_v<tui::TerminalColorCapability>);
    static_assert(std::is_enum_v<tui::TerminalAppearance>);
    static_assert(std::is_aggregate_v<tui::TerminalImage>);
    static_assert(std::is_aggregate_v<tui::ProcessTerminalOptions>);
    static_assert(std::is_enum_v<tui::InlineImageProtocol>);
    static_assert(std::is_enum_v<tui::KeyboardProtocol>);
    static_assert(std::is_final_v<tui::ProcessTerminal>);
    static_assert(std::is_aggregate_v<tui::KeyEvent>);
    static_assert(std::is_aggregate_v<tui::PasteEvent>);
    static_assert(std::is_aggregate_v<tui::KeybindingDefinition>);
    static_assert(std::is_aggregate_v<tui::KeybindingOverride>);
    static_assert(std::is_aggregate_v<tui::KeybindingIssue>);
    static_assert(std::is_aggregate_v<tui::EffectiveKeybinding>);
    static_assert(std::is_aggregate_v<tui::KeybindingResolutionRequest>);
    static_assert(std::is_aggregate_v<tui::KeybindingResolution>);
    static_assert(std::is_final_v<tui::KeybindingRegistry>);
    static_assert(std::is_aggregate_v<tui::EditorCursor>);
    static_assert(std::is_aggregate_v<tui::AutocompleteItem>);
    static_assert(std::is_aggregate_v<tui::AutocompleteRequest>);
    static_assert(std::is_aggregate_v<tui::AutocompleteSuggestions>);
    static_assert(std::is_aggregate_v<tui::EditorTheme>);
    static_assert(std::is_move_constructible_v<tui::AutocompleteProvider>);
    static_assert(!std::is_copy_constructible_v<tui::AutocompleteProvider>);
    static_assert(std::is_aggregate_v<tui::SelectItem>);
    static_assert(std::is_aggregate_v<tui::SelectListOptions>);
    static_assert(std::is_aggregate_v<tui::SettingItem>);
    static_assert(std::is_aggregate_v<tui::SettingsListOptions>);
    static_assert(std::variant_size_v<tui::SettingControlVariant> == 3);
    static_assert(std::is_aggregate_v<tui::LoaderIndicatorOptions>);
    static_assert(std::is_aggregate_v<tui::LoaderOptions>);
    static_assert(std::is_aggregate_v<tui::CancellableLoaderOptions>);
    static_assert(std::is_abstract_v<tui::AnimationTimer>);
    static_assert(std::is_move_constructible_v<tui::TextStyleHook>);
    static_assert(!std::is_copy_constructible_v<tui::TextStyleHook>);
    static_assert(std::is_move_constructible_v<tui::SettingsChangeSink>);
    static_assert(!std::is_copy_constructible_v<tui::SettingsChangeSink>);
    static_assert(std::is_final_v<tui::Editor>);
    static_assert(std::is_final_v<tui::SelectList>);
    static_assert(std::is_final_v<tui::SettingsList>);
    static_assert(std::is_final_v<tui::Loader>);
    static_assert(std::is_final_v<tui::CancellableLoader>);
    static_assert(std::variant_size_v<tui::InputEventVariant> == 2);
    static_assert(std::is_abstract_v<tui::InputHandler>);
    static_assert(std::is_abstract_v<tui::Focusable>);
    static_assert(std::is_abstract_v<tui::Terminal>);
    static_assert(std::is_move_constructible_v<tui::BackgroundHook>);
    static_assert(!std::is_copy_constructible_v<tui::BackgroundHook>);
    static_assert(std::is_aggregate_v<tui::MarkdownStyleConfig>);
    static_assert(std::is_move_constructible_v<tui::MarkdownStyleHook>);
    static_assert(!std::is_copy_constructible_v<tui::MarkdownStyleHook>);
    static_assert(std::is_move_constructible_v<tui::SyntaxHighlightHook>);
    static_assert(!std::is_copy_constructible_v<tui::SyntaxHighlightHook>);
    static_assert(std::is_move_constructible_v<tui::Markdown>);
    static_assert(!std::is_copy_constructible_v<tui::Markdown>);
    static_assert(std::is_move_constructible_v<tui::Container>);
    static_assert(!std::is_copy_constructible_v<tui::Container>);
    static_assert(std::is_move_constructible_v<tui::Box>);
    static_assert(!std::is_copy_constructible_v<tui::Box>);
    static_assert(std::is_move_constructible_v<tui::Text>);
    static_assert(!std::is_copy_constructible_v<tui::Text>);
    static_assert(std::is_final_v<tui::Container>);
    static_assert(std::is_final_v<tui::Box>);
    static_assert(std::is_final_v<tui::Spacer>);
    static_assert(std::is_final_v<tui::Text>);
    static_assert(std::is_final_v<tui::TruncatedText>);
    static_assert(std::is_final_v<tui::Image>);
    static_assert(std::is_move_constructible_v<tui::Image>);
    static_assert(!std::is_copy_constructible_v<tui::Image>);
    static_assert(std::is_final_v<tui::Markdown>);
    static_assert(std::is_final_v<tui::Tui>);
    static_assert(std::is_aggregate_v<tui::VirtualTerminalStyle>);
    static_assert(std::is_aggregate_v<tui::VirtualTerminalCell>);
    static_assert(std::is_aggregate_v<tui::VirtualTerminalImage>);
    static_assert(std::is_final_v<tui::VirtualTerminal>);
    // ADR 0006: the local environment uniquely owns its synchronous state, so
    // environment copies cannot alias live state.
    static_assert(!std::is_copy_constructible_v<harness::AsyncLocalExecutionEnv>);
    static_assert(!std::is_copy_assignable_v<harness::AsyncLocalExecutionEnv>);
    static_assert(std::is_move_constructible_v<harness::AsyncLocalExecutionEnv>);
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
