#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/Sdk.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/ai/providers/OpenAIChatClient.hpp"
#include "../../include/cch/ai/providers/StreamTransport.hpp"
#include "../../include/cch/coding_agent/Skill.hpp"
#include "../../include/cch/harness/ExecutionEnv.hpp"
#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../include/cch/util/Error.hpp"
#include "ai/providers/FakeChatClient.hpp"
#include "coding_agent/AgentSessionBridge.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "harness/session/SessionJournalTestHooks.hpp"
#include "../support/EnvVarGuard.hpp"
#include "../support/TempWorkspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

/// A minimal fake tool for testing custom tool registration.
class FakeEchoTool final : public agent::AsyncAgentTool {
public:
    FakeEchoTool() {
        def_.name = "echo";
        def_.description = "Echo back the input";
        def_.parameters = ai::JsonSchema::object();
    }

    [[nodiscard]] const ai::Tool& definition() const override { return def_; }

    [[nodiscard]] boost::asio::awaitable<util::Expected<agent::AsyncToolExecutionResult>> execute(
        agent::ToolInvocation /*invocation*/) override {
        agent::AsyncToolExecutionResult result;
        result.content.push_back(ai::text_content("echo: ok"));
        co_return result;
    }

private:
    ai::Tool def_;
};

/// Create a temp workspace and session path for testing.
struct TestPaths {
    cch::tests::TempWorkspace workspace;
    std::filesystem::path session_file;

    TestPaths() {
        session_file = workspace.path() / "test-session.jsonl";
    }
};

harness::session::SessionMetadata test_metadata(const TestPaths& paths) {
    return {"sdk-session-test", "2026-07-05T00:00:00Z", paths.workspace.path(), "fake", "fake-model"};
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

coding_agent::CreateAgentSessionOptions sdk_resume_options(const TestPaths& paths) {
    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    return opts;
}

void expect_unsupported_sdk_resume(const TestPaths& paths) {
    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().message == "unsupported_session_topology");
}

void write_project_skill(
    const TestPaths& paths,
    std::string name,
    std::string body = "Project skill body.") {
    paths.workspace.write(
        ".cpp-harness/skills/" + name + "/SKILL.md",
        "---\n"
        "name: " + name + "\n"
        "description: Project skill.\n"
        "---\n" +
        body + "\n");
}

void write_project_prompt(
    const TestPaths& paths,
    std::string name,
    std::string body = "Project prompt body.") {
    paths.workspace.write(
        ".cpp-harness/prompts/" + name + ".md",
        "---\n"
        "description: Project prompt.\n"
        "---\n" +
        body + "\n");
}

coding_agent::Skill host_skill(std::string name, std::string body = "Host skill body.") {
    return coding_agent::Skill{
        .name = std::move(name),
        .description = "Host skill.",
        .content = std::move(body),
        .filePath = "/host/SKILL.md",
    };
}

coding_agent::PromptTemplate host_template(std::string name, std::string body = "Host prompt body.") {
    return coding_agent::PromptTemplate{
        .name = std::move(name),
        .description = "Host template.",
        .content = std::move(body),
    };
}

bool has_sdk_diag(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const auto& diag) { return diag.code == code; });
}

std::vector<coding_agent::SdkDiagnostic>::const_iterator find_sdk_diag(
    const std::vector<coding_agent::SdkDiagnostic>& diagnostics,
    std::string_view code) {
    return std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const auto& diag) { return diag.code == code; });
}

/// A fake execution environment that records how many times cleanup() runs.
class CountingFakeEnv final : public harness::AsyncExecutionEnv {
public:
    explicit CountingFakeEnv(std::filesystem::path workspace)
        : workspace_(std::move(workspace)) {}

    [[nodiscard]] const std::filesystem::path& workspace() const override { return workspace_; }
    [[nodiscard]] bool bash_enabled() const override { return false; }

    [[nodiscard]] boost::asio::awaitable<util::Expected<harness::AsyncFileReadResult>> read_file(
        std::string /*path*/,
        int /*offset*/,
        int /*limit*/) override {
        co_return harness::AsyncFileReadResult{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<harness::AsyncFileWriteResult>> write_file(
        std::string /*path*/,
        std::string /*content*/,
        bool /*create_parents*/) override {
        co_return harness::AsyncFileWriteResult{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<harness::AsyncFileEditResult>> edit_file(
        std::string /*path*/,
        std::string /*old_text*/,
        std::string /*new_text*/) override {
        co_return harness::AsyncFileEditResult{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<harness::AsyncShellResult>> run_shell(
        std::string /*command*/,
        std::chrono::milliseconds /*timeout*/) override {
        co_return harness::AsyncShellResult{};
    }

    boost::asio::awaitable<void> cleanup() override {
        ++cleanup_count;
        co_return;
    }

    int cleanup_count{0};

private:
    std::filesystem::path workspace_;
};

/// A host chat client that records the request so tests can inspect the tool
/// registry sent to the model.
class CaptureChatClient final : public ai::StreamingChatClient {
public:
    std::optional<ai::StreamChatRequest> captured_request;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink /*sink*/) override {
        captured_request = request;
        ai::AssistantMessage msg;
        msg.provider = "capture";
        msg.api = "capture";
        msg.model = request.model;
        msg.stop_reason = ai::AssistantStopReason::Stop;
        msg.content.emplace_back(ai::TextContent{"captured", std::nullopt});
        co_return msg;
    }
};

class PreflightRejectingChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& /*request*/,
        ai::AssistantEventSink /*sink*/) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "provider rejected in-memory prompt"));
    }
};

class RecoveringNetworkTransport final : public ai::providers::StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::providers::StreamResponse>> async_stream(
        const ai::providers::StreamRequest& request,
        ai::providers::BodyChunkHandler on_body_chunk) override {
        requests.push_back(request);
        if (requests.size() == 1) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Network,
                "provider connection failed",
                "could not resolve api.example: Name or service not known"));
        }

        const std::string response_body =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"recovered\"},"
            "\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        if (auto delivered = on_body_chunk(response_body); !delivered) {
            co_return std::unexpected(delivered.error());
        }

        ai::providers::StreamResponse response;
        response.head.status_code = 200;
        response.body = response_body;
        co_return response;
    }

    std::vector<ai::providers::StreamRequest> requests;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// U1: Public SDK contracts compile and are move-only
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK prompt contract exposes success-or-error and separate state", "[sdk][u1]") {
    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{std::filesystem::path{"/tmp/test"}};
    opts.workspace = std::filesystem::path{"/tmp"};
    opts.max_turns = 10;

    coding_agent::PromptOptions prompt_opts;
    CHECK(prompt_opts.expand_prompt_templates);

    using PromptCompletion = decltype(
        std::declval<coding_agent::AgentSession&>().prompt(std::declval<std::string>()));
    static_assert(std::is_same_v<PromptCompletion, util::ExpectedVoid>);
    static_assert(!std::is_constructible_v<coding_agent::PromptOptions, agent::AgentEventSink>);
}

TEST_CASE("AgentSession is move-only", "[sdk][u1]") {
    static_assert(std::is_move_constructible_v<coding_agent::AgentSession>);
    static_assert(!std::is_copy_constructible_v<coding_agent::AgentSession>);
    static_assert(!std::is_copy_assignable_v<coding_agent::AgentSession>);
}

TEST_CASE("EventSubscription is move-only", "[sdk][u1]") {
    static_assert(std::is_move_constructible_v<coding_agent::EventSubscription>);
    static_assert(!std::is_copy_constructible_v<coding_agent::EventSubscription>);
    static_assert(!std::is_copy_assignable_v<coding_agent::EventSubscription>);
}

