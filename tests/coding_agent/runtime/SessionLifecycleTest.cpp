#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"
#include "ai/providers/FakeProvider.hpp"

#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include "agent/harness/session/SessionJournalTestHooks.hpp"
#include "support/ModelsFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

harness::session::SessionMetadata test_metadata(const tests::TempWorkspace& workspace) {
    return {
        .session_id = "session-lifecycle-test",
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

std::string user_text_at(const std::vector<ai::MessageVariant>& messages, std::size_t index) {
    REQUIRE(index < messages.size());
    const auto* user = std::get_if<ai::UserMessage>(&messages[index]);
    REQUIRE(user != nullptr);
    return ai::text_from_user_message(*user);
}

support::Expected<runtime::OpenSession> open_resumed_session(
    const std::filesystem::path& path,
    const tests::TempWorkspace& workspace) {
    auto prepared = runtime::prepare_resume_target(path, workspace.path(), true);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    return runtime::publish_resume_session(*prepared);
}

} // namespace

TEST_CASE("resumed session uses tree context for linear sessions", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "linear.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    CHECK(opened->metadata.session_id == "session-lifecycle-test");
    CHECK(opened->metadata.workspace == workspace.path());
    CHECK(opened->metadata.provider == "fake");
    CHECK(opened->metadata.model == "fake-model");
    REQUIRE(opened->history.size() == 2);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
    CHECK_FALSE(opened->context_model.has_value());
    CHECK_FALSE(opened->context_thinking_level.has_value());
}

TEST_CASE("resumed session uses compaction tree context", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "compact.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("msg1")));
    REQUIRE(store->append(user_msg("msg2")));
    REQUIRE(store->append(user_msg("msg3")));

    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto msg3_id = pre->entries[3].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_compaction(std::nullopt, "summary of msg1-2", msg3_id, 1000, std::nullopt, std::nullopt));
    REQUIRE(resumed->append(user_msg("msg4")));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Compacted);
    REQUIRE(opened->history.size() == 3);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(opened->history[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(opened->history[0]).summary == "summary of msg1-2");
    CHECK(user_text_at(opened->history, 1) == "msg3");
    CHECK(user_text_at(opened->history, 2) == "msg4");
}

TEST_CASE("resumed session uses active leaf path", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "active-leaf.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Branched);
    REQUIRE(opened->history.size() == 1);
    CHECK(user_text_at(opened->history, 0) == "first");
}

TEST_CASE("AgentSession prompt after leaf resume becomes the next resume point", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-continuation.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed_store = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed_store);
    REQUIRE(resumed_store->append_leaf(std::nullopt, first_id));

    runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::ExplicitResumeSessionTarget{path};
    request.execution_runtime_target = tests::detail::fixture_runtime_target();

    auto session_result = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(session_result);
    auto prompt_result = session_result->session->prompt_blocking("continue branch");
    REQUIRE(prompt_result);
    session_result->session->close();

    auto reopened = open_resumed_session(path, workspace);
    REQUIRE(reopened);
    CHECK(reopened->topology == harness::session::SessionTopology::Branched);
    REQUIRE(reopened->history.size() == 3);
    CHECK(user_text_at(reopened->history, 0) == "first");
    CHECK(user_text_at(reopened->history, 1) == "continue branch");
    const auto* assistant = std::get_if<ai::AssistantMessage>(&reopened->history[2]);
    REQUIRE(assistant != nullptr);
    CHECK(ai::text_from_assistant_content(assistant->content) == "fake: continue branch");
}

TEST_CASE(
    "resumed AgentSession recovers when the message write succeeds but its leaf write fails",
    "[coding-agent][runtime][session][persistence-failure]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-partial-append.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("inactive second")));
    REQUIRE(store->append(user_msg("inactive third")));

    auto loaded_before_resume = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded_before_resume);
    REQUIRE(loaded_before_resume->entries.size() >= 4);
    const auto first_id = loaded_before_resume->entries[1].entry_id;

    auto resumed_store = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed_store);
    REQUIRE(resumed_store->append_leaf(std::nullopt, first_id));

    runtime::AgentSessionCreationRequest request;
    request.no_skills = true;
    request.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::ExplicitResumeSessionTarget{path};
    request.execution_runtime_target = tests::detail::fixture_runtime_target();

    auto session_result = coding_agent::create_agent_session_for_testing(
        std::move(request), ai::providers::make_scripted_fake_models());
    REQUIRE(session_result);
    auto& session = session_result->session;

    // A resumed branch message append writes the message and then its active
    // leaf marker. Fail only the second physical write: the user message
    // lands but its leaf marker does not, so the append reports a persistence
    // failure after the message was already admitted.
    harness::session::testing::fail_nth_append_for_test(path, 2);
    auto failed = session->prompt_blocking("continue after partial write");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == support::ErrorCode::Session);
    CHECK(failed.error().message == "could not persist session entry");
    CHECK(session->is_open());
    // No rollback theatre: the admitted user and assistant messages stay in
    // live Session state (admission is asynchronous; there is no mid-flight
    // veto).
    CHECK(session->message_count() == 3);

    // Reopening follows the durable message even though its leaf marker was
    // the failed physical write. Whether the assistant reply settled before
    // the failure latched is timing-dependent, so the durable history is the
    // user message plus at most the assistant reply.
    auto after_failure = open_resumed_session(path, workspace);
    REQUIRE(after_failure);
    REQUIRE(after_failure->history.size() >= 2);
    REQUIRE(after_failure->history.size() <= 3);
    CHECK(user_text_at(after_failure->history, 0) == "first");
    CHECK(user_text_at(after_failure->history, 1) == "continue after partial write");

    // The recorded failure is session-scoped sticky state (ADR 0040): a later
    // prompt is rejected with the typed persistence failure instead of
    // recovering.
    auto recovered = session->prompt_blocking("recover branch");
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == support::ErrorCode::Session);
    CHECK(
        recovered.error().message ==
        "session persistence failed; rejecting new prompt");
    CHECK(session->message_count() == 3);
    session->close();
}

TEST_CASE("resumed session ignores invalid leaf target", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "invalid-leaf.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, "missing-entry"));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 2);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
}

TEST_CASE("resumed session topology follows active path only", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "inactive-tree-data.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;
    const auto third_id = pre->entries[3].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_branch_summary(first_id, third_id, "inactive branch", std::nullopt, std::nullopt));
    REQUIRE(resumed->append_compaction(first_id, "inactive compaction", third_id, 1000, std::nullopt, std::nullopt));
    REQUIRE(resumed->append_leaf(std::nullopt, third_id));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 3);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
    CHECK(user_text_at(opened->history, 2) == "third");
}

TEST_CASE("resumed session carries effective model and thinking level", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "context-state.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append(user_msg("hello")));

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    CHECK(opened->context_model == "gpt-4o");
    CHECK(opened->context_thinking_level == "high");
    REQUIRE(opened->history.size() == 1);
    CHECK(user_text_at(opened->history, 0) == "hello");
}
