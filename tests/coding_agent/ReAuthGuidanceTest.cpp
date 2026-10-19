#include <cch/ai/Models.hpp>

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

using namespace cch;

namespace {

class AuthTerminalProvider final : public tests::ScriptedProvider {
public:
    AuthTerminalProvider(support::ErrorCode code,
                         std::string message,
                         std::string content = {})
        : ScriptedProvider("sdk-host"),
          code_(code),
          message_(std::move(message)),
          content_(std::move(content)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        ++request_count;
        auto terminal = ai::assistant_text_message(content_);
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = message_;
        terminal.provider = "sdk-host";
        terminal.api = "fake";
        terminal.model = model.id;
        // Session files require real epoch timestamps on assistant messages.
        terminal.timestamp = 1718000000123;
        if (sink) {
            CCH_TRY_VOID(sink(ai::AssistantErrorEvent{
                .reason = terminal.stop_reason,
                .error = terminal,
                .failure = support::make_error(code_, message_),
            }));
        }
        co_return terminal;
                });
    }

    int request_count{0};

private:
    support::ErrorCode code_;
    std::string message_;
    std::string content_;
};

/// Session creation against the scripted-client seam (a real ModelRuntime
/// composed over scripted providers, ADR 0034 "primary seam — Agent +
/// session-assembly composition").
[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> create_scripted_session(
        tests::RuntimeFixture& runtime,
        std::shared_ptr<tests::ScriptedProvider> client,
        const std::filesystem::path& session_file,
        const std::filesystem::path& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target =
        coding_agent::ExplicitOpenOrCreateSessionTarget{session_file};
    options.workspace = workspace;
    options.request_model = cch::tests::scripted_request_model("sdk-host", "sdk-model");
    options.execution_runtime_target = runtime.make_target();
    // The scripted provider catalog crosses the assembly seam as an explicit
    // override: slicing ModelsSessionOptions into the base request would
    // silently drop it (the one-argument overload cannot recover it).
    auto models = cch::tests::models_from_provider(std::move(client));
    return runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, cch::tests::cli_fake_overrides(std::move(models))));
}

} // namespace

TEST_CASE("Session preserves gateway authentication diagnostics without model login guidance",
        "[coding_agent][gateway][auth]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    const std::string diagnostic = "Gateway rejected the invocation key";
    auto client = std::make_shared<AuthTerminalProvider>(support::ErrorCode::Auth, diagnostic);
    auto created = create_scripted_session(runtime, client, workspace.path() / "session.jsonl", workspace.path());
    REQUIRE(created);
    tests::RuntimeLoopDriver driver(runtime);
    auto* session = created->session.get();
    REQUIRE(session->prompt_blocking("hello"));
    CHECK(client->request_count == 1);
    const auto snapshot = session->snapshot();
    REQUIRE_FALSE(snapshot.agent_state.messages.empty());
    const auto* terminal = std::get_if<ai::AssistantMessage>(&snapshot.agent_state.messages.back());
    REQUIRE(terminal);
    CHECK(terminal->error_message == diagnostic);
    CHECK(terminal->stop_reason == ai::AssistantStopReason::Error);
    session->close();
}
