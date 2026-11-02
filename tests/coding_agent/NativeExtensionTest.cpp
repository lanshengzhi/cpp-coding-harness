// #1112 native extension lifecycle and run triggering (pi-v1.3.0).
//
// The acceptance properties are lifecycle properties, not registration ones:
// a native extension must observe the busy session in `before_agent_start`,
// its system-prompt and custom-message contributions must reach every request
// of the run (including after a tool turn), and `send_message` from an idle
// session must start a real second run. A tool-only seam and a queue-only seam
// both fail these cases; the tests below separate them.
//
// Frozen evidence: fixtures/pi-v1.3.0/audit-1049/extension-run.json,
// `before_agent_start observes busy state and contributes prompt plus custom
// message` and `extension-triggered run starts from an idle session and
// retains contributions across a tool turn`. The test uses the scripted
// provider seam and an in-memory Session, so no live keys or network.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/extensions/NativeExtension.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using tests::run_awaitable;

namespace {

constexpr std::string_view kSystemMarker = "AUDIT_1112_SYSTEM_CONTRIBUTION";
constexpr std::string_view kContributionType = "audit-1112-contribution";
constexpr std::string_view kContributionText = "audit-1112 custom contribution";
constexpr std::string_view kTriggerType = "audit-1112-trigger";
constexpr std::string_view kTriggerText = "extension-triggered";

[[nodiscard]] ai::CustomMessage custom_message(std::string custom_type, std::string text) {
    return ai::CustomMessage{
            .custom_type = std::move(custom_type),
            .content = {ai::text_content(std::move(text))},
            .display = true,
            .details = std::nullopt,
            .timestamp = 0,
    };
}

[[nodiscard]] std::optional<std::string> custom_message_text(const ai::CustomMessage& message) {
    const auto text = ai::text_from_content(message.content);
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

/// Scripted provider: first request calls the extension tool, every later
/// request answers text. The requests are captured for the frozen comparison.
class LifecycleProvider final : public tests::ScriptedProvider {
public:
    explicit LifecycleProvider(bool call_tool_first = true)
        : ScriptedProvider("fake"), call_tool_first_(call_tool_first) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions) override {
        const std::size_t index = requests_.size();
        requests_.push_back(std::move(context));
        const bool call_tool = call_tool_first_ && index == 0;
        return ai::detail::make_model_stream(
                [model = std::move(model), index, call_tool](ai::AssistantEventSink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage response;
                    response.provider = "fake";
                    response.api = "scripted-fake";
                    response.model = model.id;
                    if (call_tool) {
                        response.content = {ai::text_content("calling ext_note")};
                        response.stop_reason = ai::AssistantStopReason::ToolUse;
                        response.content.emplace_back(ai::ToolCallContent{
                                .id = "call_ext_note",
                                .name = "ext_note",
                                .arguments = support::JsonValue::object_t{},
                                .raw_arguments = "{}",
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else if (index == 1) {
                        response.content = {ai::text_content("first run complete")};
                        response.stop_reason = ai::AssistantStopReason::Stop;
                    } else {
                        response.content = {ai::text_content("extension-triggered run complete")};
                        response.stop_reason = ai::AssistantStopReason::Stop;
                    }
                    co_return response;
                });
    }

    std::vector<ai::AiContext> requests_;

private:
    bool call_tool_first_{true};
};

struct Invocation {
    coding_agent::extensions::BeforeAgentStartEvent::MessageType message_type{
            coding_agent::extensions::BeforeAgentStartEvent::MessageType::User};
    bool is_idle{true};
    bool api_is_idle{true};
    std::string prompt{};
    std::string custom_text{};
    std::string seen_system_prompt{};
};

/// One native extension that contributes a tool, records every invocation,
/// and contributes a system prompt plus a custom message. `fail_next` makes
/// the next hook fail before the provider request.
class LifecycleExtension final : public coding_agent::extensions::NativeExtension {
public:
    explicit LifecycleExtension(std::string marker) : marker_(std::move(marker)) {}

    [[nodiscard]] support::Expected<std::vector<coding_agent::extensions::ExtensionTool>> load_tools() override {
        coding_agent::extensions::ExtensionTool tool;
        tool.definition.name = "ext_note";
        tool.definition.description = "Native extension note tool";
        tool.definition.parameters = support::JsonValue::object_t{
                {"type", "object"},
                {"properties", support::JsonValue::object_t{}},
                {"additionalProperties", false},
        };
        tool.concurrency = agent::ToolConcurrency::ParallelSafe;
        tool.prompt_snippet = "Read the native extension note";
        tool.execute = [](support::JsonValue,
                               std::stop_token) -> support::AsyncResult<coding_agent::extensions::ExtensionToolResult> {
            return support::AsyncResult<coding_agent::extensions::ExtensionToolResult>{
                    coding_agent::extensions::ExtensionToolResult{
                            .content = {ai::text_content("extension tool result")},
                            .details = std::nullopt,
                            .is_error = false,
                    }};
        };
        std::vector<coding_agent::extensions::ExtensionTool> tools;
        tools.push_back(std::move(tool));
        return tools;
    }

    [[nodiscard]] support::ExpectedVoid bind(coding_agent::extensions::ExtensionSession& session) override {
        session_ = &session;
        return {};
    }

    [[nodiscard]] support::AsyncResult<coding_agent::extensions::BeforeAgentStartResult> before_agent_start(
            coding_agent::extensions::BeforeAgentStartEvent event,
            coding_agent::extensions::ExtensionContext context) override {
        Invocation invocation;
        invocation.message_type = event.message_type;
        invocation.is_idle = context.is_idle;
        invocation.api_is_idle = session_ != nullptr && session_->is_idle();
        invocation.seen_system_prompt = event.system_prompt;
        if (event.message_type == coding_agent::extensions::BeforeAgentStartEvent::MessageType::Custom) {
            invocation.custom_text = custom_message_text(event.message).value_or("");
        } else {
            invocation.prompt = event.prompt;
        }
        invocations.push_back(invocation);
        if (fail_next) {
            fail_next = false;
            return support::AsyncResult<coding_agent::extensions::BeforeAgentStartResult>{std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "native extension before_agent_start failed"))};
        }
        coding_agent::extensions::BeforeAgentStartResult result;
        result.system_prompt = event.system_prompt + "\n" + marker_;
        result.message = custom_message(std::string{kContributionType}, std::string{kContributionText});
        return support::AsyncResult<coding_agent::extensions::BeforeAgentStartResult>{std::move(result)};
    }

    [[nodiscard]] coding_agent::extensions::ExtensionSession* session() const { return session_; }

    std::vector<Invocation> invocations;
    bool fail_next{false};

private:
    std::string marker_;
    /// The binding is a Session member that outlives the extension; the
    /// extension never outlives the Session in these cases.
    coding_agent::extensions::ExtensionSession* session_{nullptr};
};

/// A second extension with no tools, used for the registration-order case.
class OrderingExtension final : public coding_agent::extensions::NativeExtension {
public:
    OrderingExtension(std::string marker, std::string message)
        : marker_(std::move(marker)), message_(std::move(message)) {}

    [[nodiscard]] support::Expected<std::vector<coding_agent::extensions::ExtensionTool>> load_tools() override {
        return std::vector<coding_agent::extensions::ExtensionTool>{};
    }

    [[nodiscard]] support::ExpectedVoid bind(coding_agent::extensions::ExtensionSession&) override { return {}; }

    [[nodiscard]] support::AsyncResult<coding_agent::extensions::BeforeAgentStartResult> before_agent_start(
            coding_agent::extensions::BeforeAgentStartEvent event,
            coding_agent::extensions::ExtensionContext) override {
        seen_system_prompts.push_back(event.system_prompt);
        coding_agent::extensions::BeforeAgentStartResult result;
        result.system_prompt = event.system_prompt + "\n" + marker_;
        result.message = custom_message("audit-1112-order", message_);
        return support::AsyncResult<coding_agent::extensions::BeforeAgentStartResult>{std::move(result)};
    }

    std::vector<std::string> seen_system_prompts;

private:
    std::string marker_;
    std::string message_;
};

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<std::shared_ptr<coding_agent::extensions::NativeExtension>> extensions) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.native_extensions = std::move(extensions);
    auto models = tests::models_from_provider(std::move(provider));
    return runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
}

