// P18 evidence (#414): the System Prompt is built at session construction in
// pi's exact shape (ADR 0036 G4) and flows into every run through
// `AgentContext.system_prompt`, exactly like pi's `agent.state.systemPrompt`
// → `createContextSnapshot()`. The identity delta (the "cch" identity line
// and the C++ binary's own docs paths) is pinned byte-for-byte by the
// differential golden in `tests/fixtures/prompts/` (SystemPromptBuilderTest);
// this test pins the flow end to end: a scripted provider records the request
// context the session built. No live keys or network.

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "util/ExpectedMacros.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

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

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions,
        ai::AssistantEventSink) override {
        requests.push_back(context);
        auto terminal = ai::assistant_text_message("done");
        terminal.provider = "sdk-host";
        terminal.api = "fake";
        terminal.model = model.id;
        terminal.timestamp = 1718000000123;
        co_return terminal;
    }

    std::vector<ai::AiContext> requests;
};

/// Session creation against the scripted-client seam (a real ModelRuntime
/// composed over scripted providers, ADR 0034 "primary seam — Agent +
/// session-assembly composition").
[[nodiscard]] util::Expected<coding_agent::CreateAgentSessionResult>
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

    // The cch identity line (the only identity delta from pi).
    CHECK(prompt.find(
              "You are an expert coding assistant operating inside cch, a "
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
