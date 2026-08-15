#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/agent/harness/session/JsonlSessionStore.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>
#include "../../support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {
harness::session::SessionMetadata test_metadata(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-10T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

} // namespace

// ── U1: SessionTree construction and basic queries ──

TEST_CASE("SessionTree constructs from linear LoadedSession", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "linear.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("hello")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("world")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 3); // header + 2 messages

    harness::session::SessionTree tree(std::move(*loaded));
    CHECK(tree.size() == 2);
    CHECK_FALSE(tree.empty());
    CHECK(tree.metadata().session_id == "session-test");
}

TEST_CASE("SessionTree getEntry finds entry by ID", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "lookup.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("first")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);

    std::string entry_id = loaded->entries[1].entry_id;
    REQUIRE_FALSE(entry_id.empty());

    harness::session::SessionTree tree(std::move(*loaded));
    const auto* entry = tree.getEntry(entry_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->entry_id == entry_id);
    CHECK(entry->kind == harness::session::SessionEntryKind::Message);
}

TEST_CASE("SessionTree getEntry returns nullptr for unknown ID", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "empty.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("msg")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    CHECK(tree.getEntry("deadbeef") == nullptr);
}

TEST_CASE("SessionTree getChildren returns correct children", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "children.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Simulate a fork: A → B → C and A → B → D
    // We can't directly create forks via JsonlSessionStore, but we can write entries with explicit parent IDs.
    // Use model_change entries that can set parentId.
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("root")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("branch-1")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("branch-2")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() >= 3);

    harness::session::SessionTree tree(std::move(*loaded));

    // In a linear tree, each parent has at most 1 child.
    // Root's parent is nullopt, so it won't appear in children_ map under any key.
    // The second entry's parent should be the first entry's ID.
    if (tree.size() >= 2) {
        std::string parent_id = tree.entries()[0].entry_id;
        auto children = tree.getChildren(parent_id);
        // Linear tree: parent has exactly 1 child
        CHECK(children.size() == 1);
    }
}

TEST_CASE("SessionTree empty tree (header only)", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "header-only.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Don't append any messages

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    CHECK(tree.empty());
    CHECK(tree.size() == 0);
    CHECK(tree.entries().empty());
}

TEST_CASE("SessionTree is move-only", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "move.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("msg")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree1(std::move(*loaded));
    CHECK(tree1.size() == 1);

    // Move construction
    harness::session::SessionTree tree2(std::move(tree1));
    CHECK(tree2.size() == 1);

    // Move assignment
    harness::session::SessionTree tree3(harness::session::LoadedSession{});
    tree3 = std::move(tree2);
    CHECK(tree3.size() == 1);
}

// ── U2: Leaf tracking and tree navigation ──

TEST_CASE("SessionTree leaf_id defaults to last entry in linear session", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-default.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("first")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("second")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    // Leaf should be the last entry
    CHECK_FALSE(tree.leaf_id().empty());
    const auto* leaf = tree.leaf_entry();
    REQUIRE(leaf != nullptr);
    CHECK(leaf->entry_id == tree.leaf_id());
}

TEST_CASE("SessionTree branch switches active leaf", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "branch.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("first")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("second")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("third")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    // Default leaf is third entry
    std::string first_id = tree.entries()[0].entry_id;
    std::string third_id = tree.entries()[2].entry_id;
    CHECK(tree.leaf_id() == third_id);

    // Branch to first entry
    auto result = tree.branch(first_id);
    REQUIRE(result.has_value());
    CHECK(tree.leaf_id() == first_id);
}

TEST_CASE("SessionTree branch rejects unknown entry ID", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "branch-err.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("msg")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    std::string original_leaf = tree.leaf_id();
    auto result = tree.branch("deadbeef");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == support::ErrorCode::Session);
    // Leaf should not have changed
    CHECK(tree.leaf_id() == original_leaf);
}

TEST_CASE("SessionTree getBranch returns leaf-to-root path", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "path.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("root")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("child")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("leaf")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto path_entries = tree.getBranch();
    REQUIRE(path_entries.size() == 3);
    // Leaf-to-root: leaf is first, root is last
    CHECK(path_entries[0]->entry_id == tree.leaf_id());

    // From specific entry
    std::string middle_id = tree.entries()[1].entry_id;
    auto from_middle = tree.getBranch(middle_id);
    REQUIRE(from_middle.size() == 2);
    CHECK(from_middle[0]->entry_id == middle_id);
}

