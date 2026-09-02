#include "ai/ModelStreamBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"

#include <cch/agent/AgentEvent.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/agent/harness/session/SessionStore.hpp>

#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

using namespace cch;

namespace {

struct TestPaths {
    tests::TempWorkspace workspace;
    std::filesystem::path session_file{workspace.path() / "snapshot-session.jsonl"};
};

[[nodiscard]] harness::session::SessionMetadata test_metadata(const TestPaths& paths) {
    return {
        .session_id = "snapshot-session",
        .created_at = "2026-07-10T00:00:00Z",
        .workspace = paths.workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

[[nodiscard]] ai::MessageVariant user_message(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

[[nodiscard]] tests::ModelsSessionOptions new_session_options(const TestPaths& paths,
        coding_agent::SessionTarget target,
        std::shared_ptr<tests::ScriptedProvider> client = tests::make_scripted_fake_provider()) {
    tests::ModelsSessionOptions options;
    options.session_target = std::move(target);
    options.workspace = paths.workspace.path();
    options.request_model = cch::tests::scripted_request_model("fake", "fake-model");
    options.models = cch::tests::models_from_provider(std::move(client));
    return options;
}

template <typename T>
[[nodiscard]] T run_awaitable(tests::RuntimeFixture& runtime, boost::asio::awaitable<T> awaitable) {
    return runtime.run(support::detail::make_async_result(
            [awaitable = std::move(awaitable)]() mutable -> boost::asio::awaitable<T> {
                co_return co_await std::move(awaitable);
            }));
}

[[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult> resume_for_frontend(
        const TestPaths& paths, tests::RuntimeFixture& runtime) {
    coding_agent::runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = paths.workspace.path();
    request.session_target = coding_agent::ExplicitResumeSessionTarget{paths.session_file};
    request.execution_runtime_target = runtime.make_target();
    return runtime.run(coding_agent::create_agent_session_async(std::move(request),
            std::nullopt,
            coding_agent::runtime::AssemblyOverrides{.models = tests::make_scripted_fake_models()}));
}

class GatedSnapshotChatProvider final : public tests::ScriptedProvider {
public:
    GatedSnapshotChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        return ai::detail::make_model_stream(
            [this, model = std::move(model)](
                ai::AssistantEventSink sink) mutable
                -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        auto partial = ai::assistant_text_message("");
        partial.provider = "snapshot-fake";
        partial.api = "fake";
        partial.model = model.id;
        partial.content.clear();
        if (sink) {
            if (auto emitted = sink(ai::AssistantStartEvent{partial}); !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }

        auto executor = co_await boost::asio::this_coro::executor;
        gate_.emplace(executor);
        gate_->expires_at(std::chrono::steady_clock::time_point::max());
        started = true;

        boost::system::error_code error;
        co_await gate_->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));

        auto response = ai::assistant_text_message("released");
        response.provider = "snapshot-fake";
        response.api = "fake";
        response.model = model.id;
        co_return response;
                });
    }

    void release() {
        if (gate_) {
            gate_->cancel();
        }
    }

    bool started{false};

private:
    std::optional<boost::asio::steady_timer> gate_;
};

class CapturingSnapshotChatProvider final : public tests::ScriptedProvider {
public:
    CapturingSnapshotChatProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        return ai::detail::make_model_stream(
                [model = std::move(model)](ai::AssistantEventSink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    auto response = ai::assistant_text_message("captured");
                    response.provider = "snapshot-fake";
                    response.api = "fake";
                    response.model = model.id;
                    co_return response;
                });
    }
};

} // namespace

TEST_CASE(
    "SDK fresh persisted snapshot is passive session and Agent state",
    "[sdk][snapshot][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto options = new_session_options(paths, coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file});
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, coding_agent::runtime::AssemblyOverrides{.models = std::move(models)}));
    REQUIRE(created.has_value());

    auto snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.messages.empty());
    CHECK_FALSE(snapshot.agent_state.is_running);
    CHECK(snapshot.agent_state.model.id == "fake-model");
    CHECK(snapshot.agent_state.thinking_level == "off");
    CHECK(snapshot.agent_state.active_tool_names.size() == 4);
    CHECK(snapshot.metadata.session_id == created->resolved_identity.session_id);
    CHECK(snapshot.metadata.workspace == paths.workspace.path());
    CHECK(snapshot.topology == harness::session::SessionTopology::Linear);
    REQUIRE(snapshot.session_path.has_value());
    CHECK(*snapshot.session_path == paths.session_file);

    snapshot.agent_state.messages.push_back(user_message("mutated copy"));
    snapshot.agent_state.active_tool_names.clear();
    snapshot.metadata.session_id = "mutated copy";
    snapshot.session_path.reset();

    const auto unchanged = created->session->snapshot();
    CHECK(unchanged.agent_state.messages.empty());
    CHECK(unchanged.agent_state.active_tool_names.size() == 4);
    CHECK(unchanged.metadata.session_id == created->resolved_identity.session_id);
    CHECK(unchanged.session_path.has_value());
    created->session->close();
}

