// `/reload` runtime evidence (#418, pi `AgentSession.reload()` +
// `resourceLoader.reload()`): re-reading User Settings, skills, prompt
// templates, Project Context Files, and SYSTEM/APPEND sources through the
// retained loading request; System Prompt rebuild into the live Agent; trust
// preservation; and the fatal-error path. No live keys or network: every
// session runs against the scripted fake provider seam.

#include "ai/ModelStreamBridge.hpp"
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Skill.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace cch;

namespace {

/// A recording scripted provider that answers every request with one plain
/// success terminal (the #326 terminal contract).
class ReloadRecordingProvider final : public tests::ScriptedProvider {
public:
    ReloadRecordingProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context)](
                ai::AssistantEventSink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
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

/// Drives one `AgentSession::reload()` on a temporary executor and returns
/// the outcome.
[[nodiscard]] util::Expected<coding_agent::runtime::AgentSessionReloadResult> run_reload(
    coding_agent::AgentSession& session) {
    boost::asio::io_context io;
    std::optional<util::Expected<coding_agent::runtime::AgentSessionReloadResult>> result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            result = co_await session.reload();
            co_return;
        },
        boost::asio::detached);
    io.run();
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// Creates a trusted session whose workspace carries one skill, one prompt
/// template, a SYSTEM.md/APPEND_SYSTEM.md pair, and an AGENTS.md context file
/// — everything `/reload` re-reads.
struct ReloadFixture {
    tests::TempWorkspace workspace;
    std::shared_ptr<ReloadRecordingProvider> client;
    std::unique_ptr<coding_agent::AgentSession> session;

    void create(bool trusted = true) {
        workspace.write(
            ".pi/skills/proj-skill/SKILL.md",
            "---\n"
            "name: proj-skill\n"
            "description: initial skill description.\n"
            "---\n"
            "Initial skill body.\n");
        workspace.write(
            ".pi/prompts/proj-prompt.md",
            "---\n"
            "description: initial prompt description.\n"
            "---\n"
            "Initial prompt body: $ARGUMENTS\n");
        workspace.write(".pi/SYSTEM.md", "initial system prompt from SYSTEM.md\n");
        workspace.write(".pi/APPEND_SYSTEM.md", "initial append from APPEND_SYSTEM.md\n");
        workspace.write("AGENTS.md", "initial project instructions\n");
        const auto session_file = workspace.path() / "session.jsonl";
        client = std::make_shared<ReloadRecordingProvider>();
        tests::ModelsSessionOptions options;
        options.session_target =
            coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
        options.workspace = workspace.path();
        options.models = cch::tests::models_from_provider(client);
        options.request_model =
            cch::tests::scripted_request_model("sdk-host", "sdk-model");
        options.project_trust_override = trusted;
        auto created = coding_agent::create_agent_session(std::move(options));
        REQUIRE(created.has_value());
        session = std::move(created->session);
    }
};

} // namespace