[[nodiscard]] std::vector<std::string> raw_roles(const ai::AiContext& context) {
    std::vector<std::string> roles;
    roles.reserve(context.messages.size());
    for (const auto& message : context.messages) {
        roles.push_back(std::visit(
                [](const auto& value) -> std::string {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, ai::SystemMessage>) {
                        return "system";
                    } else if constexpr (std::is_same_v<Value, ai::UserMessage>) {
                        return "user";
                    } else if constexpr (std::is_same_v<Value, ai::AssistantMessage>) {
                        return "assistant";
                    } else if constexpr (std::is_same_v<Value, ai::ToolResultMessage>) {
                        return "toolResult";
                    } else if constexpr (std::is_same_v<Value, ai::CustomMessage>) {
                        return "custom";
                    } else if constexpr (std::is_same_v<Value, ai::BashExecutionMessage>) {
                        return "bashExecution";
                    } else {
                        return "other";
                    }
                },
                message));
    }
    return roles;
}

[[nodiscard]] std::size_t contribution_count(const ai::AiContext& context) {
    return static_cast<std::size_t>(std::ranges::count_if(context.messages, [](const auto& message) {
        const auto* custom = std::get_if<ai::CustomMessage>(&message);
        return custom != nullptr && custom->custom_type == kContributionType;
    }));
}