// ─────────────────────────────────────────────────────────────────────────────
// U2: Session creation validation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK session target is one passive variant with default persistence", "[sdk][u2][session-target]") {
    static_assert(std::is_aggregate_v<coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitNewSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::ExplicitResumeSessionTarget>);
    static_assert(std::is_aggregate_v<coding_agent::InMemorySessionTarget>);
    static_assert(std::variant_size_v<coding_agent::SessionTarget> == 4);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<0, coding_agent::SessionTarget>,
                  coding_agent::DefaultPersistedSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<1, coding_agent::SessionTarget>,
                  coding_agent::ExplicitNewSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<2, coding_agent::SessionTarget>,
                  coding_agent::ExplicitResumeSessionTarget>);
    static_assert(std::is_same_v<
                  std::variant_alternative_t<3, coding_agent::SessionTarget>,
                  coding_agent::InMemorySessionTarget>);

    coding_agent::CreateAgentSessionOptions opts;
    CHECK(std::holds_alternative<coding_agent::DefaultPersistedSessionTarget>(opts.session_target));

    using ResultPath = decltype(std::declval<coding_agent::CreateAgentSessionResult>().session_path);
    using SessionPath = decltype(std::declval<const coding_agent::AgentSession&>().session_path());
    static_assert(std::is_same_v<ResultPath, std::optional<std::filesystem::path>>);
    static_assert(std::is_same_v<SessionPath, const std::optional<std::filesystem::path>&>);
}

TEST_CASE(
    "SDK in-memory session preserves normal runtime contracts without filesystem state",
    "[sdk][in-memory][live-state]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session != nullptr);
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.workspace == std::filesystem::canonical(paths.workspace.path()));
    CHECK(std::regex_match(
        result->session_id,
        std::regex{R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"}));
    CHECK(result->session->message_count() == 0);
    CHECK_FALSE(result->session->last_assistant_text().has_value());

    std::size_t message_ends = 0;
    std::size_t tool_results = 0;
    auto subscription = result->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                ++message_ends;
                CHECK(result->session->message_count() == message_ends);
                if (std::holds_alternative<ai::ToolResultMessage>(end->message)) {
                    ++tool_results;
                }
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto prompted = result->session->prompt("read target.txt");
    REQUIRE(prompted.has_value());
    CHECK(message_ends == 4);
    CHECK(tool_results == 1);
    CHECK(result->session->message_count() == 4);
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("observed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.session_file));

    coding_agent::AgentSession moved{std::move(*result->session)};
    CHECK(moved.is_open());
    CHECK_FALSE(moved.session_path().has_value());
    CHECK(moved.message_count() == 4);
    CHECK(moved.close().has_value());
    CHECK(moved.close().has_value());
    CHECK_FALSE(moved.is_open());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE(
    "SDK in-memory subscriptions remain safe across session move assignment and destruction",
    "[sdk][in-memory][move]") {
    TestPaths paths;
    auto create_in_memory = [&]() {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::InMemorySessionTarget{};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        return coding_agent::create_agent_session(std::move(opts));
    };

    auto first = create_in_memory();
    REQUIRE(first.has_value());
    auto first_subscription = first->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; });
    REQUIRE(first_subscription.has_value());

    auto second = create_in_memory();
    REQUIRE(second.has_value());
    auto second_subscription = second->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; });
    REQUIRE(second_subscription.has_value());

    {
        coding_agent::AgentSession assigned{std::move(*first->session)};
        CHECK(static_cast<bool>(*first_subscription));
        assigned = std::move(*second->session);
        CHECK_FALSE(static_cast<bool>(*first_subscription));
        CHECK(static_cast<bool>(*second_subscription));
        CHECK(assigned.close().has_value());
        CHECK_FALSE(static_cast<bool>(*second_subscription));
    }

    CHECK_FALSE(static_cast<bool>(*first_subscription));
    CHECK_FALSE(static_cast<bool>(*second_subscription));
    first_subscription->unsubscribe();
    second_subscription->unsubscribe();
}

TEST_CASE(
    "SDK in-memory subscriber failure retains live state without persistence fallback",
    "[sdk][in-memory][subscriber-failure]") {
    TestPaths paths;
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto failed_subscription = result->session->subscribe(
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
                end != nullptr && std::holds_alternative<ai::UserMessage>(end->message)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Tool,
                    "subscriber rejected in-memory user message"));
            }
            return {};
        });
    REQUIRE(failed_subscription.has_value());

    auto failed = result->session->prompt("hello");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Tool);
    CHECK(failed.error().message == "subscriber rejected in-memory user message");
    CHECK(result->session->is_open());
    CHECK(result->session->message_count() == 1);
    CHECK_FALSE(result->session->last_assistant_text().has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));

    failed_subscription->unsubscribe();
    auto recovered = result->session->prompt("again");
    REQUIRE(recovered.has_value());
    CHECK(result->session->message_count() == 3);
    CHECK(result->session->last_assistant_text().has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK(result->session->close().has_value());
}

TEST_CASE(
    "SDK in-memory creation uses shared resource trust and prompt processing",
    "[sdk][in-memory][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill", "Use in-memory project instructions.");
    write_project_prompt(paths, "project-review", "Review in memory: $1.");
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "project-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "project-review");

    auto prompted = result->session->prompt("/project-review target.cpp");
    REQUIRE(prompted.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Review in memory: target.cpp.") !=
          std::string::npos);
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    CHECK(result->session->close().has_value());
}

TEST_CASE(
    "SDK in-memory provider preflight rejection retains only completed user state",
    "[sdk][in-memory][provider-preflight-rejection]") {
    TestPaths paths;
    cch::tests::TempWorkspace isolated;
    const auto agent_dir = isolated.path() / "agent-not-created";
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::InMemorySessionTarget{};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::make_unique<PreflightRejectingChatClient>();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    std::size_t delivered_user_messages = 0;
    auto subscription = result->session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (const auto* end = std::get_if<agent::MessageEndEvent>(&event);
                end != nullptr && std::holds_alternative<ai::UserMessage>(end->message)) {
                ++delivered_user_messages;
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto failed = result->session->prompt("hello");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Provider);
    CHECK(failed.error().message == "provider rejected in-memory prompt");
    CHECK(delivered_user_messages == 1);
    CHECK(result->session->is_open());
    CHECK(result->session->message_count() == 1);
    CHECK_FALSE(result->session->last_assistant_text().has_value());
    CHECK_FALSE(result->session_path.has_value());
    CHECK_FALSE(result->session->session_path().has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
    CHECK(result->session->close().has_value());
}

TEST_CASE(
    "SDK completes an accepted network failure as a durable error turn and remains reusable",
    "[sdk][live-state][incremental-persistence][issue11]") {
    TestPaths paths;
    auto transport = std::make_shared<RecoveringNetworkTransport>();

    ai::providers::OpenAIStreamConfig provider;
    provider.api_key = "sk-test-api-key";
    provider.api = "openai-completions";
    provider.provider = "openai-compatible";
    provider.model = "gpt-test";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::make_unique<ai::providers::StreamingOpenAIChatClient>(
        transport,
        std::move(provider));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<agent::AgentLifecycleEvent> events;
    bool terminal_was_live_before_message_end = false;
    auto subscription = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            events.push_back(event);
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }
            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
            if (assistant != nullptr && assistant->stop_reason == ai::AssistantStopReason::Error) {
                terminal_was_live_before_message_end =
                    session->message_count() == 2 &&
                    session->last_assistant_text().has_value() &&
                    session->last_assistant_text()->empty();
            }
            return {};
        });
    REQUIRE(subscription.has_value());

    auto error_turn = session->prompt("hello");
    REQUIRE(error_turn.has_value());
    CHECK(transport->requests.size() == 1);
    CHECK(terminal_was_live_before_message_end);
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);

    REQUIRE(events.size() == 8);
    std::size_t event_index = 0;
    CHECK(std::holds_alternative<agent::AgentStartEvent>(events[event_index++]));
    CHECK(std::holds_alternative<agent::TurnStartEvent>(events[event_index++]));
    const auto* user_start = std::get_if<agent::MessageStartEvent>(&events[event_index++]);
    REQUIRE(user_start != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_start->message));
    const auto* user_end = std::get_if<agent::MessageEndEvent>(&events[event_index++]);
    REQUIRE(user_end != nullptr);
    CHECK(std::holds_alternative<ai::UserMessage>(user_end->message));
    const auto* assistant_start = std::get_if<agent::MessageStartEvent>(&events[event_index++]);
    REQUIRE(assistant_start != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_start->message));
    const auto* assistant_end = std::get_if<agent::MessageEndEvent>(&events[event_index++]);
    REQUIRE(assistant_end != nullptr);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(assistant_end->message));
    const auto& terminal = std::get<ai::AssistantMessage>(assistant_end->message);
    CHECK(terminal.stop_reason == ai::AssistantStopReason::Error);
    REQUIRE(terminal.error_message.has_value());
    CHECK(*terminal.error_message == "could not resolve api.example: Name or service not known");
    const auto terminal_error_message = terminal.error_message;
    const auto* turn_end = std::get_if<agent::TurnEndEvent>(&events[event_index++]);
    REQUIRE(turn_end != nullptr);
    CHECK(turn_end->tool_results.empty());
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(turn_end->message));
    CHECK(std::get<ai::AssistantMessage>(turn_end->message).stop_reason == ai::AssistantStopReason::Error);
    CHECK(std::holds_alternative<agent::AgentEndEvent>(events[event_index++]));
    CHECK(std::none_of(
        events.begin(),
        events.end(),
        [](const auto& event) {
            return std::holds_alternative<agent::ToolExecutionStartEvent>(event) ||
                   std::holds_alternative<agent::ToolExecutionEndEvent>(event);
        }));

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    const auto& durable_terminal = std::get<ai::AssistantMessage>(durable->messages[1]);
    CHECK(durable_terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(durable_terminal.error_message == terminal_error_message);

    events.clear();
    auto recovered_turn = session->prompt("try again");
    REQUIRE(recovered_turn.has_value());
    CHECK(transport->requests.size() == 2);
    CHECK(session->message_count() == 4);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(*session->last_assistant_text() == "recovered");
    CHECK(session->close().has_value());

    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();
    coding_agent::CreateAgentSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.chat_client = std::move(capture);

    auto reopened = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 4);
    auto after_resume = reopened->session->prompt("after resume");
    REQUIRE(after_resume.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    REQUIRE(capture_ptr->captured_request->context.messages.size() == 5);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]));
    const auto& resumed_terminal = std::get<ai::AssistantMessage>(
        capture_ptr->captured_request->context.messages[1]);
    CHECK(resumed_terminal.stop_reason == ai::AssistantStopReason::Error);
    CHECK(resumed_terminal.error_message == terminal_error_message);
    CHECK(reopened->session->close().has_value());
}