TEST_CASE("SessionTree getBranch from root returns single entry", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "root-path.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("only")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto path_entries = tree.getBranch();
    REQUIRE(path_entries.size() == 1);
}

TEST_CASE("SessionTree root returns root entry", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "find-root.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("first")}));
    REQUIRE(store->append(ai::MessageVariant{ai::user_text_message("second")}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    const auto* r = tree.root();
    REQUIRE(r != nullptr);
    CHECK(r->entry_id == tree.entries()[0].entry_id);
}

TEST_CASE("SessionTree root returns nullptr for empty tree", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "empty-root.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    CHECK(tree.root() == nullptr);
    CHECK(tree.leaf_entry() == nullptr);
    CHECK(tree.leaf_id().empty());
}

// ── U3: Context reconstruction ──

namespace {
ai::MessageVariant user_msg(std::string text) {
    return ai::MessageVariant{ai::user_text_message(std::move(text))};
}

std::string first_user_text(const std::vector<ai::MessageVariant>& msgs) {
    for (const auto& m : msgs) {
        if (const auto* u = std::get_if<ai::UserMessage>(&m)) {
            const auto& blocks = std::get<std::vector<ai::Content>>(u->content);
            if (!blocks.empty()) {
                if (const auto* t = std::get_if<ai::TextContent>(&blocks.front())) {
                    return t->text;
                }
            }
        }
    }
    return {};
}

support::JsonValue ignored_payload_marker(std::string marker) {
    return support::JsonValue{support::JsonValue::object_t{
        {"ignoredPayloadMarker", support::JsonValue{std::move(marker)}},
    }};
}
} // namespace

TEST_CASE("buildSessionContext linear tree returns all messages", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-linear.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    REQUIRE(ctx.messages.size() == 2);
    CHECK(first_user_text(ctx.messages) == "first");
    CHECK_FALSE(ctx.model.has_value());
    // pi derived state: thinkingLevel defaults to "off", activeToolNames to
    // null, and no thinking entry means no resumed-entry gate.
    CHECK(ctx.thinking_level == "off");
    CHECK_FALSE(ctx.has_thinking_level_entry);
    CHECK_FALSE(ctx.active_tool_names.has_value());
}

TEST_CASE("buildSessionContext extracts model and thinking level", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-model.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Use nullopt parent_ids for linear chain inference.
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append(user_msg("hello")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    CHECK(ctx.model == "gpt-4o");
    CHECK(ctx.thinking_level == "high");
    CHECK(ctx.has_thinking_level_entry);
    REQUIRE(ctx.messages.size() == 1);
    CHECK(first_user_text(ctx.messages) == "hello");
}

TEST_CASE(
    "buildSessionContext derives thinkingLevel/model/activeToolNames over every entry type",
    "[harness][session][tree][issue357]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-derived-full.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // One linear branch carrying every entry type. Inert kinds (label, custom,
    // custom_message, branch_summary, session_info, compaction) never disturb
    // the derived state; the last derived-state entry of each kind wins
    // (pi `deriveSessionContextState`). The assistant message lands after the
    // `model_change`, so its provider/model override the earlier entry.
    REQUIRE(store->append(user_msg("root")));
    REQUIRE(store->append_compaction(std::nullopt, "minimal summary", "", 1234, std::nullopt, std::nullopt));
    REQUIRE(store->append_label_change(std::nullopt, "", "checkpoint"));
    REQUIRE(store->append_custom_entry(std::nullopt, "extension.meta", support::JsonValue{42}));
    REQUIRE(store->append_custom_message_entry(
        std::nullopt, "extension.msg", "injected", true, std::nullopt));
    REQUIRE(store->append_branch_summary(std::nullopt, "from-leaf", "explored", std::nullopt, std::nullopt));
    REQUIRE(store->append_session_info(std::nullopt, "review name"));
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4.1"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append_active_tools_change(
        std::nullopt, {"read", "bash", "edit", "write"}));

    ai::AssistantMessage assistant = ai::assistant_text_message("hi");
    assistant.api = "anthropic-messages";
    assistant.provider = "anthropic";
    assistant.model = "claude-sonnet-4-5";
    assistant.timestamp = 1784678402000;
    REQUIRE(store->append(ai::MessageVariant{std::move(assistant)}));

    // Point the leaf at the last message so the branch covers the whole chain
    // (all eleven entry kinds participate in topology).
    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 11);
    const auto assistant_id = pre->entries.back().entry_id;
    auto leaf_store = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(leaf_store);
    REQUIRE(leaf_store->append_leaf(std::nullopt, assistant_id));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));
    auto ctx = tree.buildSessionContext();

    // pi derived state: the last `thinking_level_change` wins and gates resume
    // restoration; the last assistant message's provider/model overrides the
    // earlier `model_change`; the last `active_tools_change` is copied.
    CHECK(ctx.thinking_level == "high");
    CHECK(ctx.has_thinking_level_entry);
    REQUIRE(ctx.provider.has_value());
    CHECK(*ctx.provider == "anthropic");
    REQUIRE(ctx.model.has_value());
    CHECK(*ctx.model == "claude-sonnet-4-5");
    REQUIRE(ctx.active_tool_names.has_value());
    const std::vector<std::string> expected_tools{"read", "bash", "edit", "write"};
    CHECK(*ctx.active_tool_names == expected_tools);
}

