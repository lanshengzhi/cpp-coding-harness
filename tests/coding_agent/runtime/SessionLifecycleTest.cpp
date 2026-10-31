#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <cch/coding_agent/session/SessionStore.hpp>
#include "coding_agent/session/SessionJournalTestHooks.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

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

/// `session` must outlive `runtime.run`; the one-shot factory captures its
/// reference while the prompt settles on the fixture's serialized loop.
[[nodiscard]] support::ExpectedVoid run_prompt(
        tests::RuntimeFixture& runtime_fixture, coding_agent::AgentSession& session, std::string text) {
    return runtime_fixture.run(support::detail::make_async_result(
            [&session, text = std::move(text)]() -> boost::asio::awaitable<support::ExpectedVoid> {
                co_return co_await session.prompt(std::move(text));
            }));
}

} // namespace

TEST_CASE("resumed session uses tree context for linear sessions", "[coding_agent][runtime][session][spec]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "linear.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("second")).has_value());

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

TEST_CASE("resumed session uses compaction tree context", "[coding_agent][runtime][session][spec]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "compact.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("msg1")).has_value());
    REQUIRE(store->append(user_msg("msg2")).has_value());
    REQUIRE(store->append(user_msg("msg3")).has_value());

    auto pre = harness::session::SessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto msg3_id = pre->entries[3].entry_id;

    auto resumed = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_compaction(
        std::nullopt,
        harness::session::CompactionEntryValue{
            .summary = "summary of msg1-2",
            .first_kept_entry_id = msg3_id,
            .tokens_before = 1000,
        }));
    REQUIRE(resumed->append(user_msg("msg4")).has_value());

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Compacted);
    REQUIRE(opened->history.size() == 3);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(opened->history[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(opened->history[0]).summary == "summary of msg1-2");
    CHECK(user_text_at(opened->history, 1) == "msg3");
    CHECK(user_text_at(opened->history, 2) == "msg4");
}

TEST_CASE("resumed session uses active leaf path", "[coding_agent][runtime][session][spec]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "active-leaf.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("second")).has_value());
    REQUIRE(store->append(user_msg("third")).has_value());

    auto pre = harness::session::SessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed);
    // pi `branch`: the active leaf moves to an existing entry. The leaf is
    // runtime-only state (never written), so the file keeps entries only.
    REQUIRE(resumed->branch(first_id).has_value());
    CHECK(resumed->leaf_id() == first_id);

    // The next append hangs under the branch target (pi `appendMessage`
    // hangs the message under the current leaf), so the resumed session's
    // durable history follows the branch.
    REQUIRE(resumed->append(user_msg("branched")).has_value());

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Branched);
    REQUIRE(opened->history.size() == 2);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "branched");
}

TEST_CASE("AgentSession prompt after a branch append becomes the next resume point",
        "[coding_agent][runtime][session][spec]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime_fixture;
    auto path = workspace.path() / "leaf-continuation.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("second")).has_value());
    REQUIRE(store->append(user_msg("third")).has_value());

    auto pre = harness::session::SessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;

    auto resumed_store = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed_store);
    // pi `branch`: the active leaf moves to an existing entry, so the next
    // append hangs under it (pi `appendMessage`). The leaf itself is
    // runtime-only state and is never written to the session file.
    REQUIRE(resumed_store->branch(first_id).has_value());
    REQUIRE(resumed_store->append(user_msg("branched")).has_value());

    runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::ExplicitResumeSessionTarget{path};
    request.execution_runtime_target = runtime_fixture.make_target();

    auto model_runtime = tests::scripted_fake_runtime();
    REQUIRE(model_runtime);
    request.model_runtime = std::move(*model_runtime);
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto session_result =
            runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(session_result);
    auto& session = runtime_fixture.adopt_session(std::move(session_result->session));
    auto prompt_result = run_prompt(runtime_fixture, session, "continue branch");
    REQUIRE(prompt_result);
    session.close();

    auto reopened = open_resumed_session(path, workspace);
    REQUIRE(reopened);
    CHECK(reopened->topology == harness::session::SessionTopology::Branched);
    REQUIRE(reopened->history.size() == 4);
    CHECK(user_text_at(reopened->history, 0) == "first");
    CHECK(user_text_at(reopened->history, 1) == "branched");
    CHECK(user_text_at(reopened->history, 2) == "continue branch");
    const auto* assistant = std::get_if<ai::AssistantMessage>(&reopened->history[3]);
    REQUIRE(assistant != nullptr);
    CHECK(ai::text_from_assistant_content(assistant->content) == "fake: continue branch");
}