TEST_CASE(
    "reload re-reads skills, templates, context files, and SYSTEM/APPEND and rebuilds the system prompt",
    "[coding_agent][reload][issue418]") {
    ReloadFixture fixture;
    fixture.create(/*trusted*/ true);
    auto* session = fixture.session.get();

    // The creation-time system prompt reflects the initial resources.
    const auto initial_prompt = session->snapshot().agent_state.system_prompt;
    CHECK(initial_prompt.find("initial skill description.") != std::string::npos);
    CHECK(initial_prompt.find("initial system prompt from SYSTEM.md") != std::string::npos);
    CHECK(initial_prompt.find("initial append from APPEND_SYSTEM.md") != std::string::npos);
    CHECK(initial_prompt.find("initial project instructions") != std::string::npos);
    CHECK(session->system_prompt_source().has_value());
    CHECK(session->append_system_prompt_sources().size() == 1);
    CHECK(session->context_files().size() == 1);

    // Edit every resource between create and reload.
    fixture.workspace.write(
        ".pi/skills/proj-skill/SKILL.md",
        "---\n"
        "name: proj-skill\n"
        "description: reloaded skill description.\n"
        "---\n"
        "Reloaded skill body.\n");
    fixture.workspace.write(
        ".pi/prompts/proj-prompt.md",
        "---\n"
        "description: reloaded prompt description.\n"
        "---\n"
        "Reloaded prompt body: $ARGUMENTS\n");
    fixture.workspace.write(".pi/SYSTEM.md", "reloaded system prompt from SYSTEM.md\n");
    fixture.workspace.write(".pi/APPEND_SYSTEM.md", "reloaded append from APPEND_SYSTEM.md\n");
    fixture.workspace.write("AGENTS.md", "reloaded project instructions\n");

    auto result = run_reload(*session);
    REQUIRE(result.has_value());

    // Skills/templates swap live; the loader resolved the fresh files.
    REQUIRE(session->skills().size() == 1);
    CHECK(session->skills().front().description == "reloaded skill description.");
    CHECK(session->skills().front().sourceInfo.source == "auto");
    CHECK(session->skills().front().sourceInfo.scope == coding_agent::SourceScope::Project);
    REQUIRE(session->templates().size() == 1);
    CHECK(session->templates().front().name == "proj-prompt");
    CHECK(session->templates().front().sourceInfo.source == "auto");
    CHECK(session->templates().front().sourceInfo.scope == coding_agent::SourceScope::Project);
    REQUIRE(session->context_files().size() == 1);
    CHECK(session->context_files().front().content == "reloaded project instructions\n");

    // The System Prompt rebuilt from the fresh inputs and the live Agent
    // state advanced (pi `_rebuildSystemPrompt` → `agent.state.systemPrompt`).
    const auto reloaded_prompt = session->snapshot().agent_state.system_prompt;
    CHECK(reloaded_prompt.find("reloaded skill description.") != std::string::npos);
    CHECK(reloaded_prompt.find("initial skill description.") == std::string::npos);
    CHECK(reloaded_prompt.find("reloaded system prompt from SYSTEM.md") != std::string::npos);
    CHECK(reloaded_prompt.find("reloaded append from APPEND_SYSTEM.md") != std::string::npos);
    CHECK(reloaded_prompt.find("reloaded project instructions") != std::string::npos);

    // Sources and per-kind diagnostics refresh through the runtime getters.
    CHECK(session->system_prompt_source().has_value());
    CHECK(session->append_system_prompt_sources().size() == 1);
    CHECK(session->skill_diagnostics().empty());
    CHECK(session->prompt_diagnostics().empty());
    CHECK(session->theme_diagnostics().empty());
    // The result carries the same per-kind diagnostics and theme documents.
    CHECK(result->skill_diagnostics.empty());
    CHECK(result->prompt_diagnostics.empty());
    CHECK(result->theme_diagnostics.empty());
    CHECK(result->themes.empty());

    // The next prompt streams the rebuilt System Prompt (pi
    // `AgentContext.system_prompt`).
    REQUIRE(session->prompt_blocking("hello").has_value());
    REQUIRE_FALSE(fixture.client->requests.empty());
    CHECK(fixture.client->requests.back().system_prompt == reloaded_prompt);
}

TEST_CASE(
    "reload preserves the creation-time project trust decision",
    "[coding_agent][reload][trust][issue418]") {
    // Trusted: reload re-reads the trust-gated project resources and stays
    // trusted (pi `settingsManager.reload()` preserves `projectTrusted`).
    {
        ReloadFixture trusted;
        trusted.create(/*trusted*/ true);
        auto* session = trusted.session.get();
        REQUIRE(coding_agent::detail::AgentSessionInteractiveAccess::is_project_trusted(*session));
        REQUIRE(run_reload(*session).has_value());
        CHECK(coding_agent::detail::AgentSessionInteractiveAccess::is_project_trusted(*session));
        CHECK(session->skills().size() == 1);
    }
    // Untrusted: reload keeps the untrusted decision; project resources stay
    // dropped (pi `reload()` re-runs with the preserved trust state).
    {
        ReloadFixture untrusted;
        untrusted.create(/*trusted*/ false);
        auto* session = untrusted.session.get();
        CHECK_FALSE(coding_agent::detail::AgentSessionInteractiveAccess::is_project_trusted(*session));
        // Untrusted: no project resources load at creation.
        CHECK(session->skills().empty());
        REQUIRE(run_reload(*session).has_value());
        CHECK_FALSE(coding_agent::detail::AgentSessionInteractiveAccess::is_project_trusted(*session));
        CHECK(session->skills().empty());
        CHECK(session->templates().empty());
    }
}

