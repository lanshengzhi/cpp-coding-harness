// P18 evidence (#414): the System Prompt is built at session construction in
// pi's exact shape (ADR 0036 G4) and flows into every run through
// `AgentContext.system_prompt`, exactly like pi's `agent.state.systemPrompt`
// → `createContextSnapshot()`. The identity delta (the "pike" identity line
// and the C++ binary's own docs paths) is pinned byte-for-byte by the
// differential golden in `tests/fixtures/prompts/` (SystemPromptBuilderTest);
// this test pins the flow end to end: a scripted provider records the request
// context the session built. No live keys or network.

#include "ai/ModelStreamBridge.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ExpectedMacros.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// A recording scripted provider that answers every request with one plain
/// success terminal (the #326 terminal contract) and keeps the request
/// contexts for inspection.
class RecordingProvider final : public tests::ScriptedProvider {
public:
    RecordingProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context)](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        requests.push_back(context);
        auto terminal = ai::assistant_text_message("done");
        terminal.provider = "sdk-host";
        terminal.api = "fake";
        terminal.model = model.id;
        terminal.timestamp = 1718000000123;
        co_return terminal;
                });
    }


    std::vector<ai::AiContext> requests;
};

/// Session creation against the scripted-client seam (a real ModelRuntime
/// composed over scripted providers, ADR 0034 "primary seam — Agent +
/// session-assembly composition").
[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult>
create_scripted_session(
    std::shared_ptr<RecordingProvider> client,
    const std::filesystem::path& session_file,
    const std::filesystem::path& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = workspace;
    options.models = cch::tests::models_from_provider(std::move(client));
    options.request_model = cch::tests::scripted_request_model("sdk-host", "sdk-model");
    return coding_agent::create_agent_session(std::move(options));
}

} // namespace

TEST_CASE("system prompt is built at session construction and flows through AgentContext.system_prompt", "[coding_agent][system-prompt][issue414]") {
    tests::TempWorkspace workspace;
    const auto session_file = workspace.path() / "session.jsonl";
    auto client = std::make_shared<RecordingProvider>();

    auto created = create_scripted_session(client, session_file, workspace.path());
    REQUIRE(created.has_value());

    auto prompted = created->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client->requests.size() == 1);

    const auto& system_prompt = client->requests[0].system_prompt;
    REQUIRE(system_prompt.has_value());
    // Copy: the vector reallocates on the second recorded request below.
    const std::string prompt = *system_prompt;

    // The Pike identity line (the only identity delta from pi).
    CHECK(prompt.find("You are an expert coding assistant operating inside pike, a "
                      "coding agent harness.") != std::string::npos);

    // The four fixed tools with pi's verbatim snippets.
    CHECK(prompt.find(
              "Available tools:\n"
              "- read: Read file contents\n"
              "- bash: Execute bash commands (ls, grep, find, etc.)\n"
              "- edit: Make precise file edits with exact text replacement, "
              "including multiple disjoint edits in one call\n"
              "- write: Create or overwrite files\n") != std::string::npos);

    // The auto bash-exploration rule and the always-lines.
    CHECK(prompt.find("- Use bash for file operations like ls, rg, find\n") != std::string::npos);
    CHECK(prompt.find("- Be concise in your responses\n") != std::string::npos);
    CHECK(prompt.find("- Show file paths clearly when working with files\n") != std::string::npos);

    // The identity-adjusted documentation block resolves the C++ binary's own
    // docs paths from the source tree.
    CHECK(prompt.find("- Main documentation: " + std::string{CCH_SOURCE_DIR} + "/README.md\n") != std::string::npos);
    CHECK(prompt.find("- Additional docs: " + std::string{CCH_SOURCE_DIR} + "/docs\n") != std::string::npos);
    CHECK(prompt.find("- Examples: " + std::string{CCH_SOURCE_DIR} + "/examples (extensions, custom tools, SDK)\n") != std::string::npos);

    // The trailing posix-normalized cwd line.
    CHECK(prompt.find(
              "\nCurrent working directory: " + workspace.path().string()) != std::string::npos);

    // The built prompt is the Agent's live state value (pi `state.systemPrompt`).
    CHECK(created->session->snapshot().agent_state.system_prompt == prompt);

    // Every subsequent run carries the same built prompt.
    auto second = created->session->prompt_blocking("again");
    REQUIRE(second.has_value());
    REQUIRE(client->requests.size() == 2);
    REQUIRE(client->requests[1].system_prompt.has_value());
    CHECK(*client->requests[1].system_prompt == prompt);

    created->session->close();
}

// ─────────────────────────────────────────────────────────────────────────────
// P20 (#416): Project Context Files and SYSTEM.md/APPEND_SYSTEM.md flow
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Build a scripted session request against a nested workspace the test
/// controls (so the cwd ancestor chain is fully deterministic).
[[nodiscard]] tests::ModelsSessionOptions context_session_options(
    const std::filesystem::path& session_file,
    const std::filesystem::path& workspace,
    std::shared_ptr<RecordingProvider> client) {
    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = workspace;
    options.models = cch::tests::models_from_provider(std::move(client));
    options.request_model = cch::tests::scripted_request_model("sdk-host", "sdk-model");
    return options;
}

} // namespace

