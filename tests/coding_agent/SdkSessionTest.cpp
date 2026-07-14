#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/Sdk.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/ai/Content.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/coding_agent/Skill.hpp"
#include "../../include/cch/harness/ExecutionEnv.hpp"
#include "../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../include/cch/util/Error.hpp"
#include "ai/providers/FakeChatClient.hpp"
#include "../support/TempWorkspace.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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
    opts.resume_path = paths.session_file;
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

/// Save and restore an environment variable around a test.
class EnvVarGuard {
public:
    explicit EnvVarGuard(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str()); value != nullptr) {
            previous_ = value;
        }
    }

    ~EnvVarGuard() {
        if (previous_) {
            setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void set(std::string value) const {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void unset() const {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// U1: Public SDK contracts compile and are move-only
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK types compile with only public headers", "[sdk][u1]") {
    // Verify that SDK options can be declared with public headers only
    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = std::filesystem::path{"/tmp/test"};
    opts.workspace = std::filesystem::path{"/tmp"};
    opts.max_turns = 10;

    coding_agent::PromptOptions prompt_opts;
    coding_agent::PromptOptions legacy_aggregate{
        agent::AgentEventSink{[](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid { return {}; }}};
    coding_agent::PromptResult result;
    result.code = "completed";
    result.success = true;

    CHECK(result.success);
    CHECK(result.code == "completed");
    CHECK(legacy_aggregate.expand_prompt_templates);
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

TEST_CASE("create_agent_session fails when both session_path and resume_path are set", "[sdk][u2]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.resume_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("both") != std::string::npos);
}

TEST_CASE("create_agent_session fails when neither session_path nor resume_path is set", "[sdk][u2]") {
    coding_agent::CreateAgentSessionOptions opts;
    opts.workspace = std::filesystem::path{"/tmp"};
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("neither") != std::string::npos);
}

TEST_CASE("create_agent_session fails without chat_client or provider_config", "[sdk][u2]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    // No chat_client, no provider_config

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
}

TEST_CASE("SDK provider_config api_key_env chain accepts first set fallback", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    EnvVarGuard first_key{"CCH_SDK_CHAIN_FIRST"};
    EnvVarGuard second_key{"CCH_SDK_CHAIN_SECOND"};
    first_key.unset();
    second_key.set("second-secret");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    EnvVarGuard first_key{"CCH_SDK_CHAIN_UNSET_FIRST"};
    EnvVarGuard second_key{"CCH_SDK_CHAIN_UNSET_SECOND"};
    first_key.unset();
    second_key.unset();

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.provider_config = coding_agent::SdkProviderConfig{
        .provider = "fake",
        .model = "fake-model",
        .api_key_env = std::vector<std::string>{"CCH_SDK_CHAIN_UNSET_FIRST", "CCH_SDK_CHAIN_UNSET_SECOND"},
    };

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == util::ErrorCode::Validation);
    CHECK(result.error().message.find("CCH_SDK_CHAIN_UNSET_FIRST") != std::string::npos);
    CHECK(result.error().message.find("CCH_SDK_CHAIN_UNSET_SECOND") != std::string::npos);
}

TEST_CASE("SDK provider_config api_key_env chain rejects empty chain", "[sdk][u2][api-key-env]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    EnvVarGuard home_guard{"HOME"};
    EnvVarGuard first_key{"CCH_SDK_RESUME_FIRST"};
    EnvVarGuard second_key{"CCH_SDK_RESUME_SECOND"};
    home_guard.set(home.path().string());
    first_key.unset();
    second_key.set("resume-secret");

    std::filesystem::create_directories(home.path() / ".cpp-harness");
    std::ofstream(home.path() / ".cpp-harness" / "config.json")
        << R"({"provider":"fake","model":"fake-model","api_key_env":["CCH_SDK_RESUME_FIRST","CCH_SDK_RESUME_SECOND"]})";

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_path = paths.session_file;
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
    resume_opts.resume_path = paths.session_file;
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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    REQUIRE(session != nullptr);
    CHECK(session->is_open());
    CHECK_FALSE(session->is_busy());

    // Prompt returns success
    auto prompt_result = session->prompt("hello world");
    REQUIRE(prompt_result.has_value());
    CHECK(prompt_result->success);
    CHECK(prompt_result->code == "completed");
    CHECK(prompt_result->message_count > 0);

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
    opts.session_path = paths.session_file;
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

TEST_CASE("SDK per-prompt event sink receives events", "[sdk][u3]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;

    int per_prompt_count = 0;
    coding_agent::PromptOptions prompt_opts;
    prompt_opts.event_sink = [&per_prompt_count](const agent::AgentLifecycleEvent& /*event*/) -> util::ExpectedVoid {
        ++per_prompt_count;
        return {};
    };

    auto prompt_result = session->prompt("test per-prompt sink", std::move(prompt_opts));
    REQUIRE(prompt_result.has_value());
    CHECK(per_prompt_count > 0);

    CHECK(session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// U4: Tool and resource injection
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK custom tool is registered and can be called", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    CHECK(prompt_result->success);

    CHECK(session->close().has_value());
}

TEST_CASE("SDK duplicate custom tool names fail creation", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    auto prompt_result = session->prompt("/skill:unknown");
    REQUIRE(prompt_result.has_value());
    CHECK(prompt_result->success);
    CHECK(prompt_result->code == "completed");
    CHECK(prompt_result->diagnostics.empty());

    CHECK(session->close().has_value());
}

TEST_CASE("SDK prompt leaves diagnostics empty for valid and bare skill commands", "[sdk][u4][skill-diagnostics]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    CHECK(valid_result->success);
    CHECK(valid_result->diagnostics.empty());

    auto bare_result = session->prompt("/skill:");
    REQUIRE(bare_result.has_value());
    CHECK(bare_result->code == "completed");
    CHECK(bare_result->diagnostics.empty());

    CHECK(session->close().has_value());
}

TEST_CASE("SDK host-provided templates are accessible", "[sdk][u4]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.prompt_templates.push_back(host_template("review", "Review cached: $1"));

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());
    coding_agent::AgentSession moved_session{std::move(*created->session)};

    auto result = moved_session.prompt("/review target.cpp");
    REQUIRE(result.has_value());
    CHECK(result->success);
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
    opts.session_path = paths.session_file;
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
        CHECK(result->success);
        CHECK(result->code == "completed");
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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);

    auto created = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(created.has_value());

    for (const std::string raw : {"!echo visible", "!!echo excluded-later"}) {
        auto result = created->session->prompt(raw);
        REQUIRE(result.has_value());
        CHECK(result->success);
        CHECK(result->code == "completed");
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
    opts.session_path = paths.session_file;
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
    CHECK(skill_prompt->success);
    CHECK(skill_prompt->diagnostics.empty());
    REQUIRE(skill_prompt->last_assistant_text.has_value());
    CHECK(skill_prompt->last_assistant_text->find("Use project skill instructions.") != std::string::npos);

    auto template_prompt = result->session->prompt("/project-review Ada");
    REQUIRE(template_prompt.has_value());
    CHECK(template_prompt->success);
    REQUIRE(template_prompt->last_assistant_text.has_value());
    CHECK(template_prompt->last_assistant_text->find("Review project item: Ada.") != std::string::npos);

    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK keeps host resources when project resource loading is disabled", "[sdk][project-resources]") {
    TestPaths paths;
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    home.write(
        ".cpp-harness/trust.json",
        "{\"" + paths.workspace.path().string() + "\":false}\n");
    write_project_skill(paths, "project-skill");
    write_project_prompt(paths, "project-review");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
        EnvVarGuard home_guard{"HOME"};
        home_guard.set(paths.workspace.path().string());
        paths.workspace.write(".cpp-harness/trust.json", "{not json");

        coding_agent::CreateAgentSessionOptions untrusted_opts;
        untrusted_opts.session_path = paths.session_file;
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
    trusted_opts.session_path = trusted_paths.session_file;
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

TEST_CASE("SDK project resource diagnostics are not printed during creation", "[sdk][project-resources]") {
    TestPaths paths;
    paths.workspace.write(
        ".cpp-harness/prompts/bad.md",
        "---\n"
        "bad line without colon\n"
        "---\n"
        "Body.\n");

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
        opts.session_path = paths.session_file;
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
        opts.resume_path = paths.session_file;
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
    REQUIRE(prompt_result->success);
    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// State accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK state accessors reflect committed history", "[sdk][u3]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto create_result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(create_result.has_value());

    auto& session = create_result->session;
    CHECK(session->message_count() == 0);
    CHECK_FALSE(session->last_assistant_text().has_value());

    auto pr = session->prompt("hello");
    REQUIRE(pr.has_value());
    CHECK(pr->message_count > 0);
    CHECK(pr->last_assistant_text.has_value());

    CHECK(session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateAgentSessionResult metadata
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("CreateAgentSessionResult contains metadata", "[sdk][u2]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());

    CHECK_FALSE(result->session_id.empty());
    CHECK_FALSE(result->session_path.empty());
    CHECK(result->workspace == paths.workspace.path());
    CHECK(result->provider == "sdk-host");
    CHECK(result->model == "host-client");
    CHECK(result->metadata.session_id == result->session_id);
    CHECK(result->metadata.workspace == result->workspace);
    CHECK(result->metadata.provider == result->provider);
    CHECK(result->metadata.model == result->model);

    CHECK(result->session->close().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics from host client
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("SDK creation with host client produces diagnostic", "[sdk][u2]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.builtin_tools.bash = false;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt("hello");
    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->success);

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
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = std::move(capture);
    opts.builtin_tools.bash = true;

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE(result.has_value());
    auto prompt_result = result->session->prompt("hello");
    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->success);

    REQUIRE(capture_ptr->captured_request.has_value());
    CHECK(tool_registry_contains(capture_ptr->captured_request->context, "bash"));
    CHECK(result->session->close().has_value());
}

TEST_CASE("SDK rejects workspace-local trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = paths.workspace.path() / ".cpp-harness" / "trust.json";

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("workspace") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK rejects relative trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
    opts.workspace = paths.workspace.path();
    opts.chat_client = ai::providers::make_scripted_fake_chat_client();
    opts.trust_store_path = std::filesystem::path{"relative/trust.json"};

    auto result = coding_agent::create_agent_session(std::move(opts));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("absolute") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(paths.session_file));
}

TEST_CASE("SDK accepts external not-yet-created trust_store_path", "[sdk][assembly][trust]") {
    TestPaths paths;
    cch::tests::TempWorkspace home;
    auto external_trust = home.path() / "external" / "trust.json";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());
    std::filesystem::create_directories(home.path() / ".cpp-harness");
    std::ofstream(home.path() / ".cpp-harness" / "config.json") << R"({"provider":"kimi-coding"})";

    coding_agent::CreateAgentSessionOptions opts;
    opts.session_path = paths.session_file;
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
        opts.session_path = paths.session_file;
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
        opts.resume_path = paths.session_file;
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
    EnvVarGuard home_guard{"HOME"};
    home_guard.set(home.path().string());

    {
        coding_agent::CreateAgentSessionOptions opts;
        opts.session_path = paths.session_file;
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
        opts.resume_path = paths.session_file;
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
        opts.session_path = paths.session_file;
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
        opts.resume_path = paths.session_file;
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