TEST_CASE(
    "reload returns theme documents for re-registration and reports a fatal explicit-resource failure",
    "[coding_agent][reload][issue418]") {
    // A custom project theme flows back through the reload result so the TUI
    // re-runs `discover_themes` (pi `getThemes().themes`).
    {
        ReloadFixture fixture;
        fixture.create(/*trusted*/ true);
        fixture.workspace.write(
            ".pi/themes/custom.json",
            R"({"name":"custom","colors":{"bg":"#000000","fg":"#ffffff","accent":"#00ff00","border":"#ffffff","error":"#ff0000","warning":"#ffff00","muted":"#888888","dim":"#666666","mdHeading":"#ffffff","mdCode":"#ffffff","mdCodeBg":"#222222","mdLink":"#00ffff","mdQuote":"#888888","mdListMarker":"#ffffff","mdHr":"#ffffff","mdInlineCode":"#ffffff","mdInlineCodeBg":"#222222","toolBg":"#111111","toolHeaderBg":"#222222","toolArgs":"#bbbbbb","toolOutput":"#cccccc","toolSuccessBg":"#003300","toolErrorBg":"#330000","toolPendingBg":"#222222","toolBorder":"#444444","userBg":"#1a1a2e","userText":"#ffffff","assistantBg":"#000000","assistantText":"#ffffff","thinkingBg":"#000000","thinkingText":"#888888","compactionBg":"#000000","customMessageBg":"#000000","customMessageText":"#cccccc","customMessageLabel":"#ffffff","scrollbarThumb":"#444444","selectedBg":"#333366","bashBg":"#111111","bashText":"#00ff00","bashHeaderBg":"#111111","bashHeaderText":"#00ff00","borderAccent":"#00ff00","borderMuted":"#444444","statusAccent":"#00ff00","statusWarning":"#ffff00","statusError":"#ff0000","statusMuted":"#888888","statusDim":"#666666","selectionBg":"#333366","selectionFg":"#ffffff","inputBg":"#000000","inputFg":"#ffffff","spinner":"#00ff00","errorText":"#ff0000","warningText":"#ffff00","idleStatus":"#666666","idleStatusDim":"#444444","footerBg":"#111111","footerText":"#bbbbbb","footerMuted":"#888888","footerDim":"#666666","footerAccent":"#00ff00","tuiBg":"#000000","tuiFg":"#ffffff","tuiBorder":"#444444"}})");
        auto* session = fixture.session.get();
        auto result = run_reload(*session);
        REQUIRE(result.has_value());
        REQUIRE(result->themes.size() == 1);
        CHECK(result->themes.front().scope == coding_agent::SourceScope::Project);
        CHECK(result->themes.front().path.find(".pi/themes/custom.json") != std::string::npos);
    }

    // A retained explicit prompt-template path that no longer resolves is a
    // fatal reload error (the TUI shows `Reload failed: ...`).
    {
        ReloadFixture fixture;
        fixture.create(/*trusted*/ true);
        auto* session = fixture.session.get();
        // The retained request gains an explicit prompt-template file that we
        // delete before reload. (The reload re-runs the SAME retained request,
        // so the file must exist at creation time to be retained.)
        fixture.workspace.write("explicit-template.md", "# Explicit\n\nBody: $ARGUMENTS\n");
        // Recreate with the explicit template path (workspace-relative) in
        // the request.
        const auto session_file = fixture.workspace.path() / "session2.jsonl";
        auto client = std::make_shared<ReloadRecordingProvider>();
        tests::ModelsSessionOptions options;
        options.session_target =
            coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
        options.workspace = fixture.workspace.path();
        options.models = cch::tests::models_from_provider(client);
        options.request_model =
            cch::tests::scripted_request_model("sdk-host", "sdk-model");
        options.project_trust_override = true;
        options.prompt_template_paths = {"explicit-template.md"};
        auto created = coding_agent::create_agent_session(std::move(options));
        REQUIRE(created.has_value());
        auto second = std::move(created->session);
        // The project prompt plus the explicit template are both present.
        REQUIRE(second->templates().size() == 2);
        REQUIRE(std::filesystem::remove(fixture.workspace.path() / "explicit-template.md"));

        const auto failed = run_reload(*second);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message.find("reload") != std::string::npos);
    }
}