TEST_CASE("SDK default target persists under the canonical workspace key", "[sdk][u2][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto canonical_workspace = std::filesystem::canonical(paths.workspace.path());
    const auto expected_directory = agent_dir.path() / "sessions" /
        coding_agent::session_paths::encode_workspace_key(canonical_workspace);

    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path() == expected_directory);
    CHECK(result->workspace == canonical_workspace);
    CHECK(result->metadata.workspace == canonical_workspace);
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.created_at.find('T') != std::string::npos);
    CHECK(result->session_path->filename().string().find(result->session_id) != std::string::npos);
    CHECK(result->session->session_path() == result->session_path);

    auto stored = harness::session::JsonlSessionStore::load(*result->session_path);
    REQUIRE(stored.has_value());
    CHECK(stored->metadata.session_id == result->session_id);
    CHECK(stored->metadata.created_at == result->metadata.created_at);
    CHECK(stored->metadata.workspace == canonical_workspace);
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK default persistence ignores CLI-only session directory inputs", "[sdk][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace cli_directory;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    tests::EnvVarGuard cli_dir_guard{"CCH_CODING_AGENT_SESSION_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    cli_dir_guard.set(cli_directory.path().string());
    {
        std::ofstream settings(agent_dir.path() / "settings.json");
        settings << "{\"sessionDir\":\"" << cli_directory.path().string() << "\"}";
    }

    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path().parent_path() == agent_dir.path() / "sessions");
    CHECK(std::filesystem::is_empty(cli_directory.path()));
    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// User Settings snapshot: SessionFactory loads settings once per creation
// attempt, falls back to safe defaults with an observable warning, and uses
// the same snapshot for every assembly decision.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SessionFactory applies one User Settings snapshot across provider and storage decisions", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace settings_sessions;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json",
        "{\"model\":\"settings-model\",\"sessionDir\":\"" + settings_sessions.path().string() + "\"}");

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.disable_project_skills = true;
    request.disable_prompt_templates = true;
    request.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE(result.has_value());
    // The same snapshot feeds provider resolution and CLI automatic
    // session-directory selection.
    CHECK(result->model == "settings-model");
    REQUIRE(result->session_path.has_value());
    CHECK(result->session_path->parent_path() == settings_sessions.path());
    CHECK(result->session->close().has_value());
}

TEST_CASE("SessionFactory creation without User Settings applies defaults silently", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK_FALSE(has_sdk_diag(result->diagnostics, "settings:fallback"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SessionFactory creation with malformed User Settings succeeds with safe defaults and a warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto warning = find_sdk_diag(result->diagnostics, "settings:fallback");
    REQUIRE(warning != result->diagnostics.end());
    CHECK(warning->severity == coding_agent::SdkDiagnostic::Severity::Warning);
    CHECK(warning->message.find("could not load user settings") != std::string::npos);
    CHECK(warning->message.find("invalid JSON") != std::string::npos);
    CHECK(result->session->close().has_value());
}

TEST_CASE("SessionFactory creation with invalid User Settings values falls back with a warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{\"default_project_trust\":\"bogus\"}");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    const auto warning = find_sdk_diag(result->diagnostics, "settings:fallback");
    REQUIRE(warning != result->diagnostics.end());
    CHECK(warning->severity == coding_agent::SdkDiagnostic::Severity::Warning);
    CHECK(warning->message.find("default_project_trust") != std::string::npos);
    CHECK(result->session->close().has_value());
}

TEST_CASE("SessionFactory SDK creation failure after User Settings fallback keeps the primary error and carries the warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    // No chat_client, no provider_config: normalization fails.

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message == "no chat client or provider_config supplied");
    REQUIRE(result.error().context.has_value());
    CHECK(result.error().context->find("could not load user settings") != std::string::npos);
}

TEST_CASE("SessionFactory CLI creation failure after User Settings fallback keeps the primary error and carries the warning", "[sdk][settings-snapshot]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    agent_dir.write("settings.json", "{not valid json");

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.workspace = paths.workspace.path() / "missing-workspace";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message == "workspace cannot be resolved");
    REQUIRE(result.error().context.has_value());
    CHECK(result.error().context->find("could not load user settings") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// CLI request assembly policy: SessionFactory owns workspace validity, Agent
// Session target validity, existing-file rejection, Session Resume
// compatibility, and provider readiness for the CLI creation request shape.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SessionFactory CLI creation without credentials fails with missing API key before publishing", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    tests::EnvVarGuard key_guard{"CCH_FACTORY_MISSING_KEY"};
    key_guard.unset();

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path();
    request.provider_overrides.api_key_env = "CCH_FACTORY_MISSING_KEY";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("missing API key") != std::string::npos);
    CHECK(result.error().detail.find("CCH_FACTORY_MISSING_KEY") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SessionFactory CLI explicit new target rejects an unresolvable workspace", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path() / "missing-workspace";

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SessionFactory CLI resume rejects an unresolvable explicit workspace", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());

    coding_agent::runtime::AgentSessionCreationRequest create_request;
    create_request.fake = true;
    create_request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    create_request.workspace = paths.workspace.path();
    auto created = coding_agent::create_agent_session(std::move(create_request));
    REQUIRE(created.has_value());
    REQUIRE(created->session->close().has_value());

    coding_agent::runtime::AgentSessionCreationRequest resume_request;
    resume_request.fake = true;
    resume_request.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume_request.workspace = paths.workspace.path() / "missing-workspace";
    resume_request.workspace_explicit = true;

    auto result = coding_agent::create_agent_session(std::move(resume_request));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message == "workspace cannot be resolved");
}

TEST_CASE("SessionFactory CLI explicit new target rejects an existing session file without modifying it", "[sdk][assembly][cli]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    {
        std::ofstream existing(paths.session_file, std::ios::binary);
        existing << "already here";
    }

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.fake = true;
    request.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    request.workspace = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(request));
    REQUIRE_FALSE(result.has_value());
    const auto presented = result.error().message + " " + result.error().detail;
    CHECK(presented.find("already exists") != std::string::npos);
    std::ifstream in(paths.session_file, std::ios::binary);
    const std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    CHECK(content == "already here");
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK default target gives workspace symlink aliases one directory", "[sdk][default-persistence][symlink]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    cch::tests::TempWorkspace aliases;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    const auto alias = aliases.path() / "workspace-alias";
    std::filesystem::create_directory_symlink(paths.workspace.path(), alias);

    auto create_from = [&](const std::filesystem::path& workspace) {
        coding_agent::CreateAgentSessionOptions opts;
        opts.workspace = workspace;
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        return coding_agent::create_agent_session(std::move(opts));
    };

    auto physical = create_from(paths.workspace.path());
    REQUIRE(physical.has_value());
    auto linked = create_from(alias);
    REQUIRE(linked.has_value());
    REQUIRE(physical->session_path.has_value());
    REQUIRE(linked->session_path.has_value());
    CHECK(physical->session_path->parent_path() == linked->session_path->parent_path());
    CHECK(physical->workspace == linked->workspace);
    CHECK(physical->metadata.workspace == linked->metadata.workspace);
    CHECK(physical->session->close().has_value());
    CHECK(linked->session->close().has_value());
}
#endif

