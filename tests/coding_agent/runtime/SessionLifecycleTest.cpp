#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/SessionLifecycle.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../support/TempWorkspace.hpp"

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
    return {"session-lifecycle-test", "2026-07-05T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

std::string user_text_at(const std::vector<ai::MessageVariant>& messages, std::size_t index) {
    REQUIRE(index < messages.size());
    const auto* user = std::get_if<ai::UserMessage>(&messages[index]);
    REQUIRE(user != nullptr);
    return ai::text_from_content(user->content);
}

runtime::SessionOpenRequest resume_request(const std::filesystem::path& path,
                                           const tests::TempWorkspace& workspace) {
    runtime::SessionOpenRequest request;
    request.resume_path = path;
    request.workspace = workspace.path();
    request.workspace_explicit = true;
    return request;
}

} // namespace

TEST_CASE("open_session resume uses tree context for linear sessions", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "linear.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto opened = runtime::open_session(resume_request(path, workspace));
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

TEST_CASE("open_session resume uses compaction tree context", "[coding-agent][runtime][session]") {
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

    auto opened = runtime::open_session(resume_request(path, workspace));
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Compacted);
    REQUIRE(opened->history.size() == 3);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(opened->history[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(opened->history[0]).summary == "summary of msg1-2");
    CHECK(user_text_at(opened->history, 1) == "msg3");
    CHECK(user_text_at(opened->history, 2) == "msg4");
}

TEST_CASE("open_session resume uses active leaf path", "[coding-agent][runtime][session]") {
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

    auto opened = runtime::open_session(resume_request(path, workspace));
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Branched);
    REQUIRE(opened->history.size() == 1);
    CHECK(user_text_at(opened->history, 0) == "first");
}

TEST_CASE("open_session resume ignores invalid leaf target", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "invalid-leaf.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, "missing-entry"));

    auto opened = runtime::open_session(resume_request(path, workspace));
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 2);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
}

TEST_CASE("open_session topology follows active path only", "[coding-agent][runtime][session]") {
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

    auto opened = runtime::open_session(resume_request(path, workspace));
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    REQUIRE(opened->history.size() == 3);
    CHECK(user_text_at(opened->history, 0) == "first");
    CHECK(user_text_at(opened->history, 1) == "second");
    CHECK(user_text_at(opened->history, 2) == "third");
}

TEST_CASE("open_session resume carries effective model and thinking level", "[coding-agent][runtime][session]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "context-state.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append(user_msg("hello")));

    auto opened = runtime::open_session(resume_request(path, workspace));
    REQUIRE(opened);
    CHECK(opened->topology == harness::session::SessionTopology::Linear);
    CHECK(opened->context_model == "gpt-4o");
    CHECK(opened->context_thinking_level == "high");
    REQUIRE(opened->history.size() == 1);
    CHECK(user_text_at(opened->history, 0) == "hello");
}
