#include "../../third_party/catch2/catch_test_macros.hpp"
#include "support/ModelsFixture.hpp"

#include "cli/OneShotCliFrontend.hpp"
#include "cli/TextCliRenderer.hpp"

#include "ai/providers/FakeProvider.hpp"
#include "coding_agent/AgentSession.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextHelpers.hpp"

#include <sstream>
#include <streambuf>
#include <string>
#include <utility>

using namespace cch;

namespace {

class FailingStreambuf final : public std::streambuf {
protected:
    int_type overflow(int_type) override { return traits_type::eof(); }
};

/// A host client whose accepted call streams a partial text delta, terminates
/// with one error/aborted event carrying the final Assistant Message, and
/// returns that same message as a successful C++ value.
class TerminalOutcomeChatProvider final : public tests::ScriptedProvider {
public:
    TerminalOutcomeChatProvider(
        ai::AssistantStopReason reason,
        std::string diagnostic,
        std::string partial = "partial draft")
        : ScriptedProvider("sdk-host"),
          reason_(reason),
          diagnostic_(std::move(diagnostic)),
          partial_(std::move(partial)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext&,
        ai::ProviderStreamOptions,
        ai::AssistantEventSink sink) override {
        ++request_count;
        ai::AssistantMessage terminal;
        terminal.provider = "host";
        terminal.api = "host";
        terminal.model = model.id;
        terminal.stop_reason = reason_;
        terminal.error_message = diagnostic_;
        terminal.content.emplace_back(ai::text_content(partial_));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{terminal}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto delta = sink(ai::TextDeltaEvent{0, partial_, terminal}); !delta) {
                co_return std::unexpected(delta.error());
            }
            if (auto ended = sink(ai::AssistantErrorEvent{reason_, terminal}); !ended) {
                co_return std::unexpected(ended.error());
            }
        }
        co_return terminal;
    }

    int request_count{0};

private:
    ai::AssistantStopReason reason_;
    std::string diagnostic_;
    std::string partial_;
};

class CapturingChatProvider final : public tests::ScriptedProvider {
public:
    CapturingChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::Model& model,
        const ai::AiContext& context,
        ai::ProviderStreamOptions,
        ai::AssistantEventSink sink) override {
        messages = context.messages;
        ai::AssistantMessage message;
        message.provider = "host";
        message.api = "host";
        message.model = model.id;
        message.stop_reason = ai::AssistantStopReason::Stop;
        message.content.emplace_back(ai::text_content("captured"));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{message}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto done = sink(ai::AssistantDoneEvent{message.stop_reason, message}); !done) {
                co_return std::unexpected(done.error());
            }
        }
        co_return message;
    }

    std::vector<ai::MessageVariant> messages;
};

tests::ModelsSessionOptions fake_in_memory_options(
    const std::filesystem::path& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace;
    options.models = ai::providers::make_scripted_fake_models();
    return options;
}

struct CreatedSession {
    coding_agent::CreateAgentSessionResult created;
};

CreatedSession make_session(const tests::TempWorkspace& workspace) {
    auto created = coding_agent::create_agent_session(fake_in_memory_options(workspace.path()));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

CreatedSession make_session(
    const tests::TempWorkspace& workspace,
    std::shared_ptr<ai::Provider> chat_client) {
    auto options = fake_in_memory_options(workspace.path());
    options.models = cch::tests::models_from_provider(std::move(chat_client));
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

} // namespace

TEST_CASE(
    "one-shot frontend exits 2 when event subscription fails",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);
    session.created.session->close();

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::StartupFailure);
    CHECK(error.str().find("could not subscribe event renderer: ") != std::string::npos);
}

TEST_CASE(
    "one-shot text frontend reports a /clear stream failure",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    FailingStreambuf failing;
    std::ostream broken_output{&failing};
    std::istringstream input;
    std::ostringstream error;
    cli::TextCliRenderer renderer{broken_output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = broken_output, .error = error,
            .prompt = "/clear"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::RuntimeError);
    CHECK(error.str().find("failed to clear terminal") != std::string::npos);
}