TEST_CASE(
    "buildSessionContext last model_change wins over an earlier assistant message",
    "[harness][session][tree][issue357]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-derived-model.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("root")));

    ai::AssistantMessage assistant = ai::assistant_text_message("earlier");
    assistant.api = "anthropic-messages";
    assistant.provider = "anthropic";
    assistant.model = "claude-sonnet-4-5";
    assistant.timestamp = 1784678402000;
    REQUIRE(store->append(ai::MessageVariant{std::move(assistant)}));

    // The `model_change` closest to the leaf wins over the earlier assistant
    // message; an explicit "off" thinking entry and an empty tools entry stay
    // engaged (pi: entries update the derived state, they never clear it).
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4.1"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "off"));
    REQUIRE(store->append_active_tools_change(std::nullopt, {}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));
    auto ctx = tree.buildSessionContext();

    REQUIRE(ctx.provider.has_value());
    CHECK(*ctx.provider == "openai");
    REQUIRE(ctx.model.has_value());
    CHECK(*ctx.model == "gpt-4.1");
    CHECK(ctx.thinking_level == "off");
    CHECK(ctx.has_thinking_level_entry);
    REQUIRE(ctx.active_tool_names.has_value());
    CHECK(ctx.active_tool_names->empty());
}

TEST_CASE("buildSessionContext compaction skips pre-kept messages", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-compact.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    // Write 3 messages, then a compaction that keeps from message 3
    REQUIRE(store->append(user_msg("msg1")));
    REQUIRE(store->append(user_msg("msg2")));
    REQUIRE(store->append(user_msg("msg3")));

    // Get the entry IDs from a separate load to construct the compaction
    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);  // header + 3 messages
    std::string msg3_id = pre->entries[3].entry_id;  // third message (1-indexed after header)

    // Now write the compaction entry via open_existing
    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_compaction(std::nullopt, "summary of msg1-2", msg3_id, 1000, std::nullopt, std::nullopt));
    REQUIRE(resumed->append(user_msg("msg4")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    // Expected: CompactionSummaryMessage + msg3 + msg4 (msg1 and msg2 are before the first kept entry)
    REQUIRE(ctx.messages.size() == 3);
    // First message should be compaction summary
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(ctx.messages[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(ctx.messages[0]).summary == "summary of msg1-2");
    CHECK(std::get<ai::CompactionSummaryMessage>(ctx.messages[0]).tokens_before == 1000);
    // Remaining should be msg3 and msg4
    CHECK(first_user_text({ctx.messages[1]}) == "msg3");
    CHECK(first_user_text({ctx.messages[2]}) == "msg4");
}

TEST_CASE("buildSessionContext branch summary converted to message", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-branch.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("root")));
    REQUIRE(store->append_branch_summary(std::nullopt, "from-branch", "explored X", std::nullopt, std::nullopt));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    REQUIRE(ctx.messages.size() == 2);
    // First: user message
    CHECK(first_user_text({ctx.messages[0]}) == "root");
    // Second: BranchSummaryMessage
    REQUIRE(std::holds_alternative<ai::BranchSummaryMessage>(ctx.messages[1]));
    CHECK(std::get<ai::BranchSummaryMessage>(ctx.messages[1]).summary == "explored X");
}

TEST_CASE("buildSessionContext custom message converted", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-custom.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("root")));
    REQUIRE(store->append_custom_message_entry(std::nullopt, "my-ext", "injected", true, std::nullopt));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    REQUIRE(ctx.messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(ctx.messages[1]));
    const auto& cm = std::get<ai::CustomMessage>(ctx.messages[1]);
    CHECK(cm.custom_type == "my-ext");
    REQUIRE_FALSE(cm.content.empty());
    CHECK(std::get<ai::TextContent>(cm.content.front()).text == "injected");
}