[[nodiscard]] std::optional<std::string> last_tool_result_text(const ai::AiContext& context) {
    for (auto iterator = context.messages.rbegin(); iterator != context.messages.rend(); ++iterator) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&*iterator)) {
            return ai::text_from_content(result->content);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a native extension observes the busy start, contributes a prompt and message, and keeps them across a tool "
          "turn",
        "[coding_agent][extension][issue1112][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto provider = std::make_shared<LifecycleProvider>();
    auto extension = std::make_shared<LifecycleExtension>(std::string{kSystemMarker});
    std::vector<std::shared_ptr<coding_agent::extensions::NativeExtension>> extensions{extension};
    auto created = create_session(runtime, workspace, provider, std::move(extensions));
    REQUIRE(created.has_value());
    auto& session = runtime.adopt_session(std::move(created->session));

    REQUIRE(run_awaitable(runtime, session.prompt("start")).has_value());

    // The first run observes the hook while the session is busy; the hook's
    // API-side idle check agrees with the context fact.
    REQUIRE(extension->invocations.size() == 1);
    const auto& first = extension->invocations.front();
    CHECK(first.message_type == coding_agent::extensions::BeforeAgentStartEvent::MessageType::User);
    CHECK_FALSE(first.is_idle);
    CHECK_FALSE(first.api_is_idle);
    CHECK(first.prompt == "start");
    CHECK(first.custom_text.empty());
    CHECK(first.seen_system_prompt.find(kSystemMarker) == std::string::npos);

    // Two requests in the first run: the initial one and the request after the
    // extension tool result. Both carry the system contribution and the custom
    // message; the second also carries the tool result.
    REQUIRE(provider->requests_.size() == 2);
    const auto& first_request = provider->requests_[0];
    REQUIRE(first_request.system_prompt.has_value());
    CHECK(first_request.system_prompt->find(kSystemMarker) != std::string::npos);
    CHECK(raw_roles(first_request) == std::vector<std::string>{"system", "user", "custom"});
    CHECK(contribution_count(first_request) == 1);
    CHECK_FALSE(last_tool_result_text(first_request).has_value());

    const auto& tool_request = provider->requests_[1];
    REQUIRE(tool_request.system_prompt.has_value());
    CHECK(tool_request.system_prompt->find(kSystemMarker) != std::string::npos);
    CHECK(raw_roles(tool_request) == std::vector<std::string>{"system", "user", "custom", "assistant", "toolResult"});
    CHECK(contribution_count(tool_request) == 1);
    CHECK(last_tool_result_text(tool_request) == std::optional<std::string>{"extension tool result"});
    CHECK_FALSE(session.is_busy());
    CHECK(extension->session()->is_idle());

    // From idle, the extension's own `sendMessage({triggerTurn: true})` starts
    // a second run. The triggered run sees the busy state and the custom
    // trigger message, and its request retains both the prior tool turn and
    // both contributions.
    REQUIRE(extension->session() != nullptr);
    auto sent =
            extension->session()->send_message(custom_message(std::string{kTriggerType}, std::string{kTriggerText}));
    REQUIRE(run_awaitable(runtime, std::move(sent)).has_value());

    REQUIRE(extension->invocations.size() == 2);
    const auto& triggered = extension->invocations[1];
    CHECK(triggered.message_type == coding_agent::extensions::BeforeAgentStartEvent::MessageType::Custom);
    CHECK_FALSE(triggered.is_idle);
    CHECK_FALSE(triggered.api_is_idle);
    CHECK(triggered.prompt.empty());
    CHECK(triggered.custom_text == kTriggerText);
    // pi's `before_agent_start` event system prompt is the run's structural
    // prompt; the previous run's handler replacement is run-local, not part of
    // the next run's base prompt. The handler adds the marker again below.
    CHECK(triggered.seen_system_prompt.find(kSystemMarker) == std::string::npos);

    REQUIRE(provider->requests_.size() == 3);
    const auto& triggered_request = provider->requests_[2];
    REQUIRE(triggered_request.system_prompt.has_value());
    CHECK(triggered_request.system_prompt->find(kSystemMarker) != std::string::npos);
    CHECK(raw_roles(triggered_request) ==
            std::vector<std::string>{
                    "system", "user", "custom", "assistant", "toolResult", "assistant", "custom", "custom"});
    CHECK(contribution_count(triggered_request) == 2);
    CHECK(last_tool_result_text(triggered_request) == std::optional<std::string>{"extension tool result"});
    CHECK_FALSE(session.is_busy());

    // The live transcript mirrors the frozen final message shape: the trigger
    // message precedes the triggered run's contribution.
    const auto snapshot = session.snapshot();
    std::vector<std::pair<std::string, std::string>> custom_messages;
    for (const auto& message : snapshot.agent_state.messages) {
        if (const auto* custom = std::get_if<ai::CustomMessage>(&message)) {
            custom_messages.emplace_back(custom->custom_type, custom_message_text(*custom).value_or(""));
        }
    }
    REQUIRE(custom_messages.size() == 3);
    CHECK(custom_messages[0] ==
            std::pair<std::string, std::string>{"audit-1112-contribution", std::string{kContributionText}});
    CHECK(custom_messages[1] ==
            std::pair<std::string, std::string>{std::string{kTriggerType}, std::string{kTriggerText}});
    CHECK(custom_messages[2] ==
            std::pair<std::string, std::string>{"audit-1112-contribution", std::string{kContributionText}});
}