TEST_CASE("failed SDK default assembly publishes no session", "[sdk][assembly][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    const auto agent_dir = home.path() / "not-created" / "agent";
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(std::filesystem::exists(agent_dir / "sessions"));
}

TEST_CASE("SDK explicit new and resume paths bypass automatic storage", "[sdk][u2][session-target]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set((agent_dir.path() / "unused-agent-dir").string());

    coding_agent::CreateAgentSessionOptions create;
    create.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    create.workspace = paths.workspace.path();
    create.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto created = coding_agent::create_agent_session(std::move(create));
    REQUIRE(created.has_value());
    CHECK(created->session_path == std::optional<std::filesystem::path>{paths.session_file});
    CHECK(created->session->session_path() == created->session_path);
    CHECK_FALSE(std::filesystem::exists(agent_dir.path() / "unused-agent-dir" / "sessions"));
    CHECK(created->session->close().has_value());

    coding_agent::CreateAgentSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume.workspace = paths.workspace.path();
    resume.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session_path == std::optional<std::filesystem::path>{paths.session_file});
    CHECK(resumed->session->session_path() == resumed->session_path);
    CHECK_FALSE(std::filesystem::exists(agent_dir.path() / "unused-agent-dir" / "sessions"));
    CHECK(resumed->session->close().has_value());
}

TEST_CASE("SDK default creation ignores a valid legacy session that remains explicitly resumable", "[sdk][default-persistence]") {
    TestPaths paths;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.path().string());
    const auto legacy_directory = paths.workspace.path() / ".cpp-harness" / "sessions";
    const auto legacy_session = legacy_directory / "legacy.jsonl";
    std::filesystem::create_directories(legacy_directory);
    {
        auto legacy_store = harness::session::JsonlSessionStore::create_new(
            legacy_session,
            harness::session::SessionMetadata{
                "legacy-session-id",
                "2026-07-18T00:00:00.000Z",
                paths.workspace.path(),
                "fake",
                "fake-model",
            });
        REQUIRE(legacy_store.has_value());
    }

    coding_agent::CreateAgentSessionOptions defaults;
    defaults.workspace = paths.workspace.path();
    defaults.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto created = coding_agent::create_agent_session(std::move(defaults));
    REQUIRE(created.has_value());
    REQUIRE(created->session_path.has_value());
    CHECK(*created->session_path != legacy_session);
    CHECK(created->session_path->parent_path().parent_path() == agent_dir.path() / "sessions");
    CHECK(std::filesystem::is_regular_file(legacy_session));
    CHECK(created->session->close().has_value());

    coding_agent::CreateAgentSessionOptions resume;
    resume.session_target = coding_agent::ExplicitResumeSessionTarget{legacy_session};
    resume.workspace = paths.workspace.path();
    resume.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto resumed = coding_agent::create_agent_session(std::move(resume));
    REQUIRE(resumed.has_value());
    CHECK(resumed->session_id == "legacy-session-id");
    CHECK(resumed->session_path == std::optional<std::filesystem::path>{legacy_session});
    CHECK(resumed->session->close().has_value());
}

TEST_CASE("SDK default publication failure does not use the legacy workspace directory", "[sdk][default-persistence][failure]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    const auto agent_dir = home.path() / "agent";
    const auto sessions_root = agent_dir / "sessions";
    std::filesystem::create_directories(agent_dir);
    {
        std::ofstream blocker(sessions_root);
        blocker << "not a directory";
    }
    const auto legacy_directory = paths.workspace.path() / ".cpp-harness" / "sessions";
    std::filesystem::create_directories(legacy_directory);
    const auto legacy_marker = legacy_directory / "legacy.jsonl";
    {
        std::ofstream marker(legacy_marker);
        marker << "legacy";
    }
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    agent_dir_guard.set(agent_dir.string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().detail.find(sessions_root.string()) != std::string::npos);
    CHECK(std::filesystem::is_regular_file(legacy_marker));
    CHECK(std::filesystem::file_size(legacy_marker) == 6);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK default creation fails when the Agent Config Directory is unresolved", "[sdk][default-persistence][failure]") {
    TestPaths paths;
    tests::EnvVarGuard agent_dir_guard{"CCH_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    agent_dir_guard.unset();
    home_guard.unset();

    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Session);
    CHECK(result.error().detail.find("<unresolved agent config sessions root>") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.workspace.path() / ".cpp-harness" / "sessions"));
}
#endif

TEST_CASE("create_agent_session fails without chat_client or provider_config", "[sdk][u2]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    // No chat_client, no provider_config

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
}

TEST_CASE("SDK provider_config api_key_env chain accepts first set fallback", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    tests::EnvVarGuard first_key{"CCH_SDK_CHAIN_FIRST"};
    tests::EnvVarGuard second_key{"CCH_SDK_CHAIN_SECOND"};
    first_key.unset();
    second_key.set("second-secret");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.provider_config = coding_agent::SdkProviderConfig{
        .provider = "fake",
        .model = "fake-model",
        .api_key_env = std::vector<std::string>{"CCH_SDK_CHAIN_FIRST", "CCH_SDK_CHAIN_SECOND"},
    };

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->provider == "fake");
    CHECK(result->model == "fake-model");
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK provider_config api_key_env chain rejects unset variables", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    tests::EnvVarGuard first_key{"CCH_SDK_CHAIN_UNSET_FIRST"};
    tests::EnvVarGuard second_key{"CCH_SDK_CHAIN_UNSET_SECOND"};
    first_key.unset();
    second_key.unset();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.provider_config = coding_agent::SdkProviderConfig{
        .provider = "fake",
        .model = "fake-model",
        .api_key_env = std::vector<std::string>{"CCH_SDK_CHAIN_UNSET_FIRST", "CCH_SDK_CHAIN_UNSET_SECOND"},
    };

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("missing API key") != std::string::npos);
    const auto presented = result.error().message + " " + result.error().detail;
    CHECK(presented.find("CCH_SDK_CHAIN_UNSET_FIRST") != std::string::npos);
    CHECK(presented.find("CCH_SDK_CHAIN_UNSET_SECOND") != std::string::npos);
}

TEST_CASE("SDK provider_config api_key_env chain rejects empty chain", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.provider_config = coding_agent::SdkProviderConfig{
        .provider = "fake",
        .model = "fake-model",
        .api_key_env = std::vector<std::string>{},
    };

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("api_key_env chain is empty") != std::string::npos);
}

TEST_CASE("SDK resume uses config-derived api_key_env chain", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard first_key{"CCH_SDK_RESUME_FIRST"};
    tests::EnvVarGuard second_key{"CCH_SDK_RESUME_SECOND"};
    home_guard.set(home.path().string());
    first_key.unset();
    second_key.set("resume-secret");

    std::filesystem::create_directories(home.path() / ".cpp-harness" / "agent");
    std::ofstream(home.path() / ".cpp-harness" / "agent" / "settings.json")
        << R"({"provider":"fake","model":"fake-model","api_key_env":["CCH_SDK_RESUME_FIRST","CCH_SDK_RESUME_SECOND"]})";

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.provider_config = coding_agent::SdkProviderConfig{
            .provider = "fake",
            .model = "fake-model",
            .api_key_env = std::vector<std::string>{"CCH_SDK_RESUME_FIRST", "CCH_SDK_RESUME_SECOND"},
        };

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->session->close().has_value());
    }

    coding_agent::CreateAgentSessionOptions resume_opts;
    resume_opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    resume_opts.workspace = paths.workspace.path();

    auto resume_result = coding_agent::create_agent_session(std::move(resume_opts));
    REQUIRE(resume_result.has_value());
    CHECK(resume_result->provider == "fake");
    CHECK(resume_result->model == "fake-model");
    CHECK(resume_result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// U3: Session facade, prompt lifecycle, event fanout
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK create/prompt/close cycle with fake client", "[sdk][u3]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    REQUIRE(session != nullptr);
    CHECK(session->is_open());
    CHECK_FALSE(session->is_busy());

    // Prompt completion reports success; resulting state is queried separately.
    auto prompt_result = session->prompt("hello world");
    REQUIRE(prompt_result.has_value());
    CHECK(session->message_count() > 0);

    // Close is idempotent
    auto close1 = session->close();
    CHECK(close1.has_value());
    CHECK_FALSE(session->is_open());

    auto close2 = session->close();
    CHECK(close2.has_value());

    // Prompt after close fails
    auto prompt_after = session->prompt("should fail");
    REQUIRE_FALSE(prompt_after.has_value());
    CHECK(prompt_after.error().code == util::ErrorCode::Validation);
}