TEST_CASE("buildSessionContext uses typed timestamps for extended entries", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    harness::session::LoadedSession loaded;
    loaded.metadata = test_metadata(workspace);

    harness::session::SessionEntry root;
    root.kind = harness::session::SessionEntryKind::Message;
    root.entry_id = "msg001";
    root.timestamp = 1783209600100;
    root.message = user_msg("root");
    loaded.entries.push_back(std::move(root));

    harness::session::SessionEntry custom;
    custom.kind = harness::session::SessionEntryKind::CustomMessage;
    custom.entry_id = "cust001";
    custom.parent_id = "msg001";
    custom.timestamp = 1783209601234;
    custom.value = harness::session::CustomMessageEntryValue{
        .custom_type = "ext",
        .content = "injected",
        .display = true,
        .details = std::nullopt,
    };
    loaded.entries.push_back(std::move(custom));

    harness::session::SessionEntry branch;
    branch.kind = harness::session::SessionEntryKind::BranchSummary;
    branch.entry_id = "branch001";
    branch.parent_id = "cust001";
    branch.timestamp = 1783209602345;
    branch.value = harness::session::BranchSummaryEntryValue{
        .from_id = "oldleaf",
        .summary = "branch summary",
        .details = std::nullopt,
        .from_hook = std::nullopt,
    };
    loaded.entries.push_back(std::move(branch));

    harness::session::SessionEntry compaction;
    compaction.kind = harness::session::SessionEntryKind::Compaction;
    compaction.entry_id = "compact1";
    compaction.parent_id = "branch001";
    compaction.timestamp = 1783209603456;
    compaction.value = harness::session::CompactionEntryValue{
        .summary = "compact summary",
        .first_kept_entry_id = "msg001",
        .tokens_before = 42,
        .details = std::nullopt,
        .from_hook = std::nullopt,
    };
    loaded.entries.push_back(std::move(compaction));

    harness::session::SessionTree tree(std::move(loaded));

    auto ctx = tree.buildSessionContext();
    REQUIRE(ctx.messages.size() == 4);

    const auto& compaction_msg = std::get<ai::CompactionSummaryMessage>(ctx.messages[0]);
    CHECK(compaction_msg.summary == "compact summary");
    CHECK(compaction_msg.tokens_before == 42);
    CHECK(compaction_msg.timestamp == 1783209603456);

    const auto& custom_msg = std::get<ai::CustomMessage>(ctx.messages[2]);
    CHECK(custom_msg.custom_type == "ext");
    CHECK(custom_msg.timestamp == 1783209601234);

    const auto& branch_msg = std::get<ai::BranchSummaryMessage>(ctx.messages[3]);
    CHECK(branch_msg.summary == "branch summary");
    CHECK(branch_msg.from_id == "oldleaf");
    CHECK(branch_msg.timestamp == 1783209602345);
}

