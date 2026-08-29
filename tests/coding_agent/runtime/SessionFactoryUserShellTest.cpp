#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UserBashTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/io_context.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_cli_session(
    const tests::TempWorkspace& workspace,
    bool provide_user_shell) {
    runtime::AgentSessionCreationRequest request;
    request.provide_user_shell = provide_user_shell;
    request.execution_runtime_target = tests::detail::fixture_runtime_target();
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};
    auto runtime = tests::detail::scripted_fake_runtime();
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    request.model_runtime = std::move(*runtime);
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    return coding_agent::create_agent_session(std::move(request));
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

[[nodiscard]] support::Expected<runtime::UserBashCompletion> run_user_bash_blocking(
    coding_agent::AgentSession& session,
    std::string command) {
    boost::asio::io_context io;
    tests::BashResult slot;
    tests::spawn_bash(io, session, std::move(command), slot);
    io.run();
    REQUIRE(slot.has_value());
    return std::move(*slot);
}

[[nodiscard]] support::ExpectedVoid run_prompt_blocking(
    coding_agent::AgentSession& session,
    std::string text) {
    boost::asio::io_context io;
    tests::PromptResult slot;
    tests::spawn_prompt(io, session, std::move(text), slot);
    io.run();
    REQUIRE(slot.has_value());
    return *slot;
}

} // namespace

TEST_CASE(
    "Native TUI session assembly provides its independent User Shell",
    "[coding_agent][runtime][assembly][issue90]") {
    tests::TempWorkspace workspace;
    auto created = create_cli_session(workspace, true);
    REQUIRE(created);
    auto& session = *created->session;

    REQUIRE(coding_agent::detail::AgentSessionInteractiveAccess::has_user_shell(session));

    const auto completion = run_user_bash_blocking(session, "printf 'assembly-output\\n'");
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
    auto created = create_cli_session(workspace, false);
    REQUIRE(created);
    CHECK_FALSE(
        coding_agent::detail::AgentSessionInteractiveAccess::has_user_shell(*created->session));
}

TEST_CASE(
    "the model Bash tool is always registered alongside the Session-owned User Shell",
    "[coding_agent][runtime][assembly][issue90]") {
    tests::TempWorkspace workspace;
    auto created = create_cli_session(workspace, true);
    REQUIRE(created);
    auto& session = *created->session;

    // The model Bash Tool is always available under the fixed tool set (the
    // --enable-bash opt-in is gone) and executes in the workspace.
    const auto prompted = run_prompt_blocking(session, "bash echo model-tool-output");
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
    const auto included = run_user_bash_blocking(session, "printf 'still-direct\\n'");
    REQUIRE(included);
    CHECK(included->message.output.find("still-direct") != std::string::npos);
    CHECK_FALSE(included->message.exclude_from_context);
    CHECK(bash_message_count(session.snapshot().agent_state.messages) == 1);
}