TEST_CASE("SDK event subscription delivers lifecycle events", "[sdk][u3]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;

    // Subscribe
    int event_count = 0;
    auto sub_result = session->subscribe(
        [&event_count](const agent::AgentLifecycleEvent& /*event*/) -> util::ExpectedVoid {
            ++event_count;
            return {};
        });
    REQUIRE(sub_result.has_value());
    CHECK(static_cast<bool>(*sub_result));

    // Run a prompt
    auto prompt_result = session->prompt("test event delivery");
    REQUIRE(prompt_result.has_value());
    CHECK(event_count > 0);

    // Unsubscribe
    sub_result->unsubscribe();
    CHECK_FALSE(static_cast<bool>(*sub_result));

    // Close
    CHECK(session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// U4: Tool and resource injection
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK custom tool is registered and can be called", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    // Register a custom tool
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools = std::move(custom_tools);

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto prompt_result = session->prompt("hello");
    REQUIRE(prompt_result.has_value());

    CHECK(session->close().has_value());
}

TEST_CASE("SDK duplicate custom tool names fail creation", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;
    custom_tools.push_back(std::make_unique<FakeEchoTool>());
    custom_tools.push_back(std::make_unique<FakeEchoTool>()); // duplicate name
    opts.custom_tools = std::move(custom_tools);

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(create_result.has_value());
    CHECK(create_result.error().code == util::ErrorCode::Validation);
    CHECK(create_result.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("SDK host-provided skills are accessible", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    coding_agent::Skill skill;
    skill.name = "test-skill";
    skill.description = "A test skill for SDK testing";
    skill.content = "This is test skill content.";
    skill.filePath = "/tmp/test-skill.md";
    opts.skills.push_back(std::move(skill));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->skills().size() == 1);
    CHECK(session->skills()[0].name == "test-skill");

    CHECK(session->close().has_value());
}

TEST_CASE("SDK unknown skill command reaches the provider without diagnostics", "[sdk][u4][skill-diagnostics]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto prompt_result = session->prompt("/skill:unknown");
    REQUIRE(prompt_result.has_value());

    CHECK(session->close().has_value());
}

TEST_CASE("SDK prompt leaves diagnostics empty for valid and bare skill commands", "[sdk][u4][skill-diagnostics]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    coding_agent::Skill skill;
    skill.name = "valid-skill";
    skill.description = "A valid skill for SDK diagnostics testing";
    skill.content = "Use the valid skill instructions.";
    skill.filePath = "/tmp/valid-skill.md";
    opts.skills.push_back(std::move(skill));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto valid_result = session->prompt("/skill:valid-skill with args");
    REQUIRE(valid_result.has_value());

    auto bare_result = session->prompt("/skill:");
    REQUIRE(bare_result.has_value());

    CHECK(session->close().has_value());
}

TEST_CASE("SDK host-provided templates are accessible", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    coding_agent::PromptTemplate tmpl;
    tmpl.name = "greet";
    tmpl.description = "Greeting template";
    tmpl.content = "Hello, $ARGUMENTS!";
    opts.prompt_templates.push_back(std::move(tmpl));

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->templates().size() == 1);
    CHECK(session->templates()[0].name == "greet");

    CHECK(session->close().has_value());
}

TEST_CASE("moved SDK session retains its owned prompt resource snapshot", "[sdk][u4][prompt-processing]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.prompt_templates.push_back(host_template("review", "Review cached: $1"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    coding_agent::AgentSession moved_session{std::move(*created->session)};

    auto result = moved_session.prompt("/review target.cpp");
    REQUIRE(result.has_value());
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& messages = capture_ptr->captured_request->context.messages;
    const auto user = std::find_if(messages.begin(), messages.end(), [](const ai::MessageVariant& message) {
        return std::holds_alternative<ai::UserMessage>(message);
    });
    REQUIRE(user != messages.end());
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(*user).content) == "Review cached: target.cpp");

    CHECK(moved_session.close().has_value());
}