TEST_CASE("buildSessionContext reads known entry meaning from typed values", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    harness::session::LoadedSession loaded;
    loaded.metadata = test_metadata(workspace);

    harness::session::SessionEntry model;
    model.kind = harness::session::SessionEntryKind::ModelChange;
    model.entry_id = "model001";
    model.value = harness::session::ModelChangeValue{.provider = "typed-provider", .model_id = "typed-model"};
    model.payload = ignored_payload_marker("model");
    loaded.entries.push_back(std::move(model));

    harness::session::SessionEntry thinking;
    thinking.kind = harness::session::SessionEntryKind::ThinkingLevelChange;
    thinking.entry_id = "think001";
    thinking.parent_id = "model001";
    thinking.value = harness::session::ThinkingLevelChangeValue{.thinking_level = "typed-thinking"};
    thinking.payload = ignored_payload_marker("thinking");
    loaded.entries.push_back(std::move(thinking));

    harness::session::SessionEntry custom;
    custom.kind = harness::session::SessionEntryKind::CustomMessage;
    custom.entry_id = "custom01";
    custom.parent_id = "think001";
    custom.value = harness::session::CustomMessageEntryValue{
        .custom_type = "typed-ext",
        .content = "typed content",
        .display = true,
        .details = std::nullopt,
    };
    custom.payload = ignored_payload_marker("custom");
    loaded.entries.push_back(std::move(custom));

    harness::session::SessionEntry branch;
    branch.kind = harness::session::SessionEntryKind::BranchSummary;
    branch.entry_id = "branch01";
    branch.parent_id = "custom01";
    branch.value = harness::session::BranchSummaryEntryValue{
        .from_id = "typed-from",
        .summary = "typed branch",
        .details = std::nullopt,
        .from_hook = std::nullopt,
    };
    branch.payload = ignored_payload_marker("branch");
    loaded.entries.push_back(std::move(branch));

    harness::session::SessionEntry compaction;
    compaction.kind = harness::session::SessionEntryKind::Compaction;
    compaction.entry_id = "compact1";
    compaction.parent_id = "branch01";
    compaction.value = harness::session::CompactionEntryValue{
        .summary = "typed compact",
        .first_kept_entry_id = "custom01",
        .tokens_before = 77,
        .details = std::nullopt,
        .from_hook = std::nullopt,
    };
    compaction.payload = ignored_payload_marker("compaction");
    loaded.entries.push_back(std::move(compaction));

    harness::session::SessionTree tree(std::move(loaded));
    auto ctx = tree.buildSessionContext();

    CHECK(ctx.model == "typed-model");
    CHECK(ctx.thinking_level == "typed-thinking");
    REQUIRE(ctx.messages.size() == 3);
    const auto& compact_msg = std::get<ai::CompactionSummaryMessage>(ctx.messages[0]);
    CHECK(compact_msg.summary == "typed compact");
    CHECK(compact_msg.tokens_before == 77);
    const auto& custom_msg = std::get<ai::CustomMessage>(ctx.messages[1]);
    CHECK(custom_msg.custom_type == "typed-ext");
    CHECK(std::get<ai::TextContent>(custom_msg.content.front()).text == "typed content");
    const auto& branch_msg = std::get<ai::BranchSummaryMessage>(ctx.messages[2]);
    CHECK(branch_msg.summary == "typed branch");
    CHECK(branch_msg.from_id == "typed-from");
}

TEST_CASE("buildSessionContext empty tree returns empty context", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-empty.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    auto ctx = tree.buildSessionContext();
    CHECK(ctx.messages.empty());
    CHECK_FALSE(ctx.model.has_value());
    CHECK(ctx.thinking_level == "off");
    CHECK_FALSE(ctx.has_thinking_level_entry);
    CHECK_FALSE(ctx.active_tool_names.has_value());
}

TEST_CASE("buildSessionContext respects branch navigation", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "ctx-nav.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));
    REQUIRE(store->append(user_msg("third")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    harness::session::SessionTree tree(std::move(*loaded));

    // Default: leaf is third, context has 3 messages
    auto ctx_all = tree.buildSessionContext();
    CHECK(ctx_all.messages.size() == 3);

    // Branch to first entry
    std::string first_id = tree.entries()[0].entry_id;
    REQUIRE(tree.branch(first_id).has_value());

    // Now context should have only 1 message
    auto ctx_branched = tree.buildSessionContext();
    CHECK(ctx_branched.messages.size() == 1);
    CHECK(first_user_text(ctx_branched.messages) == "first");
}

// ── U5: JsonlSessionStore integration ──

TEST_CASE("open_as_tree returns valid SessionTree", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "open-tree.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("hello")));

    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());
    CHECK(tree->size() == 1);
    CHECK_FALSE(tree->leaf_id().empty());
}

TEST_CASE("append_leaf round-trips and restores leaf position", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-roundtrip.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("first")));
    REQUIRE(store->append(user_msg("second")));

    // Get second message's ID
    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 3);
    std::string second_id = pre->entries[2].entry_id;

    // Open existing and write a Leaf entry pointing to second message
    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, second_id));

    // Load as tree — leaf should be restored to second_id
    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());
    CHECK(tree->leaf_id() == second_id);
}

TEST_CASE("SessionTree ignores a stale latest leaf marker and falls back to last navigable entry", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-stale-latest.jsonl";
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
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));
    REQUIRE(resumed->append_leaf(std::nullopt, "missing-entry"));

    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());
    CHECK(tree->leaf_id() == third_id);
}