TEST_CASE("before_agent_start dispatches in registration order and later handlers see the prompt replacement",
        "[coding_agent][extension][issue1112][ordering][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto provider = std::make_shared<LifecycleProvider>(false);
    auto first = std::make_shared<OrderingExtension>("ORDER_FIRST", "first contribution");
    auto second = std::make_shared<OrderingExtension>("ORDER_SECOND", "second contribution");
    std::vector<std::shared_ptr<coding_agent::extensions::NativeExtension>> extensions{first, second};
    auto created = create_session(runtime, workspace, provider, std::move(extensions));
    REQUIRE(created.has_value());
    auto& session = runtime.adopt_session(std::move(created->session));

    REQUIRE(run_awaitable(runtime, session.prompt("order")).has_value());
    REQUIRE(provider->requests_.size() == 1);
    const auto& request = provider->requests_[0];
    REQUIRE(request.system_prompt.has_value());

    // Each handler observed the prompt returned by the handler before it.
    REQUIRE(first->seen_system_prompts.size() == 1);
    REQUIRE(second->seen_system_prompts.size() == 1);
    CHECK(first->seen_system_prompts[0].find("ORDER_FIRST") == std::string::npos);
    CHECK(second->seen_system_prompts[0].find("ORDER_FIRST") != std::string::npos);
    CHECK(second->seen_system_prompts[0].find("ORDER_SECOND") == std::string::npos);

    // The final replacement wins for the request, and the injected messages
    // follow the starting message in registration order.
    const auto first_marker = request.system_prompt->find("ORDER_FIRST");
    const auto second_marker = request.system_prompt->find("ORDER_SECOND");
    REQUIRE(first_marker != std::string::npos);
    REQUIRE(second_marker != std::string::npos);
    CHECK(first_marker < second_marker);
    CHECK(raw_roles(request) == std::vector<std::string>{"system", "user", "custom", "custom"});
    const auto* first_custom = std::get_if<ai::CustomMessage>(&request.messages[2]);
    const auto* second_custom = std::get_if<ai::CustomMessage>(&request.messages[3]);
    REQUIRE(first_custom != nullptr);
    REQUIRE(second_custom != nullptr);
    CHECK(custom_message_text(*first_custom) == std::optional<std::string>{"first contribution"});
    CHECK(custom_message_text(*second_custom) == std::optional<std::string>{"second contribution"});
}

TEST_CASE("a before_agent_start failure ends the run before the provider request and leaves the session reusable",
        "[coding_agent][extension][issue1112][failure][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto provider = std::make_shared<LifecycleProvider>();
    auto extension = std::make_shared<LifecycleExtension>(std::string{kSystemMarker});
    std::vector<std::shared_ptr<coding_agent::extensions::NativeExtension>> extensions{extension};
    auto created = create_session(runtime, workspace, provider, std::move(extensions));
    REQUIRE(created.has_value());
    auto& session = runtime.adopt_session(std::move(created->session));

    extension->fail_next = true;
    const auto failed = run_awaitable(runtime, session.prompt("fails"));
    REQUIRE_FALSE(failed.has_value());
    // The dispatch failed before the run started, so no provider request and
    // no starting message were committed.
    CHECK(provider->requests_.empty());
    CHECK_FALSE(session.is_busy());
    CHECK(extension->invocations.size() == 1);

    REQUIRE(run_awaitable(runtime, session.prompt("recovers")).has_value());
    REQUIRE(provider->requests_.size() == 2);
    REQUIRE(extension->invocations.size() == 2);
    CHECK_FALSE(extension->invocations[1].is_idle);
    CHECK(provider->requests_[0].system_prompt.has_value());
    CHECK(provider->requests_[0].system_prompt->find(kSystemMarker) != std::string::npos);
}
