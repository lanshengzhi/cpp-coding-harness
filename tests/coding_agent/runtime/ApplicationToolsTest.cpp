#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"
#include "support/FakeModelStream.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

void enqueue_call(tests::FakeModelStream& stream, std::string name, support::JsonValue::object_t arguments) {
    auto response = ai::assistant_text_message("");
    response.content.clear();
    response.content.emplace_back(ai::ToolCallContent{
            .id = "call-" + std::to_string(stream.responses.size()),
            .name = std::move(name),
            .arguments = std::move(arguments),
    });
    response.stop_reason = ai::AssistantStopReason::ToolUse;
    stream.responses.push_back(std::move(response));
}

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> create_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::FakeModelStream> stream) {
    std::vector<coding_agent::ModelRuntimeTestProvider> providers;
    providers.push_back({
            .definition = {.id = "fake"},
            .stream =
                    [stream = std::move(stream)](ai::Model model,
                            ai::AiContext context,
                            coding_agent::ModelRuntimeTestStreamOptions options) {
                        return stream->factory()(
                                std::move(model), std::move(context), {.stop_token = options.stop_token});
                    },
    });
    auto models = coding_agent::create_model_runtime_for_testing({}, {.providers = std::move(providers)});
    REQUIRE(models);
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.workspace = workspace.path();
    request.execution_runtime_target = runtime.make_target();
    request.session_target = coding_agent::InMemorySessionTarget{};
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.session_facts.no_context_files = true;
    request.model_runtime = std::move(*models);
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(created);
    return std::move(created->session);
}

[[nodiscard]] std::vector<ai::ToolResultMessage> results(const coding_agent::AgentSession& session) {
    std::vector<ai::ToolResultMessage> collected;
    for (const auto& message : session.snapshot().agent_state.messages) {
        if (const auto* result = std::get_if<ai::ToolResultMessage>(&message)) {
            collected.push_back(*result);
        }
    }
    return collected;
}

} // namespace

TEST_CASE("Application Session tools write edit and read through the assembled local environment",
        "[coding_agent][environment][issue1073][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto stream = std::make_shared<tests::FakeModelStream>();
    workspace.write("anchor.txt", "workspace-marker\n");
    enqueue_call(*stream, "write", {{"path", "note.txt"}, {"content", "before\n"}});
    enqueue_call(*stream, "read", {{"path", "note.txt"}});
    enqueue_call(*stream,
            "edit",
            {{"path", "note.txt"},
                    {"edits",
                            support::JsonValue::array_t{
                                    support::JsonValue::object_t{{"oldText", "before"}, {"newText", "after"}}}}});
    enqueue_call(*stream, "read", {{"path", "note.txt"}});
    enqueue_call(*stream, "bash", {{"command", "cat anchor.txt note.txt; printf 'stderr-output\\n' >&2"}});
    auto& session = runtime.adopt_session(create_session(runtime, workspace, stream));
    REQUIRE(tests::run_awaitable(runtime, session.prompt("exercise the local tools")));
    const auto observed = results(session);
    REQUIRE(observed.size() == 5);
    for (const auto& result : observed) {
        CHECK_FALSE(result.is_error);
    }
    CHECK(ai::text_from_content(observed[1].content) == "before\n");
    CHECK(ai::text_from_content(observed[3].content) == "after\n");
    CHECK(ai::text_from_content(observed[4].content) == "workspace-marker\nafter\nstderr-output\n");
}

TEST_CASE("Application Session tools preserve files after a failed edit and report shell failure",
        "[coding_agent][environment][issue1073][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto stream = std::make_shared<tests::FakeModelStream>();
    enqueue_call(*stream, "write", {{"path", "note.txt"}, {"content", "original\n"}});
    enqueue_call(*stream,
            "edit",
            {{"path", "note.txt"},
                    {"edits",
                            support::JsonValue::array_t{
                                    support::JsonValue::object_t{{"oldText", "missing"}, {"newText", "damaged"}}}}});
    enqueue_call(*stream, "read", {{"path", "note.txt"}});
    enqueue_call(*stream, "bash", {{"command", "printf 'failed-output\\n'; exit 7"}});
    auto& session = runtime.adopt_session(create_session(runtime, workspace, stream));
    REQUIRE(tests::run_awaitable(runtime, session.prompt("exercise tool failures")));
    const auto observed = results(session);
    REQUIRE(observed.size() == 4);
    CHECK_FALSE(observed[0].is_error);
    CHECK(observed[1].is_error);
    CHECK_FALSE(observed[2].is_error);
    CHECK(ai::text_from_content(observed[2].content) == "original\n");
    CHECK(observed[3].is_error);
    CHECK(ai::text_from_content(observed[3].content) == "failed-output\n\n\nCommand exited with code 7");
}

TEST_CASE("Application Session abort cancels a shell that has started in its workspace",
        "[coding_agent][environment][issue1073][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime;
    auto stream = std::make_shared<tests::FakeModelStream>();
    enqueue_call(*stream, "bash", {{"command", "printf 'started' > shell-ready; exec sleep 30"}});
    auto& session = runtime.adopt_session(create_session(runtime, workspace, stream));
    // The Session outlives the one-shot operation; co_spawn owns its closure,
    // and this outer coroutine waits for the prompt before its frame returns.
    const auto cancel_running_shell =
            [&session, marker = workspace.path() / "shell-ready"]() -> boost::asio::awaitable<support::ExpectedVoid> {
        const auto executor = co_await boost::asio::this_coro::executor;
        std::optional<support::ExpectedVoid> prompted;
        boost::asio::co_spawn(
                executor,
                [&session, &prompted]() -> boost::asio::awaitable<void> {
                    prompted = co_await session.prompt("run until cancelled");
                },
                boost::asio::detached);
        boost::asio::steady_timer timer(executor);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        std::error_code error;
        while (!std::filesystem::exists(marker, error) && !prompted && std::chrono::steady_clock::now() < deadline) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
        const bool started = std::filesystem::exists(marker, error);
        session.abort();
        while (!prompted) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
        CHECK(started);
        co_return std::move(*prompted);
    };
    REQUIRE(tests::run_awaitable(runtime, cancel_running_shell()));
    const auto observed = results(session);
    REQUIRE(observed.size() == 1);
    CHECK(observed[0].is_error);
    CHECK(ai::text_from_content(observed[0].content) == "Command aborted");
    CHECK_FALSE(session.is_busy());
}