TEST_CASE(
    "SDK in-memory snapshot retains metadata without inventing a session file",
    "[sdk][snapshot][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto options = new_session_options(paths, coding_agent::InMemorySessionTarget{});
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, coding_agent::runtime::AssemblyOverrides{.models = std::move(models)}));
    REQUIRE(created.has_value());

    const auto snapshot = created->session->snapshot();
    CHECK(snapshot.metadata.session_id == created->resolved_identity.session_id);
    CHECK(snapshot.metadata.provider == "fake");
    CHECK(snapshot.metadata.model == "fake-model");
    CHECK(snapshot.metadata.workspace == paths.workspace.path());
    CHECK_FALSE(snapshot.session_path.has_value());
    CHECK(snapshot.topology == harness::session::SessionTopology::Linear);
    CHECK(snapshot.agent_state.messages.empty());
    created->session->close();
}

TEST_CASE(
    "SDK active snapshot copies running and streaming state on the prompt executor",
    "[sdk][snapshot][async][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto client = std::make_shared<GatedSnapshotChatProvider>();
    auto* client_ptr = client.get(); // options/session owns it until this test closes the session

    auto options = new_session_options(paths, coding_agent::InMemorySessionTarget{}, std::move(client));
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, coding_agent::runtime::AssemblyOverrides{.models = std::move(models)}));
    REQUIRE(created.has_value());

    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> prompt_result;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            prompt_result = co_await created->session->prompt("active snapshot");
            co_return;
        },
        boost::asio::detached);

    while (!client_ptr->started) {
        REQUIRE(io.poll_one() == 1);
    }

    const auto active = created->session->snapshot();
    CHECK(active.agent_state.is_running);
    REQUIRE(active.agent_state.messages.size() == 1);
    CHECK(std::holds_alternative<ai::UserMessage>(active.agent_state.messages[0]));
    REQUIRE(active.agent_state.streaming_message.has_value());
    CHECK(active.agent_state.streaming_message->model == "fake-model");
    CHECK(active.agent_state.model.id == "fake-model");
    CHECK(active.agent_state.thinking_level == "off");
    CHECK(active.agent_state.active_tool_names.size() == 4);
    CHECK(active.agent_state.pending_tool_call_ids.empty());

    client_ptr->release();
    if (io.stopped()) {
        io.restart();
    }
    io.run();
    REQUIRE(prompt_result.has_value());
    CHECK(prompt_result->has_value());
    created->session->close();
}

TEST_CASE(
    "SDK snapshot retains live messages and diagnostics after subscriber failure",
    "[sdk][snapshot][subscriber-failure][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto options = new_session_options(paths, coding_agent::InMemorySessionTarget{});
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, coding_agent::runtime::AssemblyOverrides{.models = std::move(models)}));
    REQUIRE(created.has_value());

    auto failing = created->session->subscribe(
        [](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid {
            return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "snapshot subscriber failed"));
        });
    REQUIRE(failing.has_value());
    REQUIRE(run_awaitable(runtime, created->session->prompt("subscriber failure")).has_value());

    const auto snapshot = created->session->snapshot();
    CHECK(snapshot.agent_state.messages.size() == 2);
    REQUIRE(snapshot.agent_state.diagnostics.size() == 1);
    CHECK(snapshot.agent_state.diagnostics[0].message == "agent event observer failed");
    CHECK(snapshot.agent_state.diagnostics[0].detail.find("snapshot subscriber failed") !=
          std::string::npos);
    CHECK_FALSE(static_cast<bool>(*failing));
    created->session->close();
}

