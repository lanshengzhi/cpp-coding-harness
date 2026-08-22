#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include "coding_agent/tui/TestTuiActionSink.hpp"
#include "support/TempWorkspace.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include <cch/coding_agent/ModelRuntime.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/io_context.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::PromptOptions;
using coding_agent::tui::InteractiveSessionRun;
using coding_agent::tui::InteractiveSessionRunBuilder;
using coding_agent::tui::TuiActionVariant;
using coding_agent::tui::OpenBrowserAction;
using coding_agent::tui::ReplaceSessionAction;
using coding_agent::tui::ReportBootCreationFailureAction;
using coding_agent::tui::ReportBootDiagnosticsAction;

class DummyClipboardReader final : public coding_agent::tui::AsyncClipboardReader {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<coding_agent::tui::ClipboardImage>>>
    read_image() override {
        co_return std::optional<coding_agent::tui::ClipboardImage>{std::nullopt};
    }

    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<std::string>>>
    read_text() override {
        co_return std::optional<std::string>{std::nullopt};
    }
};

} // namespace

TEST_CASE(
    "InteractiveSessionRun carries facts, run-intent values, and capability injections",
    "[coding_agent][tui][session_run][issue518]") {
    coding_agent::runtime::InteractiveSessionFacts facts;
    facts.no_skills = true;
    facts.no_prompt_templates = true;
    facts.model = "test-model-42";
    facts.provider = "test-provider-alpha";

    auto run = InteractiveSessionRunBuilder{}
        .with_session_facts(facts)
        .with_agent_config_directory(std::filesystem::path{"/test/agent/config"})
        .with_initial_prompt(std::optional<std::string>{"initial user message"})
        .with_initial_prompt_options(PromptOptions{.expand_prompt_templates = true, .images = {}})
        .with_model_fallback_message(std::optional<std::string>{"model fallback triggered"})
        .with_clipboard_reader(std::make_unique<DummyClipboardReader>())
        .build();

    CHECK(run.session_facts().no_skills);
    CHECK(run.session_facts().no_prompt_templates);
    CHECK(run.session_facts().model == "test-model-42");
    CHECK(run.session_facts().provider == "test-provider-alpha");
    CHECK(run.agent_config_directory() == std::filesystem::path{"/test/agent/config"});
    REQUIRE(run.initial_prompt().has_value());
    CHECK(*run.initial_prompt() == "initial user message");
    CHECK(run.initial_prompt_options().expand_prompt_templates);
    REQUIRE(run.model_fallback_message().has_value());
    CHECK(*run.model_fallback_message() == "model fallback triggered");
    CHECK(run.has_clipboard_reader());
    auto reader = run.take_clipboard_reader();
    CHECK(reader != nullptr);
    CHECK_FALSE(run.has_clipboard_reader());
    CHECK_FALSE(run.creation_failure_reported());
    CHECK(std::holds_alternative<coding_agent::tui::BindExistingSession>(run.session_intent()));
}

TEST_CASE(
    "InteractiveSessionRunBuilder assembles BindExistingSession and DeferBoot intents",
    "[coding_agent][tui][session_run][issue518]") {
    tests::TempWorkspace workspace;
    auto options = coding_agent::runtime::AgentSessionCreationRequest{};
    options.workspace = workspace.path();
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.session_facts.no_skills = true;
    options.session_facts.no_prompt_templates = true;
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    // 1. BindExistingSession via with_session
    auto bind_run = InteractiveSessionRunBuilder{}
        .with_session(*created->session)
        .build();

    const auto* bind = std::get_if<coding_agent::tui::BindExistingSession>(&bind_run.session_intent());
    REQUIRE(bind != nullptr);
    CHECK(bind->session == created->session.get());

    auto taken_intent = bind_run.take_session_intent();
    const auto* taken_bind = std::get_if<coding_agent::tui::BindExistingSession>(&taken_intent);
    REQUIRE(taken_bind != nullptr);
    CHECK(taken_bind->session == created->session.get());

    const auto* reset_bind = std::get_if<coding_agent::tui::BindExistingSession>(&bind_run.session_intent());
    REQUIRE(reset_bind != nullptr);
    CHECK(reset_bind->session == nullptr);

    // 2. DeferBoot via with_defer_boot
    coding_agent::runtime::AgentSessionCreationRequest defer_req;
    defer_req.workspace = workspace.path();
    defer_req.session_target = coding_agent::ExplicitResumeSessionTarget{"/path/to/session.jsonl"};

    auto defer_run = InteractiveSessionRunBuilder{}
        .with_defer_boot(std::move(defer_req))
        .build();

    const auto* defer = std::get_if<coding_agent::tui::DeferBoot>(&defer_run.session_intent());
    REQUIRE(defer != nullptr);
    CHECK(defer->request.workspace == workspace.path());
    CHECK(std::holds_alternative<coding_agent::ExplicitResumeSessionTarget>(defer->request.session_target));
}