TEST_CASE(
    "system prompt default branch renders project context files in pi's order",
    "[coding_agent][system-prompt][context-files][issue416]") {
    tests::TempWorkspace workspace;
    // Controlled ancestor chain: parent (AGENTS.md) and child (CLAUDE.md),
    // global agent dir absent (isolated test home).
    workspace.write("parent/AGENTS.md", "parent instructions\n");
    workspace.write("parent/child/CLAUDE.md", "child instructions\n");
    const auto child = workspace.path() / "parent" / "child";
    const auto session_file = child / "session.jsonl";
    auto client = std::make_shared<RecordingProvider>();

    auto options = context_session_options(session_file, child, client);
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    auto prompted = created->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].system_prompt.has_value());
    const std::string prompt = *client->requests[0].system_prompt;

    // The default branch still renders the tools; the context section lands
    // between the append area and the trailing cwd line, root-most first.
    CHECK(prompt.find("Available tools:") != std::string::npos);
    const std::string expected_section =
        "\n\n<project_context>\n\n"
        "Project-specific instructions and guidelines:\n\n"
        "<project_instructions path=\"" +
        (workspace.path() / "parent" / "AGENTS.md").string() +
        "\">\n"
        "parent instructions\n\n"
        "</project_instructions>\n\n"
        "<project_instructions path=\"" +
        (child / "CLAUDE.md").string() +
        "\">\n"
        "child instructions\n\n"
        "</project_instructions>\n\n"
        "</project_context>\n";
    CHECK(prompt.find(expected_section) != std::string::npos);
    CHECK(prompt.find("\nCurrent working directory: " + child.string()) !=
          std::string::npos);

    created->session->close();
}

TEST_CASE(
    "system prompt custom branch renders the custom prompt, joined appends, and context files",
    "[coding_agent][system-prompt][context-files][issue416]") {
    tests::TempWorkspace workspace;
    workspace.write("parent/AGENTS.md", "parent instructions\n");
    workspace.write("parent/child/AGENTS.md", "child instructions\n");
    workspace.write("parent/child/custom.md", "custom file prompt\n");
    const auto child = workspace.path() / "parent" / "child";
    const auto session_file = child / "session.jsonl";
    auto client = std::make_shared<RecordingProvider>();

    auto options = context_session_options(session_file, child, client);
    // pi `--system-prompt <file>`: text-or-file resolution reads the file.
    options.session_facts.system_prompt = (child / "custom.md").string();
    // pi `--append-system-prompt` repeatable: joined with "\n\n".
    options.session_facts.append_system_prompt = {"first append", "second append"};
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    auto prompted = created->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].system_prompt.has_value());
    const std::string prompt = *client->requests[0].system_prompt;

    // Custom branch: no default tools section; the prompt opens with the
    // resolved file content.
    CHECK(prompt.starts_with("custom file prompt\n"));
    CHECK(prompt.find("Available tools:") == std::string::npos);
    // Append strings joined with "\n\n" right after the custom prompt.
    // The resolved file content carries its trailing newline; pi appends the
    // section with another "\n\n" prefix, and the two append strings join
    // with "\n\n".
    CHECK(prompt.find("custom file prompt\n\n\nfirst append\n\nsecond append") !=
          std::string::npos);
    // Context files still render, root-most first.
    CHECK(prompt.find(
              "<project_instructions path=\"" +
              (workspace.path() / "parent" / "AGENTS.md").string() + "\">") !=
          std::string::npos);
    CHECK(prompt.find(
              "<project_instructions path=\"" + (child / "AGENTS.md").string() +
              "\">") != std::string::npos);
    CHECK(prompt.find("\nCurrent working directory: " + child.string()) !=
          std::string::npos);

    created->session->close();
}

TEST_CASE(
    "system prompt flows the discovered SYSTEM.md and APPEND_SYSTEM.md through the custom branch",
    "[coding_agent][system-prompt][context-files][issue416]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/SYSTEM.md", "custom system prompt from SYSTEM.md\n");
    workspace.write(".pi/APPEND_SYSTEM.md", "append from APPEND_SYSTEM.md\n");
    const auto session_file = workspace.path() / "session.jsonl";
    auto client = std::make_shared<RecordingProvider>();

    auto options = context_session_options(session_file, workspace.path(), client);
    // The `.pi/` markers trigger the trust decision; `--approve` trusts the
    // project so the project SYSTEM.md/APPEND_SYSTEM.md load.
    options.project_trust_override = true;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    auto prompted = created->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].system_prompt.has_value());
    const std::string prompt = *client->requests[0].system_prompt;

    CHECK(prompt.starts_with("custom system prompt from SYSTEM.md\n"));
    // The SYSTEM.md content carries its trailing newline before pi's "\n\n"
    // append-section prefix.
    CHECK(prompt.find("custom system prompt from SYSTEM.md\n\n\nappend from APPEND_SYSTEM.md\n") !=
          std::string::npos);
    CHECK(prompt.find("Available tools:") == std::string::npos);
    CHECK(prompt.find("\nCurrent working directory: " + workspace.path().string()) !=
          std::string::npos);

    created->session->close();
}

TEST_CASE(
    "system prompt drops project context files under --no-context-files",
    "[coding_agent][system-prompt][context-files][issue416]") {
    tests::TempWorkspace workspace;
    workspace.write("AGENTS.md", "should not appear\n");
    const auto session_file = workspace.path() / "session.jsonl";
    auto client = std::make_shared<RecordingProvider>();

    auto options = context_session_options(session_file, workspace.path(), client);
    options.session_facts.no_context_files = true;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    auto prompted = created->session->prompt_blocking("hello");
    REQUIRE(prompted.has_value());
    REQUIRE(client->requests.size() == 1);
    REQUIRE(client->requests[0].system_prompt.has_value());
    const std::string prompt = *client->requests[0].system_prompt;

    CHECK(prompt.find("<project_context>") == std::string::npos);
    CHECK(prompt.find("should not appear") == std::string::npos);

    created->session->close();
}