TEST_CASE("resumed AgentSession recovers when the message write succeeds but the reply write fails",
        "[coding_agent][runtime][session][persistence-failure][spec][issue1044]") {
    tests::TempWorkspace workspace;
    tests::RuntimeFixture runtime_fixture;
    auto path = workspace.path() / "leaf-partial-append.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("inactive second")).has_value());
    REQUIRE(store->append(user_msg("inactive third")).has_value());

    auto loaded_before_resume = harness::session::SessionStore::load(path);
    REQUIRE(loaded_before_resume);
    REQUIRE(loaded_before_resume->entries.size() >= 4);
    const auto first_id = loaded_before_resume->entries[1].entry_id;

    auto resumed_store = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed_store);
    // pi `branch`: the active leaf moves to an existing entry, so the next
    // append hangs under it. The leaf itself is runtime-only state and is
    // never written to the session file — the branch is durable through the
    // next append's explicit `parentId`, exactly like pi's wire.
    REQUIRE(resumed_store->branch(first_id).has_value());
    REQUIRE(resumed_store->append(user_msg("branched")).has_value());

    runtime::AgentSessionCreationRequest request;
    request.session_facts.no_skills = true;
    request.session_facts.no_prompt_templates = true;
    request.workspace = workspace.path();
    request.session_target = coding_agent::ExplicitResumeSessionTarget{path};
    request.execution_runtime_target = runtime_fixture.make_target();

    auto model_runtime = tests::scripted_fake_runtime();
    REQUIRE(model_runtime);
    request.model_runtime = std::move(*model_runtime);
    request.request_model = tests::scripted_request_model("fake", "fake-model");
    auto session_result =
            runtime_fixture.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
    REQUIRE(session_result);
    auto& session = runtime_fixture.adopt_session(std::move(session_result->session));

    // A prompt writes the user message and then the assistant reply. Fail
    // only the second physical write: the user message lands but the reply
    // does not, so the append reports a persistence failure after the message
    // was already admitted.
    harness::session::testing::fail_nth_append_for_test(path, 2);
    auto failed = run_prompt(runtime_fixture, session, "continue after partial write");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == support::ErrorCode::Session);
    CHECK(failed.error().message == "could not persist session entry");
    CHECK(session.is_open());
    // No rollback theatre: the admitted user and assistant messages stay in
    // live Session state (admission is asynchronous; there is no mid-flight
    // veto). The resumed history is `first` + `branched`; the run declares the
    // executable loadout as a system message before its own prompt, then the
    // user and assistant messages.
    CHECK(session.message_count() == 5);

    // Reopening follows the durable message. Whether the assistant reply
    // settled before the failure latched is timing-dependent, so the durable
    // history is the resumed branch plus the user message, plus at most the
    // assistant reply.
    auto after_failure = open_resumed_session(path, workspace);
    REQUIRE(after_failure);
    REQUIRE(after_failure->history.size() >= 3);
    REQUIRE(after_failure->history.size() <= 4);
    CHECK(user_text_at(after_failure->history, 0) == "first");
    CHECK(user_text_at(after_failure->history, 1) == "branched");
    CHECK(user_text_at(after_failure->history, 2) == "continue after partial write");

    // The recorded failure is session-scoped sticky state (ADR 0040): a later
    // prompt is rejected with the typed persistence failure instead of
    // recovering.
    auto recovered = run_prompt(runtime_fixture, session, "recover branch");
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == support::ErrorCode::Session);
    CHECK(
        recovered.error().message ==
        "session persistence failed; rejecting new prompt");
    CHECK(session.message_count() == 5);
    session.close();
}

TEST_CASE("resumed session rejects a branch to a missing entry", "[coding_agent][runtime][session][spec][issue1044]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "invalid-leaf.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("second")).has_value());

    auto resumed = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed);
    // pi `branch` rejects an unknown target and keeps the current position.
    CHECK_FALSE(resumed->branch("missing-entry").has_value());
    CHECK_FALSE(resumed->leaf_id().empty());

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 2);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
}

TEST_CASE("resumed session topology follows active path only", "[coding_agent][runtime][session][spec][issue1044]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "inactive-tree-data.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")).has_value());
    REQUIRE(store->append(user_msg("second")).has_value());
    REQUIRE(store->append(user_msg("third")).has_value());

    auto pre = harness::session::SessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;
    const auto third_id = pre->entries[3].entry_id;

    auto resumed = harness::session::SessionStore::open_existing(path);
    REQUIRE(resumed);
    // Two entries that hang off the first message and are therefore NOT on
    // the active path.
    REQUIRE(resumed->append_branch_summary(first_id, third_id, "inactive branch", std::nullopt, std::nullopt));
    REQUIRE(resumed->append_compaction(
        first_id,
        harness::session::CompactionEntryValue{
            .summary = "inactive compaction",
            .first_kept_entry_id = third_id,
            .tokens_before = 1000,
        }));

    // pi `branch`: navigate back to the third message and continue there, so
    // the durable active path is first → second → third → fourth. The leaf
    // move itself is runtime-only state and is never written; the branch is
    // durable through the next append's explicit `parentId`.
    REQUIRE(resumed->branch(third_id).has_value());
    REQUIRE(resumed->append(user_msg("fourth")).has_value());

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 4);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
    CHECK(user_text_at(opened->history, 2) == "third");
    CHECK(user_text_at(opened->history, 3) == "fourth");
}

TEST_CASE("resumed session carries effective model and thinking level", "[coding_agent][runtime][session][spec]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "context-state.jsonl";
    auto store = harness::session::SessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append(user_msg("hello")).has_value());

    auto opened = open_resumed_session(path, workspace);
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    CHECK(opened->context_model == "gpt-4o");
    CHECK(opened->context_thinking_level == "high");
    REQUIRE(opened->history.size() == 1);
    CHECK(user_text_at(opened->history, 0) == "hello");
}