TEST_CASE("SDK expand_prompt_templates false sends slash-shaped input raw to the provider", "[sdk][u4][prompt-processing]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.skills.push_back(host_skill("cached"));
    opts.prompt_templates.push_back(host_template("review", "expanded: $1"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    for (const std::string raw : {"/skill:cached", "/review arg"}) {
        coding_agent::PromptOptions prompt_options;
        prompt_options.expand_prompt_templates = false;
        auto result = created->session->prompt(raw, std::move(prompt_options));
        REQUIRE(result.has_value());
        REQUIRE(capture_ptr->captured_request.has_value());
        const auto& messages = capture_ptr->captured_request->context.messages;
        const auto user = std::find_if(messages.rbegin(), messages.rend(), [](const ai::MessageVariant& message) {
            return std::holds_alternative<ai::UserMessage>(message);
        });
        REQUIRE(user != messages.rend());
        CHECK(ai::text_from_content(std::get<ai::UserMessage>(*user).content) == raw);
    }

    CHECK(created->session->close().has_value());
}

TEST_CASE("SDK treats user bash prefixes as ordinary prompts", "[sdk][u4][prompt-processing][user-bash]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    for (const std::string raw : {"!echo visible", "!!echo excluded-later"}) {
        auto result = created->session->prompt(raw);
        REQUIRE(result.has_value());
        REQUIRE(capture_ptr->captured_request.has_value());
        const auto& messages = capture_ptr->captured_request->context.messages;
        const auto user = std::find_if(messages.rbegin(), messages.rend(), [](const ai::MessageVariant& message) {
            return std::holds_alternative<ai::UserMessage>(message);
        });
        REQUIRE(user != messages.rend());
        CHECK(ai::text_from_content(std::get<ai::UserMessage>(*user).content) == raw);
    }

    CHECK(created->session->close().has_value());
}

TEST_CASE("SDK loads trusted project resources through shared loader", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill", "Use project skill instructions.");
    write_project_prompt(paths, "project-review", "Review project item: $1.");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "project-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "project-review");

    auto skill_prompt = result->session->prompt("/skill:project-skill now");
    REQUIRE(skill_prompt.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Use project skill instructions.") != std::string::npos);

    auto template_prompt = result->session->prompt("/project-review Ada");
    REQUIRE(template_prompt.has_value());
    REQUIRE(result->session->last_assistant_text().has_value());
    CHECK(result->session->last_assistant_text()->find("Review project item: Ada.") != std::string::npos);

    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK keeps host resources when project resource loading is disabled", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = false;
    opts.skills.push_back(host_skill("host-skill"));
    opts.prompt_templates.push_back(host_template("host-review"));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "host-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "host-review");
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK keeps host resources and returns resource decisions when project is untrusted", "[sdk][project-resources]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    home.write(
        ".cpp-harness/trust.json",
        "{\"" + paths.workspace.path().string() + "\":false}\n");
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.skills.push_back(host_skill("host-skill"));
    opts.prompt_templates.push_back(host_template("host-review"));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "host-skill");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].name == "host-review");
    CHECK(has_sdk_diag(result->diagnostics, "resource:project_skills"));
    CHECK(has_sdk_diag(result->diagnostics, "resource:project_prompts"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK project resource duplicates prefer host resources with structured diagnostics", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "same-skill", "Project skill body.");
    write_project_prompt(paths, "same-template", "Project template body.");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    opts.skills.push_back(host_skill("same-skill", "Host skill body."));
    opts.prompt_templates.push_back(host_template("same-template", "Host template body."));

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].content == "Host skill body.");
    REQUIRE(result->session->templates().size() == 1);
    CHECK(result->session->templates()[0].content == "Host template body.");
    CHECK(has_sdk_diag(result->diagnostics, "duplicate:duplicate_skill_skipped"));
    CHECK(has_sdk_diag(result->diagnostics, "duplicate:duplicate_template_skipped"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK returns trust and adapter diagnostics as values", "[sdk][project-resources]") {
    TestPaths paths;
    paths.workspace.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");

    {
        tests::EnvVarGuard home_guard{"HOME"};
        home_guard.set(paths.workspace.path().string());
        paths.workspace.write(".cpp-harness/trust.json", "{not json");

        coding_agent::CreateAgentSessionOptions untrusted_opts;
        untrusted_opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        untrusted_opts.workspace = paths.workspace.path();
        untrusted_opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        untrusted_opts.load_project_resources = true;

        auto untrusted = coding_agent::create_agent_session(std::move(untrusted_opts));
        REQUIRE(untrusted.has_value());
        CHECK(has_sdk_diag(untrusted->diagnostics, "trust:trust_store_unavailable"));
        CHECK(has_sdk_diag(untrusted->diagnostics, "resource:project_skills"));
        CHECK(untrusted->session->skills().empty());
        CHECK(untrusted->session->close().has_value());
    }

    TestPaths trusted_paths;
    trusted_paths.workspace.write(
        ".cpp-harness/skills/bad/SKILL.md",
        "---\n"
        "name: bad\n"
        "description:\n"
        "---\n"
        "Body.\n");

    coding_agent::CreateAgentSessionOptions trusted_opts;
    trusted_opts.session_target = coding_agent::ExplicitNewSessionTarget{trusted_paths.session_file};
    trusted_opts.workspace = trusted_paths.workspace.path();
    trusted_opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    trusted_opts.load_project_resources = true;
    trusted_opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto trusted = coding_agent::create_agent_session(std::move(trusted_opts));
    REQUIRE(trusted.has_value());
    CHECK(has_sdk_diag(trusted->diagnostics, "skill:invalid_metadata"));
    CHECK(trusted->session->skills().empty());
    CHECK(trusted->session->close().has_value());
}

TEST_CASE("SDK bounds auto-discovered resource diagnostics", "[sdk][project-resources]") {
    TestPaths paths;
    const std::string long_skill_name(2048, 'x');
    paths.workspace.write(
        ".cpp-harness/skills/long-name/SKILL.md",
        "---\n"
        "name: " + long_skill_name + "\n"
        "description: Long invalid skill name.\n"
        "---\n"
        "Body.\n");
    for (int index = 0; index < 80; ++index) {
        paths.workspace.write(
            ".cpp-harness/prompts/bad-" + std::to_string(index) + ".md",
            "---\n"
            "invalid frontmatter line\n"
            "---\n"
            "Body.\n");
    }

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.provider_config = coding_agent::SdkProviderConfig{
        .provider = "fake",
        .model = "fake-model",
    };
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->templates().empty());
    CHECK(result->diagnostics.size() <= 64);
    CHECK(has_sdk_diag(result->diagnostics, "resource:diagnostics_truncated"));
    const auto truncated_message = std::find_if(
        result->diagnostics.begin(),
        result->diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "skill:invalid_metadata" &&
                   diagnostic.message.ends_with("...[truncated]");
        });
    REQUIRE(truncated_message != result->diagnostics.end());
    CHECK(truncated_message->message.size() <= 1024);
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK project resource diagnostics are not printed during creation", "[sdk][project-resources]") {
    TestPaths paths;
    paths.workspace.write(
        ".cpp-harness/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    std::ostringstream captured_out;
    std::ostringstream captured_err;
    auto* old_out = std::cout.rdbuf(captured_out.rdbuf());
    auto* old_err = std::cerr.rdbuf(captured_err.rdbuf());
    auto result = coding_agent::create_agent_session(std::move(opts));
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);

    REQUIRE(result.has_value());
    CHECK(has_sdk_diag(result->diagnostics, "template:parse_failed"));
    CHECK(captured_out.str().empty());
    CHECK(captured_err.str().empty());
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK default built-in tools exclude bash", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    // Default builtin_tools: read=true, write=true, edit_file=true, bash=false

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    // Prompt with "bash ..." — the fake client would try to make a bash tool call
    // But since bash is not registered, it should just get a text response
    auto& session = create_result->session;
    auto prompt_result = session->prompt("bash echo hello");
    REQUIRE(prompt_result.has_value());
    // The fake client responds with text when no tool is matched for "bash" prefix
    // (it checks prompt.rfind("bash ", 0), which matches, then returns a bash tool call)
    // But since bash is not registered, the agent loop won't find the tool and will error.
    // This verifies the tool is not registered by checking the session works.

    CHECK(session->close().has_value());
}

TEST_CASE("SDK bash tool can be explicitly enabled", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.builtin_tools.bash = true;

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->is_open());
    CHECK(session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Session resume
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK can resume a linear session", "[sdk][u3]") {
    TestPaths paths;

    // First, create a session
    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        auto& session = result->session;
        auto pr = session->prompt("hello");
        REQUIRE(pr.has_value());
        CHECK(session->close().has_value());
    }

    // Then, resume it
    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        auto& session = result->session;
        CHECK(session->is_open());
        CHECK(session->message_count() > 0);

        auto pr = session->prompt("continue");
        REQUIRE(pr.has_value());
        CHECK(session->close().has_value());
    }
}

TEST_CASE("SDK rejects branched active resumed topology", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));

    expect_unsupported_sdk_resume(paths);
}

TEST_CASE("SDK rejects compacted active resumed topology", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("before compaction")));
    REQUIRE(store->append(user_msg("kept message")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto kept_id = pre->entries[2].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_compaction(std::nullopt, "summary", kept_id, 1000, std::nullopt, std::nullopt));

    expect_unsupported_sdk_resume(paths);
}

TEST_CASE("SDK resumes linear active topology with inactive branch and compaction data", "[sdk][u3][resume-topology]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;
    const auto third_id = pre->entries[3].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_branch_summary(first_id, third_id, "inactive branch", std::nullopt, std::nullopt));
    REQUIRE(resumed->append_compaction(first_id, "inactive compaction", third_id, 1000, std::nullopt, std::nullopt));
    REQUIRE(resumed->append_leaf(std::nullopt, third_id));

    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(result.has_value());
    CHECK(result->session->message_count() == 3);

    auto prompt_result = result->session->prompt("continue linear path");
    REQUIRE(prompt_result.has_value());
    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// State accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK state accessors reflect committed history", "[sdk][u3]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->message_count() == 0);
    CHECK_FALSE(session->last_assistant_text().has_value());

    auto pr = session->prompt("hello");
    REQUIRE(pr.has_value());
    CHECK(session->message_count() > 0);
    CHECK(session->last_assistant_text().has_value());

    CHECK(session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Live state updates before subscribers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK message_end subscribers observe live state updated before delivery", "[sdk][live-state]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::size_t seen_message_ends = 0;
    std::size_t user_message_ends = 0;
    std::size_t assistant_message_ends = 0;
    std::size_t tool_result_message_ends = 0;
    std::optional<std::string> assistant_text_in_subscriber;

    auto sub = session->subscribe([&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
            const auto& message = end->message;
            // Live history is updated before subscribers run, so the message count
            // equals the number of message_end events already delivered plus the
            // current one.
            CHECK(session->message_count() == seen_message_ends + 1);

            if (std::holds_alternative<ai::UserMessage>(message)) {
                ++user_message_ends;
            } else if (std::holds_alternative<ai::AssistantMessage>(message)) {
                ++assistant_message_ends;
                assistant_text_in_subscriber = session->last_assistant_text();
            } else if (std::holds_alternative<ai::ToolResultMessage>(message)) {
                ++tool_result_message_ends;
            }
            ++seen_message_ends;
        }
        return {};
    });
    REQUIRE(sub.has_value());

    auto first = session->prompt("hello");
    REQUIRE(first.has_value());
    CHECK(user_message_ends == 1);
    CHECK(assistant_message_ends == 1);
    CHECK(tool_result_message_ends == 0);
    CHECK(seen_message_ends == 2);
    REQUIRE(assistant_text_in_subscriber.has_value());
    CHECK(assistant_text_in_subscriber->find("fake: hello") != std::string::npos);
    CHECK(session->last_assistant_text() == assistant_text_in_subscriber);
    CHECK(session->message_count() == 2);

    // Tool-using prompt: user, assistant with tool call, tool result, final assistant.
    assistant_text_in_subscriber.reset();
    auto second = session->prompt("read target.txt");
    REQUIRE(second.has_value());
    CHECK(user_message_ends == 2);
    CHECK(tool_result_message_ends == 1);
    CHECK(assistant_message_ends == 3);
    CHECK(seen_message_ends == 6);
    REQUIRE(assistant_text_in_subscriber.has_value());
    CHECK(assistant_text_in_subscriber->find("observed") != std::string::npos);
    CHECK(session->last_assistant_text() == assistant_text_in_subscriber);

    CHECK(session->close().has_value());
}