TEST_CASE("SessionTree restores leaf from typed leaf value", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    harness::session::LoadedSession loaded;
    loaded.metadata = test_metadata(workspace);

    harness::session::SessionEntry first;
    first.kind = harness::session::SessionEntryKind::Message;
    first.entry_id = "first001";
    first.message = user_msg("first");
    loaded.entries.push_back(std::move(first));

    harness::session::SessionEntry second;
    second.kind = harness::session::SessionEntryKind::Message;
    second.entry_id = "second01";
    second.parent_id = "first001";
    second.message = user_msg("second");
    loaded.entries.push_back(std::move(second));

    harness::session::SessionEntry leaf;
    leaf.kind = harness::session::SessionEntryKind::Leaf;
    leaf.entry_id = "leaf0001";
    leaf.value = harness::session::LeafEntryValue{.target_id = std::string{"first001"}};
    leaf.payload = ignored_payload_marker("leaf");
    loaded.entries.push_back(std::move(leaf));

    harness::session::SessionTree tree(std::move(loaded));
    CHECK(tree.leaf_id() == "first001");
}

TEST_CASE("open_as_tree missing file returns error", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "nonexistent.jsonl";
    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE_FALSE(tree.has_value());
}

// ── U4: Branch summary hook ──

TEST_CASE("branchWithSummary generates summary and switches leaf", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "branch-summary-hook.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("root")));
    REQUIRE(store->append(user_msg("branch-a")));
    REQUIRE(store->append(user_msg("branch-b")));

    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());

    std::string root_id = tree->entries()[0].entry_id;
    std::string leaf_before = tree->leaf_id();

    // Hook that returns a summary.
    std::vector<harness::session::SessionEntry> appended_entries;
    harness::session::SessionTree::BranchSummaryHook hook =
        [](const harness::session::SessionTree::BranchSummaryContext& ctx)
        -> support::Expected<std::optional<harness::session::SessionTree::BranchSummaryData>> {
        harness::session::SessionTree::BranchSummaryData data;
        data.summary = "summarized " + std::to_string(ctx.branch_entries.size()) + " entries";
        return data;
    };

    auto append_writer = [&](const harness::session::SessionEntry& entry) -> support::ExpectedVoid {
        appended_entries.push_back(entry);
        return {};
    };

    auto result = tree->branchWithSummary(root_id, hook, std::move(append_writer));
    REQUIRE(result.has_value());

    // Leaf should now be root_id
    CHECK(tree->leaf_id() == root_id);

    // A branch summary should have been appended
    REQUIRE(appended_entries.size() == 1);
    CHECK(appended_entries[0].kind == harness::session::SessionEntryKind::BranchSummary);
    CHECK(appended_entries[0].parent_id == root_id);
    REQUIRE(std::holds_alternative<harness::session::BranchSummaryEntryValue>(appended_entries[0].value));
    const auto& value = std::get<harness::session::BranchSummaryEntryValue>(appended_entries[0].value);
    CHECK(value.from_id == leaf_before);
    CHECK(value.summary == "summarized 2 entries");
}

TEST_CASE("branchWithSummary nullopt skips summary", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "skip-summary.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("root")));
    REQUIRE(store->append(user_msg("leaf")));

    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());

    std::string root_id = tree->entries()[0].entry_id;

    // Hook that returns nullopt (skip summary).
    harness::session::SessionTree::BranchSummaryHook hook =
        [](const harness::session::SessionTree::BranchSummaryContext&)
        -> support::Expected<std::optional<harness::session::SessionTree::BranchSummaryData>> {
        return std::nullopt;
    };

    bool writer_called = false;
    auto append_writer = [&](const harness::session::SessionEntry&) -> support::ExpectedVoid {
        writer_called = true;
        return {};
    };

    auto result = tree->branchWithSummary(root_id, hook, std::move(append_writer));
    REQUIRE(result.has_value());
    CHECK(tree->leaf_id() == root_id);
    CHECK_FALSE(writer_called);  // No entry written
}

TEST_CASE("branchWithSummary rejects unknown target", "[harness][session][tree]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "bad-target.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, test_metadata(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_msg("msg")));

    auto tree = harness::session::JsonlSessionStore::open_as_tree(path);
    REQUIRE(tree.has_value());

    harness::session::SessionTree::BranchSummaryHook hook =
        [](const harness::session::SessionTree::BranchSummaryContext&)
        -> support::Expected<std::optional<harness::session::SessionTree::BranchSummaryData>> {
        return std::nullopt;
    };
    auto append_writer = [](const harness::session::SessionEntry&) -> support::ExpectedVoid { return {}; };

    auto result = tree->branchWithSummary("deadbeef", hook, std::move(append_writer));
    REQUIRE_FALSE(result.has_value());
}