TEST_CASE(
    "InteractiveSessionRun dispatches host effects for ReplaceSessionAction installing runtime capabilities",
    "[coding_agent][tui][session_run][issue517]") {
    tests::TempWorkspace workspace;
    auto io = std::make_shared<boost::asio::io_context>();
    auto runtime_root = std::make_shared<harness::RuntimeRoot>(io, harness::RuntimeLimits{});
    auto shared_runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(shared_runtime.has_value());

    coding_agent::runtime::InteractiveSessionFacts facts;
    facts.no_skills = true;
    facts.no_prompt_templates = true;

    auto run = InteractiveSessionRunBuilder{}
        .with_session_facts(facts)
        .with_runtime_root(runtime_root)
        .with_shared_runtime(*shared_runtime)
        .build();

    coding_agent::runtime::AgentSessionCreationRequest request;
    request.workspace = workspace.path();
    request.session_target = coding_agent::InMemorySessionTarget{};

    auto result = run.dispatch_action(0, TuiActionVariant{ReplaceSessionAction{std::move(request)}});
    REQUIRE(result.has_value());

    auto* session_result =
        std::get_if<support::Expected<coding_agent::CreateAgentSessionResult>>(&*result);
    REQUIRE(session_result != nullptr);
    REQUIRE(session_result->has_value());
    CHECK((*session_result)->session != nullptr);
}

TEST_CASE(
    "InteractiveSessionRun dispatches host effects for ReportBootCreationFailureAction",
    "[coding_agent][tui][session_run][issue517]") {
    std::ostringstream error_stream;

    auto run = InteractiveSessionRunBuilder{}
        .with_error_stream(&error_stream)
        .with_is_resume_target(false)
        .build();

    CHECK_FALSE(run.creation_failure_reported());

    const auto error = support::make_error(
        support::ErrorCode::Unknown,
        "Failed to bootstrap session",
        "Permission denied");

    auto result = run.dispatch_action(0, TuiActionVariant{ReportBootCreationFailureAction{error}});
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<std::monostate>(*result));

    CHECK(run.creation_failure_reported());
    const std::string text = error_stream.str();
    CHECK(text.find("could not create session: Failed to bootstrap session") != std::string::npos);
    CHECK(text.find("Permission denied") != std::string::npos);
}

TEST_CASE(
    "InteractiveSessionRun dispatches host effects for ReportBootDiagnosticsAction",
    "[coding_agent][tui][session_run][issue517]") {
    std::ostringstream error_stream;

    auto run = InteractiveSessionRunBuilder{}
        .with_error_stream(&error_stream)
        .build();

    std::vector<coding_agent::SessionDiagnostic> diagnostics{
        coding_agent::SessionDiagnostic{
            .severity = coding_agent::SessionDiagnostic::Severity::Info,
            .code = "trust:loaded",
            .message = "Loaded trust configuration",
            .path = std::nullopt,
        },
        coding_agent::SessionDiagnostic{
            .severity = coding_agent::SessionDiagnostic::Severity::Warning,
            .code = "theme:missing",
            .message = "Theme color not found",
            .path = std::optional<std::string>{"theme.json"},
        },
        coding_agent::SessionDiagnostic{
            .severity = coding_agent::SessionDiagnostic::Severity::Error,
            .code = "skills:syntax",
            .message = "Invalid skill definition",
            .path = std::optional<std::string>{"SKILL.md"},
        },
    };

    auto result = run.dispatch_action(0, TuiActionVariant{ReportBootDiagnosticsAction{std::move(diagnostics)}});
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<std::monostate>(*result));

    const std::string text = error_stream.str();
    CHECK(text.find("[trust:info] loaded: Loaded trust configuration") != std::string::npos);
    CHECK(text.find("[theme:warn] missing: Theme color not found (theme.json)") != std::string::npos);
    CHECK(text.find("[skills:error] syntax: Invalid skill definition (SKILL.md)") != std::string::npos);
}

TEST_CASE(
    "InteractiveSessionRun supports custom action sink override",
    "[coding_agent][tui][session_run][issue517]") {
    auto recorder = std::make_shared<coding_agent::tui::testing::ActionSinkRecorder>();

    auto run = InteractiveSessionRunBuilder{}
        .with_action_sink(recorder->make_sink())
        .build();

    auto sink = run.make_action_sink();
    REQUIRE(sink != nullptr);

    auto result = sink(42, TuiActionVariant{OpenBrowserAction{.url = "https://example.com"}});
    REQUIRE(result.has_value());

    REQUIRE(recorder->generations.size() == 1);
    CHECK(recorder->generations[0] == 42);
    REQUIRE(recorder->open_browser.size() == 1);
    CHECK(recorder->open_browser[0].url == "https://example.com");
}