TEST_CASE(
    "reload surfaces per-kind diagnostics through the result",
    "[coding_agent][reload][diagnostics][issue418]") {
    ReloadFixture fixture;
    fixture.create(/*trusted*/ true);
    // Two skills with the same name from different sources: project `.pi`
    // first (wins), then the user `.agents/skills` convention would be
    // needed — instead, use an explicit `--skill` path that collides.
    fixture.workspace.write(
        ".pi/skills/dup/SKILL.md",
        "---\n"
        "name: dup\n"
        "description: project duplicate.\n"
        "---\n"
        "Project dup body.\n");
    fixture.workspace.write(
        "dup-skill/SKILL.md",
        "---\n"
        "name: dup\n"
        "description: explicit duplicate.\n"
        "---\n"
        "Explicit dup body.\n");

    const auto session_file = fixture.workspace.path() / "session.jsonl";
    auto client = std::make_shared<ReloadRecordingProvider>();
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = fixture.workspace.path();
    options.models = cch::tests::models_from_provider(client);
    options.request_model = cch::tests::scripted_request_model("sdk-host", "sdk-model");
    options.project_trust_override = true;
    options.skill_paths = {"dup-skill/SKILL.md"};
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    auto* session = created->session.get();

    // The explicit duplicate collides with the discovered project skill
    // (first wins); the collision diagnostic lands in the skill bucket.
    auto result = run_reload(*session);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->skill_diagnostics.empty());
    const auto& collision = result->skill_diagnostics.front();
    CHECK(collision.type == coding_agent::ResourceDiagnosticType::Collision);
    REQUIRE(collision.collision.has_value());
    CHECK(collision.collision->name == "dup");
    CHECK(session->skill_diagnostics().size() == result->skill_diagnostics.size());
}

namespace {

/// A FIFO scripted provider that blocks one request (the compaction
/// summarization) until released, so a manual compaction stays in flight for
/// signal observation.
class GatedCompactionProvider final : public tests::ScriptedProvider {
public:
    GatedCompactionProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
        ai::Model model,
        ai::AiContext context,
        ai::ProviderStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model), context = std::move(context)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        ++request_count;
        requests.push_back(context);
        if (gate_request_number && request_count == *gate_request_number) {
            gated = true;
            gate_.emplace(co_await boost::asio::this_coro::executor);
            gate_->expires_at(std::chrono::steady_clock::time_point::max());
            boost::system::error_code error;
            co_await gate_->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, error));
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        response.provider = "sdk-host";
        response.api = "fake";
        response.model = model.id;
        response.timestamp = 1718000000123;
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
        }
        co_return response;
                });
    }


    void release_gate() {
        if (gate_) {
            gate_->cancel();
        }
    }

    int request_count{0};
    std::optional<int> gate_request_number;
    bool gated{false};
    std::deque<ai::AssistantMessage> responses;
    std::vector<ai::AiContext> requests;
    std::optional<boost::asio::steady_timer> gate_;
};

[[nodiscard]] ai::AssistantMessage big_assistant(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    message.usage = ai::Usage{};
    message.usage.input = 5000;
    message.usage.output = 1000;
    message.usage.total_tokens = 6000;
    return message;
}

[[nodiscard]] ai::AssistantMessage summarization_response() {
    auto summary = ai::assistant_text_message("## Goal\nCompacted history summary");
    summary.usage = ai::Usage{};
    summary.usage.input = 3000;
    summary.usage.output = 100;
    return summary;
}

} // namespace

TEST_CASE(
    "manual compaction exposes is_compacting while is_streaming stays false (reload refusal signal)",
    "[coding_agent][reload][compaction][issue418]") {
    tests::TempWorkspace workspace;
    const auto session_file = workspace.path() / "session.jsonl";
    const std::string big(20000, 'x');
    auto client = std::make_shared<GatedCompactionProvider>();
    auto* client_ptr = client.get();
    client_ptr->responses.push_back(big_assistant("a1 " + big));
    client_ptr->responses.push_back(big_assistant("a2 " + big));
    client_ptr->responses.push_back(big_assistant("a3 " + big));
    client_ptr->gate_request_number = 4; // the summarization call

    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(client);
    options.request_model =
        cch::tests::scripted_request_model("sdk-host", "sdk-model");
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    auto* session = created->session.get();
    REQUIRE(session->prompt_blocking(big + " u1").has_value());
    REQUIRE(session->prompt_blocking(big + " u2").has_value());
    REQUIRE(session->prompt_blocking(big + " u3").has_value());
    client_ptr->responses.push_back(summarization_response());

    // Drive a manual compaction on a dedicated executor and observe the
    // mid-flight signals: `is_compacting` true while `is_streaming` stays
    // false (pi `isCompacting` ≠ `isStreaming`), so the TUI's `/reload`
    // refusal shows the compaction warning.
    boost::asio::io_context io;
    std::optional<util::Expected<coding_agent::CompactionResult>> compact_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            compact_result = co_await session->compact();
            co_return;
        },
        boost::asio::detached);
    while (!client_ptr->gated && io.poll() != 0) {
    }
    REQUIRE(client_ptr->gated);
    CHECK(session->is_compacting());
    CHECK_FALSE(session->is_streaming());
    CHECK(session->is_busy());

    client_ptr->release_gate();
    io.run();
    REQUIRE(compact_result.has_value());
    REQUIRE(compact_result->has_value());
}
