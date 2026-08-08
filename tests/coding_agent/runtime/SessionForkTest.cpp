// P13: the in-session fork preparation surface (pi `AgentSessionRuntime.fork`
// subset) — user-message discovery, branched-file creation with label
// re-chaining, the verbatim pi error strings, and the in-memory branch seed.

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/SessionFork.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/harness/session/JsonlSessionStore.hpp>
#include <cch/harness/session/SessionResume.hpp>
#include <cch/harness/session/SessionTree.hpp>

#include <algorithm>
#include <set>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;

namespace {

const std::string kSessionId = "fork-session";

harness::session::SessionMetadata test_metadata(const tests::TempWorkspace& workspace) {
    return {
        .session_id = kSessionId,
        .created_at = "2026-07-05T00:00:00Z",
        .workspace = workspace.path(),
        .provider = "fake",
        .model = "fake-model",
    };
}

ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

ai::MessageVariant assistant_msg(std::string text) {
    auto message = ai::assistant_text_message(std::move(text));
    // The wire DTO requires api/provider/model and a real epoch timestamp
    // on assistant messages.
    message.api = "fake-api";
    message.provider = "fake";
    message.model = "fake-model";
    message.timestamp = 1750000000000;
    return ai::MessageVariant{std::move(message)};
}

std::string user_text_at(const std::vector<ai::MessageVariant>& messages, std::size_t index) {
    REQUIRE(index < messages.size());
    const auto* user = std::get_if<ai::UserMessage>(&messages[index]);
    REQUIRE(user != nullptr);
    return ai::text_from_user_message(*user);
}

/// Build a linear session: user, assistant, user, assistant, user, assistant.
util::Expected<harness::session::JsonlSessionStore> build_linear_session(
    const std::filesystem::path& path,
    const tests::TempWorkspace& workspace) {
    auto created = harness::session::JsonlSessionStore::create_new(
        path, test_metadata(workspace));
    if (!created) return std::unexpected(created.error());
    auto store = std::move(*created);
    for (std::size_t index = 0; index < 3; ++index) {
        if (auto appended = store.append(user_msg("user-" + std::to_string(index)));
            !appended) {
            return std::unexpected(appended.error());
        }
        if (auto appended = store.append(assistant_msg("assistant-" + std::to_string(index)));
            !appended) {
            return std::unexpected(appended.error());
        }
    }
    return store;
}

/// The entry id of the last message entry in a session file (used to target
/// label entries at real ids).
[[nodiscard]] std::string last_message_entry_id(const std::filesystem::path& path) {
    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    for (auto it = tree.entries().rbegin(); it != tree.entries().rend(); ++it) {
        if (it->kind == harness::session::SessionEntryKind::Message) {
            return it->entry_id;
        }
    }
    REQUIRE(false);
    return {};
}

/// A session with a label on the first user message and a label entry
/// BETWEEN the second user message and its assistant response, so the branch
/// writer must re-chain the assistant's parent past the removed label.
util::Expected<harness::session::JsonlSessionStore> build_labeled_session(
    const std::filesystem::path& path,
    const tests::TempWorkspace& workspace) {
    auto created = harness::session::JsonlSessionStore::create_new(
        path, test_metadata(workspace));
    if (!created) return std::unexpected(created.error());
    auto store = std::move(*created);
    if (auto appended = store.append(user_msg("user-0")); !appended) {
        return std::unexpected(appended.error());
    }
    // Label targeting the first user message (its real entry id).
    const auto user_0_id = last_message_entry_id(path);
    if (auto appended = store.append_label_change(std::nullopt, user_0_id, "important");
        !appended) {
        return std::unexpected(appended.error());
    }
    if (auto appended = store.append(user_msg("user-1")); !appended) {
        return std::unexpected(appended.error());
    }
    // A label BETWEEN user-1 and its response: the response's parent is the
    // label entry, so re-chaining must skip it.
    const auto user_1_id = last_message_entry_id(path);
    if (auto appended = store.append_label_change(std::nullopt, user_1_id, "middle");
        !appended) {
        return std::unexpected(appended.error());
    }
    if (auto appended = store.append(assistant_msg("assistant-1")); !appended) {
        return std::unexpected(appended.error());
    }
    return store;
}

std::vector<ai::MessageVariant> resumed_history(const std::filesystem::path& path) {
    auto resumed = harness::session::resume_session(path);
    REQUIRE(resumed.has_value());
    return resumed->history;
}

} // namespace

TEST_CASE("user messages for forking list user texts in order", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto created = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(created.has_value());
    REQUIRE(created->append(user_msg("first")).has_value());
    REQUIRE(created->append(user_msg("   ")).has_value());
    REQUIRE(created->append(assistant_msg("reply")).has_value());
    REQUIRE(created->append(user_msg("second")).has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();
    const auto messages = runtime::user_messages_for_forking(source);
    // pi keeps whitespace-only texts (JS truthiness); tool results and
    // non-user roles are skipped.
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].text == "first");
    CHECK(messages[1].text == "   ");
    CHECK(messages[2].text == "second");
    CHECK_FALSE(messages[0].entry_id.empty());
    CHECK(messages[0].entry_id != messages[1].entry_id);
}