TEST_CASE(
    "one-shot text frontend renders one prompt through the event subscription",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(output.str().find("fake: hello") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "one-shot frontend sends initial CLI images after the complete text block",
    "[cli][frontend][issue63]") {
    tests::TempWorkspace workspace;
    auto client = std::make_shared<CapturingChatProvider>();
    auto* probe = client.get();
    auto session = make_session(workspace, std::move(client));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    coding_agent::PromptOptions options;
    options.images = {
        ai::ImageContent{.data = "first-data", .mime_type = "image/png"},
        ai::ImageContent{.data = "second-data", .mime_type = "image/webp"},
    };
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "<file name=\"/tmp/first\"></file>\ndescribe"},
        std::move(options)};

    REQUIRE(frontend.run() == cli::OneShotCliOutcome::Success);
    REQUIRE_FALSE(probe->messages.empty());
    const auto* user = std::get_if<ai::UserMessage>(&probe->messages.back());
    REQUIRE(user != nullptr);
    const auto& user_blocks = std::get<std::vector<ai::Content>>(user->content);
    REQUIRE(user_blocks.size() == 3);
    CHECK(std::get<ai::TextContent>(user_blocks[0]).text ==
        "<file name=\"/tmp/first\"></file>\ndescribe");
    CHECK(std::get<ai::ImageContent>(user_blocks[1]).data == "first-data");
    CHECK(std::get<ai::ImageContent>(user_blocks[2]).data == "second-data");

    const auto snapshot = session.created.session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() >= 2);
    const auto* persisted_user = std::get_if<ai::UserMessage>(
        &snapshot.agent_state.messages.front());
    REQUIRE(persisted_user != nullptr);
    const auto& persisted_blocks =
        std::get<std::vector<ai::Content>>(persisted_user->content);
    REQUIRE(persisted_blocks.size() == 3);
    CHECK(std::get<ai::TextContent>(persisted_blocks[0]).text ==
        std::get<ai::TextContent>(user_blocks[0]).text);
    CHECK(std::get<ai::ImageContent>(persisted_blocks[1]).data == "first-data");
    CHECK(std::get<ai::ImageContent>(persisted_blocks[2]).mime_type == "image/webp");
}

TEST_CASE(
    "one-shot text frontend presents an accepted error terminal outcome once",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error, "host transport lost"));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(output.str().find("[assistant] partial draft") != std::string::npos);
    CHECK(tests::count_occurrences(output.str(), "[error] host transport lost") == 1);
    CHECK(output.str().find("[completed]") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "one-shot text frontend presents an accepted aborted terminal outcome once",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Aborted, "host cancelled the request"));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(tests::count_occurrences(output.str(), "[aborted] host cancelled the request") == 1);
    CHECK(output.str().find("[completed]") != std::string::npos);
    CHECK(output.str().find("[error]") == std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "one-shot text frontend redacts and bounds terminal diagnostics",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-hostsecret123456789";
    std::string diagnostic = "provider rejected " + secret + " detail ";
    diagnostic += std::string(9000, 'x');
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error, diagnostic));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(output.str().find(secret) == std::string::npos);
    CHECK(output.str().find("[REDACTED]") != std::string::npos);
    // The presented diagnostic stays inside the bounded-output budget even
    // though the raw provider diagnostic far exceeds it.
    CHECK(output.str().size() < diagnostic.size());
    CHECK(error.str().empty());
}

TEST_CASE(
    "one-shot text frontend redacts and bounds partial terminal output",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-partialsecret123456789";
    std::string partial = "draft contains " + secret + " ";
    partial += std::string(9000, 'x');
    auto session = make_session(
        workspace,
        std::make_shared<TerminalOutcomeChatProvider>(
            ai::AssistantStopReason::Error,
            "transport failed",
            partial));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(output.str().find(secret) == std::string::npos);
    CHECK(output.str().find("[assistant] draft contains [REDACTED]") != std::string::npos);
    CHECK(output.str().size() < partial.size());
    CHECK(tests::count_occurrences(output.str(), "[error] transport failed") == 1);
    CHECK(error.str().empty());
}

TEST_CASE(
    "one-shot text frontend continues after a weak subscriber failure",
    "[cli][frontend][issue36]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    auto rejecting = session.created.session->subscribe(
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "synthetic subscriber failure"));
            }
            return {};
        });
    REQUIRE(rejecting.has_value());

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::OneShotCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::OneShotCliFrontendConfig{
            .output = output, .error = error,
            .prompt = "hello"}};

    CHECK(frontend.run() == cli::OneShotCliOutcome::Success);
    CHECK(error.str().empty());
    CHECK(output.str().find("[model-request]") != std::string::npos);
    CHECK(output.str().find("[assistant]") != std::string::npos);
    CHECK(output.str().find("[completed]") != std::string::npos);
    CHECK_FALSE(static_cast<bool>(*rejecting));
}

