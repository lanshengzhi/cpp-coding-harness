#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_cli_session(
        tests::RuntimeFixture& runtime, const tests::TempWorkspace& workspace, bool provide_user_shell) {
    runtime::AgentSessionCreationRequest request;
    request.provide_user_shell = provide_user_shell;
    request.execution_runtime_target = runtime.make_target();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    auto model_runtime = tests::detail::scripted_fake_runtime();
    if (!model_runtime) {
        return std::unexpected(model_runtime.error());
    }
    request.model_runtime = std::move(*model_runtime);
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    return runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
}

[[nodiscard]] std::vector<const ai::ToolResultMessage*> tool_results(
    const std::vector<ai::MessageVariant>& messages) {
    std::vector<const ai::ToolResultMessage*> results;
    for (const auto& message : messages) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
            results.push_back(result);
        }
    }
    return results;
}

[[nodiscard]] std::size_t bash_message_count(
    const std::vector<ai::MessageVariant>& messages) {
    std::size_t count = 0;
    for (const auto& message : messages) {
        if (std::holds_alternative<ai::BashExecutionMessage>(message)) ++count;
    }
    return count;
}

/// The Session must outlive `runtime.run`; the one-shot async factory captures
/// its reference while the prompt or User Bash settles on the fixture loop.
[[nodiscard]] support::Expected<runtime::UserBashCompletion> run_user_bash(
        tests::RuntimeFixture& runtime, coding_agent::AgentSession& session, std::string command) {
    return runtime.run(support::detail::make_async_result(
            [&session, command = std::move(command)]()
                    -> boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>> {
                co_return co_await coding_agent::detail::AgentSessionInteractiveAccess::run_user_bash(
                        session, std::move(command), false, [](const coding_agent::runtime::UserBashProgress&) {
                            return support::ExpectedVoid{};
                        });
            }));
}

/// The Session must outlive `runtime.run`; the one-shot async factory captures
/// its reference while the prompt settles on the fixture loop.
[[nodiscard]] support::ExpectedVoid run_prompt(
        tests::RuntimeFixture& runtime, coding_agent::AgentSession& session, std::string text) {
    return runtime.run(support::detail::make_async_result(
            [&session, text = std::move(text)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await session.prompt(std::move(text));
            }));
}

} // namespace

TEST_CASE(
    "Native TUI session assembly provides its independent User Shell",
    "[coding_agent][runtime][assembly][issue90]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime_fixture;
    auto created = create_cli_session(runtime_fixture, workspace, true);
    REQUIRE(created);
    auto& session = runtime_fixture.adopt_session(std::move(created->session));

    REQUIRE(coding_agent::detail::AgentSessionInteractiveAccess::has_user_shell(session));

    const auto completion = run_user_bash(runtime_fixture, session, "printf 'assembly-output\\n'");
    REQUIRE(completion);
    CHECK(completion->message.command == "printf 'assembly-output\\n'");
    CHECK(completion->message.output.find("assembly-output") != std::string::npos);
    CHECK(completion->message.exit_code == 0);
    CHECK_FALSE(completion->message.exclude_from_context);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);
}

TEST_CASE(
    "CLI assembly without the Native TUI leaves the User Shell absent",
    "[coding_agent][runtime][assembly][issue90]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime_fixture;
    auto created = create_cli_session(runtime_fixture, workspace, false);
    REQUIRE(created);
    auto& session = runtime_fixture.adopt_session(std::move(created->session));
    CHECK_FALSE(coding_agent::detail::AgentSessionInteractiveAccess::has_user_shell(session));
}

TEST_CASE(
    "the model Bash tool is always registered alongside the Session-owned User Shell",
    "[coding_agent][runtime][assembly][issue90]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime_fixture;
    auto created = create_cli_session(runtime_fixture, workspace, true);
    REQUIRE(created);
    auto& session = runtime_fixture.adopt_session(std::move(created->session));

    // The model Bash Tool is always available under the fixed tool set (the
    // --enable-bash opt-in is gone) and executes in the workspace.
    const auto prompted = run_prompt(runtime_fixture, session, "bash echo model-tool-output");
    REQUIRE(prompted);
    const auto snapshot = session.snapshot();
    const auto results = tool_results(snapshot.agent_state.messages);
    REQUIRE(results.size() == 1);
    CHECK(results.front()->tool_name == "bash");
    CHECK_FALSE(results.front()->is_error);
    CHECK(ai::text_from_content(results.front()->content).find("model-tool-output") !=
          std::string::npos);

    // User Bash keeps its independent execution: direct invocation, own
    // capability instance, and the !/!! exclusion policy.
    const auto included = run_user_bash(runtime_fixture, session, "printf 'still-direct\\n'");
    REQUIRE(included);
    CHECK(included->message.output.find("still-direct") != std::string::npos);
    CHECK_FALSE(included->message.exclude_from_context);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);
}