TEST_CASE("fork before a middle user message writes the prefix branch with selected text", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto store = build_linear_session(path, workspace);
    REQUIRE(store.has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();
    const auto messages = runtime::user_messages_for_forking(source);
    REQUIRE(messages.size() == 3);

    // Fork before the second user message: the branch copies the first
    // user/assistant pair and pre-fills the second message's text.
    const auto prepared = runtime::prepare_fork(source, messages[1].entry_id, runtime::ForkPosition::Before);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->branched_path.has_value());
    REQUIRE(prepared->selected_text.has_value());
    CHECK(*prepared->selected_text == "user-1");
    CHECK_FALSE(prepared->in_memory_seed.has_value());

    const auto history = resumed_history(*prepared->branched_path);
    REQUIRE(history.size() == 2);
    CHECK(user_text_at(history, 0) == "user-0");

    // The branch file carries the fresh identity and the parent pointer.
    auto loaded = harness::session::JsonlSessionStore::load(*prepared->branched_path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->metadata.session_id != kSessionId);
    REQUIRE(loaded->metadata.parent_session.has_value());
    CHECK(*loaded->metadata.parent_session == path);
    CHECK(loaded->metadata.workspace == workspace.path());
}

TEST_CASE("fork at the current position duplicates the branch to the entry", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto store = build_linear_session(path, workspace);
    REQUIRE(store.has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();
    const auto messages = runtime::user_messages_for_forking(source);
    REQUIRE(messages.size() == 3);

    // Fork at the third user message: the branch copies the root-to-entry
    // path (the entry itself included; later children — the third assistant
    // response — stay behind, pi `getBranch` walks the parent chain).
    const auto prepared = runtime::prepare_fork(source, messages[2].entry_id, runtime::ForkPosition::At);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->branched_path.has_value());
    CHECK_FALSE(prepared->selected_text.has_value());

    const auto history = resumed_history(*prepared->branched_path);
    REQUIRE(history.size() == 5);
    CHECK(user_text_at(history, 0) == "user-0");
    CHECK(user_text_at(history, 4) == "user-2");
}

TEST_CASE("fork before a root user message creates an empty parented session", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto store = build_linear_session(path, workspace);
    REQUIRE(store.has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();
    const auto messages = runtime::user_messages_for_forking(source);
    REQUIRE(messages.size() == 3);

    const auto prepared = runtime::prepare_fork(source, messages[0].entry_id, runtime::ForkPosition::Before);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->branched_path.has_value());
    REQUIRE(prepared->selected_text.has_value());
    CHECK(*prepared->selected_text == "user-0");

    // pi: the no-target-leaf fork is a fresh session with the parent pointer.
    const auto history = resumed_history(*prepared->branched_path);
    CHECK(history.empty());
    auto loaded = harness::session::JsonlSessionStore::load(*prepared->branched_path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->metadata.parent_session.has_value());
    CHECK(*loaded->metadata.parent_session == path);
}