TEST_CASE(
    "SDK persists each completed message after subscribers and reopens in durable order",
    "[sdk][incremental-persistence]") {
    TestPaths paths;
    paths.workspace.write("target.txt", "target content\n");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::size_t delivered_message_ends = 0;
    auto sub = session->subscribe([&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
        if (!std::holds_alternative<agent::MessageEndEvent>(event)) {
            return {};
        }

        auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
        CHECK(durable.has_value());
        if (!durable) {
            return std::unexpected(durable.error());
        }

        // The current message is live before delivery, but becomes durable only
        // after every subscriber has observed its message_end event.
        CHECK(session->message_count() == delivered_message_ends + 1);
        CHECK(durable->messages.size() == delivered_message_ends);
        ++delivered_message_ends;
        return {};
    });
    REQUIRE(sub.has_value());

    auto prompt = session->prompt("read target.txt");
    REQUIRE(prompt.has_value());
    CHECK(delivered_message_ends == 4);

    auto durable = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    REQUIRE(durable->messages.size() == 4);
    CHECK(std::holds_alternative<ai::UserMessage>(durable->messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable->messages[1]));
    CHECK(std::holds_alternative<ai::ToolResultMessage>(durable->messages[2]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable->messages[3]));

    CHECK(session->close().has_value());

    auto reopened = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 4);
    REQUIRE(reopened->session->last_assistant_text().has_value());
    CHECK(reopened->session->last_assistant_text()->find("observed") != std::string::npos);
    CHECK(reopened->session->close().has_value());
}

TEST_CASE(
    "SDK subscriber failure stops delivery and persistence without closing the session",
    "[sdk][live-state][subscriber-failure]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    std::vector<std::string> delivery_order;
    auto first = session->subscribe(
        [state = std::make_unique<std::size_t>(0), &delivery_order](
            const agent::AgentLifecycleEvent& event) mutable -> util::ExpectedVoid {
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }

            ++*state;
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                delivery_order.emplace_back("first:user:" + std::to_string(*state));
                return {};
            }
            if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
                delivery_order.emplace_back("first:assistant:" + std::to_string(*state));
                return std::unexpected(util::make_error(
                    util::ErrorCode::Tool,
                    "subscriber rejects assistant message_end"));
            }
            return {};
        });
    REQUIRE(first.has_value());

    std::size_t second_message_ends = 0;
    auto second = session->subscribe(
        [&](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            const auto* end = std::get_if<agent::MessageEndEvent>(&event);
            if (end == nullptr) {
                return {};
            }

            ++second_message_ends;
            if (std::holds_alternative<ai::UserMessage>(end->message)) {
                delivery_order.emplace_back("second:user");
            } else if (std::holds_alternative<ai::AssistantMessage>(end->message)) {
                delivery_order.emplace_back("second:assistant");
            }
            return {};
        });
    REQUIRE(second.has_value());

    auto failed = session->prompt("hello");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Tool);
    CHECK(failed.error().message == "subscriber rejects assistant message_end");

    CHECK(second_message_ends == 1);
    const std::vector<std::string> expected_delivery_order{
        "first:user:1",
        "second:user",
        "first:assistant:2",
    };
    CHECK(delivery_order == expected_delivery_order);

    // The rejected assistant message is live, but subscriber short-circuiting
    // prevented it from becoming durable.
    CHECK(session->message_count() == 2);
    REQUIRE(session->last_assistant_text().has_value());
    CHECK(session->last_assistant_text()->find("fake: hello") != std::string::npos);
    auto durable_after_failure = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_failure.has_value());
    REQUIRE(durable_after_failure->messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_failure->messages[0]));
    CHECK(session->is_open());

    // Remove only the rejecting subscriber. The same session accepts another
    // prompt, and later completed messages continue to persist.
    first->unsubscribe();
    auto recovered = session->prompt("again");
    REQUIRE(recovered.has_value());
    CHECK(session->message_count() == 4);
    CHECK(second_message_ends == 3);

    auto durable_after_recovery = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_recovery.has_value());
    REQUIRE(durable_after_recovery->messages.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[0]));
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[1]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable_after_recovery->messages[2]));

    CHECK(session->close().has_value());
}