TEST_CASE(
    "SDK snapshot retains Live Session State after persistence failure",
    "[sdk][snapshot][persistence-failure][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitOpenOrCreateSessionTarget{paths.session_file};
    options.workspace = paths.workspace.path();
    options.models = cch::tests::models_from_provider(std::make_shared<CapturingSnapshotChatProvider>());
    auto models = std::move(options.models);
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, coding_agent::runtime::AssemblyOverrides{.models = std::move(models)}));
    REQUIRE(created.has_value());

    harness::session::testing::fail_nth_append_for_test(paths.session_file, 2);
    auto prompted = run_awaitable(runtime, created->session->prompt("persistence failure"));
    REQUIRE_FALSE(prompted.has_value());

    const auto snapshot = created->session->snapshot();
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[0]));
    CHECK(std::holds_alternative<ai::AssistantMessage>(snapshot.agent_state.messages[1]));

    auto durable = harness::session::SessionStore::load(paths.session_file);
    REQUIRE(durable.has_value());
    CHECK(durable->messages.size() == 1);
    created->session->close();
}

TEST_CASE(
    "Frontend resumed snapshot reflects compacted active-path context",
    "[sdk][snapshot][resume][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto store = harness::session::SessionStore::create_new(
        paths.session_file,
        test_metadata(paths));
    REQUIRE(store.has_value());
    REQUIRE(store->append(user_message("before compaction")).has_value());
    REQUIRE(store->append(user_message("kept")).has_value());

    auto loaded = harness::session::SessionStore::load(paths.session_file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() >= 3);
    const auto kept_id = loaded->entries[2].entry_id;

    auto resumed_store = harness::session::SessionStore::open_existing(paths.session_file);
    REQUIRE(resumed_store.has_value());
    REQUIRE(resumed_store->append_compaction(
        std::nullopt,
        harness::session::CompactionEntryValue{
            .summary = "summary of prior work",
            .first_kept_entry_id = kept_id,
            .tokens_before = 1000,
        }));
    REQUIRE(resumed_store->append(user_message("after compaction")).has_value());

    auto resumed = resume_for_frontend(paths, runtime);
    REQUIRE(resumed.has_value());
    const auto snapshot = resumed->session->snapshot();
    CHECK(snapshot.topology == harness::session::SessionTopology::Compacted);
    CHECK(snapshot.metadata.session_id == "snapshot-session");
    REQUIRE(snapshot.agent_state.messages.size() == 3);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(
        snapshot.agent_state.messages[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(snapshot.agent_state.messages[0]).summary ==
          "summary of prior work");
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[1]));
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[2]));
    resumed->session->close();
}

TEST_CASE(
    "Frontend resumed snapshot preserves branch-summary active-path meaning",
    "[sdk][snapshot][resume][issue42]") {
    TestPaths paths;
    tests::RuntimeFixture runtime;
    auto store = harness::session::SessionStore::create_new(
        paths.session_file,
        test_metadata(paths));
    REQUIRE(store.has_value());
    REQUIRE(store->append(user_message("branch root")).has_value());
    REQUIRE(store->append_branch_summary(
        std::nullopt,
        "abandoned-leaf",
        "summary of abandoned branch",
        std::nullopt,
        std::nullopt));

    auto resumed = resume_for_frontend(paths, runtime);
    REQUIRE(resumed.has_value());
    const auto snapshot = resumed->session->snapshot();
    CHECK(snapshot.topology == harness::session::SessionTopology::Branched);
    REQUIRE(snapshot.agent_state.messages.size() == 2);
    CHECK(std::holds_alternative<ai::UserMessage>(snapshot.agent_state.messages[0]));
    REQUIRE(std::holds_alternative<ai::BranchSummaryMessage>(
        snapshot.agent_state.messages[1]));
    CHECK(std::get<ai::BranchSummaryMessage>(snapshot.agent_state.messages[1]).summary ==
          "summary of abandoned branch");
    resumed->session->close();
}