TEST_CASE("fork errors match pi verbatim strings", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto store = build_linear_session(path, workspace);
    REQUIRE(store.has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();

    // Unknown entry id.
    const auto unknown = runtime::prepare_fork(source, "deadbeef", runtime::ForkPosition::Before);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message == "Invalid entry ID for forking");

    // A non-user entry with position "before": fork the last assistant
    // entry's id.
    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    const auto assistant_entry = std::find_if(
        tree.entries().begin(), tree.entries().end(),
        [](const harness::session::SessionEntry& entry) {
            return entry.kind == harness::session::SessionEntryKind::Message &&
                entry.message &&
                std::holds_alternative<ai::AssistantMessage>(*entry.message);
        });
    REQUIRE(assistant_entry != tree.entries().end());
    const auto non_user = runtime::prepare_fork(source, assistant_entry->entry_id, runtime::ForkPosition::Before);
    REQUIRE_FALSE(non_user.has_value());
    CHECK(non_user.error().message == "Invalid entry ID for forking");

    // A missing source file: the unsaved-session error.
    runtime::ForkSource missing_source;
    missing_source.session_path = workspace.path() / "missing.jsonl";
    missing_source.workspace = workspace.path();
    const auto unsaved = runtime::prepare_fork(missing_source, "deadbeef", runtime::ForkPosition::At);
    REQUIRE_FALSE(unsaved.has_value());
    CHECK(
        unsaved.error().message ==
        "This session has not been saved yet. Wait for the first assistant "
        "response before cloning or forking it.");

    // An empty source file and a header-less source file carry the G3
    // verbatim "Cannot fork: ..." strings.
    const auto empty_source = workspace.path() / "empty.jsonl";
    {
        std::ofstream output(empty_source, std::ios::binary);
        output << "\n";
    }
    std::filesystem::permissions(
        empty_source, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    runtime::ForkSource empty_fork;
    empty_fork.session_path = empty_source;
    empty_fork.workspace = workspace.path();
    const auto empty_result = runtime::prepare_fork(
        empty_fork, "deadbeef", runtime::ForkPosition::At);
    REQUIRE_FALSE(empty_result.has_value());
    CHECK(
        empty_result.error().message ==
        "Cannot fork: source session file is empty or invalid: " +
            empty_source.string());

    const auto headerless_source = workspace.path() / "headerless.jsonl";
    {
        std::ofstream output(headerless_source, std::ios::binary);
        output << "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":1,"
                  "\"message\":{\"role\":\"user\",\"content\":\"x\"}}\n";
    }
    std::filesystem::permissions(
        headerless_source, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    runtime::ForkSource headerless_fork;
    headerless_fork.session_path = headerless_source;
    headerless_fork.workspace = workspace.path();
    const auto headerless_result = runtime::prepare_fork(
        headerless_fork, "deadbeef", runtime::ForkPosition::At);
    REQUIRE_FALSE(headerless_result.has_value());
    CHECK(
        headerless_result.error().message ==
        "Cannot fork: source session has no header: " +
            headerless_source.string());
}

TEST_CASE("fork re-chains parents past labels and re-creates retained labels", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";
    auto store = build_labeled_session(path, workspace);
    REQUIRE(store.has_value());

    runtime::ForkSource source;
    source.session_path = path;
    source.workspace = workspace.path();
    const auto messages = runtime::user_messages_for_forking(source);
    REQUIRE(messages.size() == 2);

    // Fork at the second user message: the branch keeps user-0 + user-1 with
    // user-1 re-chained past the removed "middle" label entry, and
    // re-creates both labels ("important" on user-0, "middle" on user-1).
    const auto prepared = runtime::prepare_fork(source, messages[1].entry_id, runtime::ForkPosition::At);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->branched_path.has_value());

    auto loaded = harness::session::JsonlSessionStore::load(*prepared->branched_path);
    REQUIRE(loaded.has_value());
    harness::session::SessionTree tree(std::move(*loaded));
    REQUIRE(tree.entries().size() == 4);

    // The label entries are re-created with the retained targets.
    std::size_t label_count = 0;
    std::set<std::string> label_targets;
    for (const auto& entry : tree.entries()) {
        if (entry.kind != harness::session::SessionEntryKind::Label) {
            continue;
        }
        ++label_count;
        label_targets.insert(std::get<harness::session::LabelEntryValue>(entry.value).target_id);
    }
    CHECK(label_count == 2);
    CHECK(label_targets.size() == 2);

    // user-1's parent must be user-0 (the removed "middle" label entry is
    // never a parent in the branch).
    const auto user_1 = std::find_if(
        tree.entries().begin(), tree.entries().end(),
        [](const harness::session::SessionEntry& entry) {
            const auto* user = entry.message
                ? std::get_if<ai::UserMessage>(&*entry.message)
                : nullptr;
            return user != nullptr &&
                ai::text_from_user_message(*user) == "user-1";
        });
    REQUIRE(user_1 != tree.entries().end());
    REQUIRE(user_1->parent_id.has_value());
    const auto* parent = tree.getEntry(*user_1->parent_id);
    REQUIRE(parent != nullptr);
    const auto* parent_user = parent->message
        ? std::get_if<ai::UserMessage>(&*parent->message)
        : nullptr;
    REQUIRE(parent_user != nullptr);
    CHECK(ai::text_from_user_message(*parent_user) == "user-0");
}

TEST_CASE("in-memory fork produces the branch seed with the prefix messages", "[coding_agent][runtime][session-fork][issue409]") {
    tests::TempWorkspace workspace;
    harness::session::SessionContext context;
    context.messages = {
        user_msg("in-mem-0"),
        assistant_msg("in-mem-reply-0"),
        user_msg("in-mem-1"),
    };
    context.thinking_level = "high";

    runtime::ForkSource source;
    source.workspace = workspace.path();
    source.live_context = context;

    const auto messages = runtime::user_messages_for_forking(source);
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].text == "in-mem-0");
    CHECK(messages[1].text == "in-mem-1");

    // Fork before the second message: the branch carries only the first
    // pair, the selected text pre-fills the editor.
    const auto prepared = runtime::prepare_fork(source, messages[1].entry_id, runtime::ForkPosition::Before);
    REQUIRE(prepared.has_value());
    CHECK_FALSE(prepared->branched_path.has_value());
    REQUIRE(prepared->selected_text.has_value());
    CHECK(*prepared->selected_text == "in-mem-1");
    REQUIRE(prepared->in_memory_seed.has_value());
    CHECK(prepared->in_memory_seed->context.messages.size() == 2);
    CHECK(prepared->in_memory_seed->context.thinking_level == "high");
    CHECK_FALSE(prepared->in_memory_seed->parent_session.has_value());

    // Position "at" includes the chosen message itself.
    const auto at = runtime::prepare_fork(source, messages[1].entry_id, runtime::ForkPosition::At);
    REQUIRE(at.has_value());
    REQUIRE(at->in_memory_seed.has_value());
    CHECK(at->in_memory_seed->context.messages.size() == 3);

    // Unknown synthetic id.
    const auto unknown = runtime::prepare_fork(source, "mem-99", runtime::ForkPosition::Before);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message == "Invalid entry ID for forking");
}