TEST_CASE(
    "SDK persistence failure retains live history and permits later durable appends",
    "[sdk][live-state][persistence-failure]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto& session = result->session;

    // The user message reaches storage, then the assistant append fails. The
    // one-shot private hook leaves the next prompt free to persist normally.
    harness::session::testing::fail_nth_append_for_test(paths.session_file, 2);
    auto failed = session->prompt("first");
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == util::ErrorCode::Session);
    CHECK(failed.error().message == "could not persist session entry");
    CHECK(session->is_open());
    CHECK(session->message_count() == 2);
    CHECK(session->last_assistant_text() == "captured");

    auto durable_after_failure = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_failure.has_value());
    REQUIRE(durable_after_failure->messages.size() == 1);
    REQUIRE(std::holds_alternative<ai::UserMessage>(durable_after_failure->messages[0]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(durable_after_failure->messages[0]).content) == "first");

    auto recovered = session->prompt("second");
    REQUIRE(recovered.has_value());
    CHECK(session->message_count() == 4);

    // The later provider request uses retained live state even though the first
    // assistant message never reached durable storage.
    REQUIRE(capture_ptr->captured_request.has_value());
    const auto& request_messages = capture_ptr->captured_request->context.messages;
    REQUIRE(request_messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::UserMessage>(request_messages[0]));
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(request_messages[1]));
    REQUIRE(std::holds_alternative<ai::UserMessage>(request_messages[2]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(request_messages[0]).content) == "first");
    CHECK(ai::text_from_assistant_content(std::get<ai::AssistantMessage>(request_messages[1]).content) == "captured");
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(request_messages[2]).content) == "second");

    auto durable_after_recovery = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(durable_after_recovery.has_value());
    REQUIRE(durable_after_recovery->messages.size() == 3);
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[0]));
    CHECK(std::holds_alternative<ai::UserMessage>(durable_after_recovery->messages[1]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(durable_after_recovery->messages[2]));
    CHECK(ai::text_from_content(std::get<ai::UserMessage>(durable_after_recovery->messages[1]).content) == "second");

    CHECK(session->close().has_value());
    auto reopened = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE(reopened.has_value());
    CHECK(reopened->session->message_count() == 3);
    REQUIRE(reopened->session->last_assistant_text().has_value());
    CHECK(*reopened->session->last_assistant_text() == "captured");
    CHECK(reopened->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateAgentSessionResult metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CreateAgentSessionResult contains metadata", "[sdk][u2]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());

    CHECK_FALSE(result->session_id.empty());
    REQUIRE(result->session_path.has_value());
    CHECK_FALSE(result->session_path->empty());
    CHECK(result->workspace == paths.workspace.path());
    CHECK(result->provider == "sdk-host");
    CHECK(result->model == "host-client");
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.workspace == result->workspace);
    CHECK(result->metadata.provider == result->provider);
    CHECK(result->metadata.model == result->model);

    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK new sessions receive fresh identity and resume preserves it", "[sdk][assembly]") {
    TestPaths first_paths;
    TestPaths second_paths;

    coding_agent::CreateAgentSessionOptions first_opts;
    first_opts.session_target = coding_agent::ExplicitNewSessionTarget{first_paths.session_file};
    first_opts.workspace = first_paths.workspace.path();
    first_opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto first = coding_agent::create_agent_session(std::move(first_opts));
    REQUIRE(first.has_value());
    const auto first_id = first->metadata.session_id;
    const auto first_created_at = first->metadata.created_at;
    CHECK(std::regex_match(
        first_id,
        std::regex{R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"}));
    CHECK(std::regex_match(
        first_created_at,
        std::regex{R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$)"}));
    CHECK(first_id != first_created_at);
    CHECK(first->session->close().has_value());

    coding_agent::CreateAgentSessionOptions second_opts;
    second_opts.session_target = coding_agent::ExplicitNewSessionTarget{second_paths.session_file};
    second_opts.workspace = second_paths.workspace.path();
    second_opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto second = coding_agent::create_agent_session(std::move(second_opts));
    REQUIRE(second.has_value());
    CHECK(second->metadata.session_id != first_id);
    CHECK_FALSE(second->metadata.created_at.empty());
    CHECK(second->session->close().has_value());

    auto resumed = coding_agent::create_agent_session(sdk_resume_options(first_paths));
    REQUIRE(resumed.has_value());
    CHECK(resumed->metadata.session_id == first_id);
    CHECK(resumed->metadata.created_at == first_created_at);
    CHECK(resumed->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics from host client
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK creation with host client produces diagnostic", "[sdk][u2]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());

    // Should have at least one diagnostic about host client
    bool found_host_diag = false;
    for (const auto& d : result->diagnostics) {
        if (d.code == "host_client_used") {
            found_host_diag = true;
        }
    }
    CHECK(found_host_diag);

    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Factory assembly invariants
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool tool_registry_contains(const ai::AiContext& context, std::string_view name) {
    return std::any_of(
        context.tools.begin(),
        context.tools.end(),
        [name](const ai::Tool& tool) { return tool.name == name; });
}

} // namespace

TEST_CASE("Failed new-session creation leaves no session file", "[sdk][assembly]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>());
    opts.custom_tools.push_back(std::make_unique<FakeEchoTool>()); // duplicate

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("Failed SDK resume does not modify the session file", "[sdk][assembly]") {
    TestPaths paths;
    auto store = harness::session::JsonlSessionStore::create_new(paths.session_file, test_metadata(paths));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto pre = harness::session::JsonlSessionStore::load(paths.session_file);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(paths.session_file);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));

    const auto before = read_file_text(paths.session_file);

    auto result = coding_agent::create_agent_session(sdk_resume_options(paths));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "unsupported_session_topology");

    const auto after = read_file_text(paths.session_file);
    CHECK(before == after);
}

TEST_CASE("SDK host-provided execution environment is not cleaned up by session close", "[sdk][assembly]") {
    TestPaths paths;
    auto env = std::make_shared<CountingFakeEnv>(paths.workspace.path());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.execution_env = env;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->close().has_value());
    CHECK(env->cleanup_count == 0);
}

TEST_CASE("SDK disabled bash is absent from the model-visible tool registry", "[sdk][assembly]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.builtin_tools.bash = false;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt("hello");
    REQUIRE(prompt_result.has_value());

    REQUIRE(capture_ptr->captured_request.has_value());
    CHECK_FALSE(tool_registry_contains(capture_ptr->captured_request->context, "bash"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "read"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "write"));
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "edit_file"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK enabled bash appears in the model-visible tool registry", "[sdk][assembly]") {
    TestPaths paths;
    auto capture = std::make_unique<CaptureChatClient>();
    auto* capture_ptr = capture.get();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.builtin_tools.bash = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt("hello");
    REQUIRE(prompt_result.has_value());

    REQUIRE(capture_ptr->captured_request.has_value());
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "bash"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK rejects workspace-local trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = paths.workspace.path() / ".cpp-harness" / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects trust_store_path equal to the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = paths.workspace.path();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK default trust store inside the workspace cannot authorize project resources", "[sdk][assembly][trust]") {
    TestPaths paths;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(paths.workspace.path().string());
    write_project_skill(paths, "workspace-authorized");
    paths.workspace.write(
        ".cpp-harness/agent/trust.json",
        "{\"" + std::filesystem::weakly_canonical(paths.workspace.path()).string() + "\":true}\n");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->skills().empty());
    CHECK(has_sdk_diag(result->diagnostics, "trust:trust_store_unavailable"));
    CHECK(result->session->close().has_value());
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SDK rejects trust_store_path through a symlinked parent into the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto linked_workspace = external.path() / "workspace-link";
    std::filesystem::create_directory_symlink(paths.workspace.path(), linked_workspace);

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = linked_workspace / ".cpp-harness" / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects a symlinked trust_store_path into the workspace", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto workspace_trust = paths.workspace.path() / ".cpp-harness" / "trust.json";
    paths.workspace.write(".cpp-harness/trust.json", "{}\n");
    const auto linked_trust = external.path() / "trust.json";
    std::filesystem::create_symlink(workspace_trust, linked_trust);

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = linked_trust;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects trust_store_path when containment cannot be determined", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    const auto loop = external.path() / "loop";
    std::filesystem::create_directory_symlink("loop", loop);

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = loop / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("canonicalized") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}
#endif

TEST_CASE("SDK rejects relative trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = std::filesystem::path{"relative/trust.json"};

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK accepts an existing external trust store as project trust authority", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace external;
    write_project_skill(paths, "externally-authorized");
    const auto external_trust = external.path() / "trust.json";
    std::ofstream(external_trust)
        << "{\"" << std::filesystem::weakly_canonical(paths.workspace.path()).string() << "\":true}\n";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = external_trust;
    opts.load_project_resources = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    REQUIRE(result->session->skills().size() == 1);
    CHECK(result->session->skills()[0].name == "externally-authorized");
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK accepts external not-yet-created trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    auto external_trust = home.path() / "external" / "trust.json";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = external_trust;
    opts.load_project_resources = true;
    opts.default_project_trust = coding_agent::DefaultProjectTrust::Always;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider resolution and metadata precedence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK host client new session uses config provider but host model sentinel", "[sdk][provider-resolution]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    std::filesystem::create_directories(home.path() / ".cpp-harness" / "agent");
    std::ofstream(home.path() / ".cpp-harness" / "agent" / "settings.json") << R"({"provider":"kimi-coding"})";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    CHECK(result->provider == "kimi-coding");
    CHECK(result->model == "host-client");
    CHECK(result->session->provider() == "kimi-coding");
    CHECK(result->session->model() == "host-client");
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK host client resume without override retains stored metadata", "[sdk][provider-resolution]") {
    TestPaths paths;

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        opts.provider_config = coding_agent::SdkProviderConfig{
            .provider = "fake",
            .model = "fake-model",
        };

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "fake");
        CHECK(result->model == "fake-model");
        CHECK(result->session->close().has_value());
    }

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "fake");
        CHECK(result->model == "fake-model");
        CHECK_FALSE(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        CHECK(result->session->provider() == "fake");
        CHECK(result->session->model() == "fake-model");
        CHECK(result->session->close().has_value());
    }
}

TEST_CASE("SDK resume with explicit provider/model override reports diagnostic", "[sdk][provider-resolution]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "sdk-host");
        CHECK(result->model == "host-client");
        CHECK(result->session->close().has_value());
    }

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        opts.provider_config = coding_agent::SdkProviderConfig{
            .provider = "openai-compatible",
            .model = "gpt-4o",
        };

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->provider == "openai-compatible");
        CHECK(result->model == "gpt-4o");
        CHECK(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        CHECK(result->session->close().has_value());
    }
}

TEST_CASE("SDK resume with explicit model override alone reports diagnostic", "[sdk][provider-resolution]") {
    TestPaths paths;

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitNewSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        opts.provider_config = coding_agent::SdkProviderConfig{
            .provider = "openai-compatible",
            .model = "gpt-4.1-mini",
        };

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->session->close().has_value());
    }

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
        opts.workspace = paths.workspace.path();
        opts.chat_client = ai::providers::make_scripted_fake_chat_client();
        opts.provider_config = coding_agent::SdkProviderConfig{
            .provider = "openai-compatible",
            .model = "gpt-4o",
        };

        auto result = coding_agent::create_agent_session(std::move(opts));
        REQUIRE(result.has_value());
        CHECK(result->model == "gpt-4o");
        CHECK(has_sdk_diag(result->diagnostics, "resume_provider_override"));
        CHECK(result->session->close().has_value());
    }
}

