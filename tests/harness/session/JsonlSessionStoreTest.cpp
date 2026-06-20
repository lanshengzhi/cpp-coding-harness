#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/util/Json.hpp"
#include "../../support/TempWorkspace.hpp"

#include <fstream>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace cch;

namespace {
harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-10T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

ai::MessageVariant user_message(std::string content) {
    return ai::MessageVariant{ai::user_text_message(std::move(content))};
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string text_from_message(const ai::MessageVariant& message) {
    if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
        const auto& text = std::get<ai::TextContent>(user->content.front());
        return text.text;
    }
    if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
        const auto& text = std::get<ai::TextContent>(assistant->content.front());
        return text.text;
    }
    return {};
}

void make_private(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}
} // namespace

TEST_CASE("Glaze JSONL session writes header and typed message entries", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "new.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_message("hello")));

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == "session-test");
    REQUIRE(loaded->messages.size() == 1);
    CHECK(text_from_message(loaded->messages[0]) == "hello");
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[0].kind == harness::session::SessionEntryKind::Header);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Message);
}

TEST_CASE("Glaze JSONL session redacts sensitive message fields at persistence boundary", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "redacted.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);

    REQUIRE(store->append(ai::MessageVariant{ai::SystemMessage{"system token=abc123", 1}}));
    REQUIRE(store->append(user_message("user api_key=sk-secret12345 KIMI_API_KEY=kimi-user-secret")));

    auto arguments = util::read_json<util::JsonValue>(
        R"({"api_key":"sk-toolsecret123","KIMI_API_KEY":"kimi-secret-value","nested":{"kimi_api_key":"kimi-nested-argument"},"path":"secret.txt"})");
    REQUIRE(arguments);
    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{"assistant password=hunter2 kimi_api_key=kimi-text-secret", std::nullopt});
    assistant.content.emplace_back(ai::ToolCallContent{
        "call-1",
        "write_file",
        *arguments,
        R"({"api_key":"sk-toolsecret123","KIMI_API_KEY":"kimi-secret-value","path":"secret.txt"})",
        std::nullopt,
        true,
        std::nullopt,
    });
    assistant.error_message = "provider error kimi_api_key=kimi-error-secret";
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    auto details = util::read_json<util::JsonValue>(
        R"({"token":"sk-detailsecret123","safe":"kept","nested":{"KIMI_API_KEY":"kimi-detail-secret","array":[{"kimi_api_key":"kimi-array-secret"}]}})");
    REQUIRE(details);
    ai::ToolResultMessage tool;
    tool.tool_call_id = "call-1";
    tool.tool_name = "write_file";
    tool.content.emplace_back(ai::TextContent{"tool secret=plain-secret KIMI_API_KEY=kimi-tool-content", std::nullopt});
    tool.details = *details;
    REQUIRE(store->append(ai::MessageVariant{tool}));

    const auto raw = read_all(path);
    CHECK(raw.find("sk-secret12345") == std::string::npos);
    CHECK(raw.find("sk-toolsecret123") == std::string::npos);
    CHECK(raw.find("sk-detailsecret123") == std::string::npos);
    CHECK(raw.find("hunter2") == std::string::npos);
    CHECK(raw.find("kimi-user-secret") == std::string::npos);
    CHECK(raw.find("kimi-secret-value") == std::string::npos);
    CHECK(raw.find("kimi-nested-argument") == std::string::npos);
    CHECK(raw.find("kimi-text-secret") == std::string::npos);
    CHECK(raw.find("kimi-error-secret") == std::string::npos);
    CHECK(raw.find("kimi-detail-secret") == std::string::npos);
    CHECK(raw.find("kimi-array-secret") == std::string::npos);
    CHECK(raw.find("kimi-tool-content") == std::string::npos);
    CHECK(raw.find("[REDACTED]") != std::string::npos);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->messages.size() == 4);
    CHECK(text_from_message(loaded->messages[1]).find("[REDACTED]") != std::string::npos);
}

TEST_CASE("Glaze JSONL session keeps unknown future entries", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "future.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    {
        std::ofstream output(path, std::ios::app);
        output << "{\"type\":\"future\",\"payload\":42}\n";
    }
    REQUIRE(store->append(user_message("known")));

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded);
    REQUIRE(loaded->unknown_lines.size() == 1);
    REQUIRE(loaded->entries.size() == 3);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Unknown);
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::Message);
    CHECK(text_from_message(loaded->messages[0]) == "known");
}

TEST_CASE("Glaze JSONL session parses v3 tree metadata entries", "[harness][session][u8]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "v3-tree.jsonl";
    {
        std::ofstream output(path);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"sess-v3\",\"timestamp\":\"2026-06-16T00:00:00.000Z\",\"cwd\":\""
               << workspace.path().string() << "\"}\n";
        output << "{\"type\":\"model_change\",\"id\":\"model001\",\"parentId\":null,\"timestamp\":\"2026-06-16T00:00:01.000Z\",\"provider\":\"openai\",\"modelId\":\"gpt-4o\"}\n";
        output << "{\"type\":\"thinking_level_change\",\"id\":\"think001\",\"parentId\":\"model001\",\"timestamp\":\"2026-06-16T00:00:02.000Z\",\"thinkingLevel\":\"high\"}\n";
    }
    make_private(path);

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == "sess-v3");
    CHECK(loaded->metadata.workspace == workspace.path());
    CHECK(loaded->messages.empty());
    REQUIRE(loaded->entries.size() == 3);
    CHECK(loaded->entries[0].kind == harness::session::SessionEntryKind::Header);
    CHECK(loaded->entries[0].entry_id == "sess-v3");
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ModelChange);
    CHECK(loaded->entries[1].entry_id == "model001");
    CHECK_FALSE(loaded->entries[1].parent_id.has_value());
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::ThinkingLevelChange);
    REQUIRE(loaded->entries[2].parent_id.has_value());
    CHECK(*loaded->entries[2].parent_id == "model001");
    CHECK(loaded->unknown_lines.empty());

    auto opened = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(opened);
    CHECK(opened->metadata().session_id == "sess-v3");
    // After U4 (resume gate removal), sessions with tree entries can be resumed.
    // Append a message to verify linear appends still work on a resumed tree session.
    REQUIRE(opened->append(user_message("resumed after tree entries")));
    auto reloaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(reloaded);
    CHECK(reloaded->entries.size() == 4);  // header + model_change + thinking_level_change + message
    CHECK(reloaded->messages.size() == 1);
    CHECK(text_from_message(reloaded->messages[0]) == "resumed after tree entries");
}

TEST_CASE("Glaze JSONL session reports malformed line context", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "bad.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    {
        std::ofstream output(path, std::ios::app);
        output << "not-json\n";
    }

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == util::ErrorCode::Session);
    CHECK(loaded.error().detail.find("line 2") != std::string::npos);
}

TEST_CASE("Glaze JSONL session reports missing entry discriminator", "[harness][session][u3]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "missing-type.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    {
        std::ofstream output(path, std::ios::app);
        output << R"({"payload":42})" << '\n';
    }

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE_FALSE(loaded);
    CHECK(loaded.error().code == util::ErrorCode::Session);
    CHECK(loaded.error().message == "session entry missing type");
    CHECK(loaded.error().detail.find("line 2") != std::string::npos);
}

TEST_CASE("Glaze JSONL session rejects symlink and public readable files", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto real = workspace.path() / "real.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(real, metadata_for(workspace));
    REQUIRE(store);

#if defined(__unix__) || defined(__APPLE__)
    auto link = workspace.path() / "link.jsonl";
    ::symlink(real.c_str(), link.c_str());
    auto linked = harness::session::JsonlSessionStore::load(link);
    REQUIRE_FALSE(linked);
    CHECK(linked.error().message.find("symlink") != std::string::npos);

    chmod(real.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
    auto public_load = harness::session::JsonlSessionStore::load(real);
    REQUIRE_FALSE(public_load);
    CHECK(public_load.error().message.find("readable") != std::string::npos);
#endif
}

// --- U5: v3 tree entry write round-trip tests ---

namespace {
bool is_hex8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
} // namespace

TEST_CASE("v3 session header writes and loads correctly", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "v3-header.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    CHECK(loaded->metadata.session_id == "session-test");
    CHECK(loaded->metadata.workspace == workspace.path());
    CHECK(loaded->entries.size() == 1);
    CHECK(loaded->entries[0].kind == harness::session::SessionEntryKind::Header);

    // Verify v3 header JSON format
    const auto raw = read_all(path);
    CHECK(raw.find("\"type\":\"session\"") != std::string::npos);
    CHECK(raw.find("\"version\":3") != std::string::npos);
}

TEST_CASE("entry IDs are 8-char random hex", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "id-format.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_message("test")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() >= 2);
    CHECK(is_hex8(loaded->entries[1].entry_id));
}

TEST_CASE("model_change entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "model-change.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ModelChange);
    CHECK(is_hex8(loaded->entries[1].entry_id));
    CHECK_FALSE(loaded->entries[1].parent_id.has_value());
    CHECK(loaded->entries[1].payload.get<util::JsonValue::object_t>().at("provider").get<std::string>() == "openai");
    CHECK(loaded->entries[1].payload.get<util::JsonValue::object_t>().at("modelId").get<std::string>() == "gpt-4o");
    CHECK(loaded->messages.empty());
}

TEST_CASE("thinking_level_change entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "thinking-change.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_thinking_level_change("parent01", "high"));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ThinkingLevelChange);
    REQUIRE(loaded->entries[1].parent_id.has_value());
    CHECK(*loaded->entries[1].parent_id == "parent01");
    CHECK(loaded->entries[1].payload.get<util::JsonValue::object_t>().at("thinkingLevel").get<std::string>() == "high");
}

TEST_CASE("active_tools_change entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "tools-change.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_active_tools_change(std::nullopt, {"read", "write", "bash"}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ActiveToolsChange);
    const auto& tools_arr = loaded->entries[1].payload.get<util::JsonValue::object_t>().at("tools").get<util::JsonValue::array_t>();
    REQUIRE(tools_arr.size() == 3);
    CHECK(tools_arr[0].get<std::string>() == "read");
    CHECK(tools_arr[1].get<std::string>() == "write");
    CHECK(tools_arr[2].get<std::string>() == "bash");
}

TEST_CASE("custom entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "custom.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    auto data = util::JsonValue{util::JsonValue::object_t{{"count", util::JsonValue{42}}, {"name", util::JsonValue{"test"}}}};
    REQUIRE(store->append_custom_entry(std::nullopt, "my-ext", std::move(data)));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Custom);
    const auto& obj = loaded->entries[1].payload.get<util::JsonValue::object_t>();
    CHECK(obj.at("customType").get<std::string>() == "my-ext");
    CHECK(obj.at("data").get<util::JsonValue::object_t>().at("count").get<double>() == 42.0);
}

TEST_CASE("custom_message entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "custom-msg.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    auto details = util::JsonValue{util::JsonValue::object_t{{"key", util::JsonValue{"val"}}}};
    REQUIRE(store->append_custom_message_entry("parent99", "my-ext", "injected content", true, std::move(details)));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::CustomMessage);
    const auto& obj = loaded->entries[1].payload.get<util::JsonValue::object_t>();
    CHECK(obj.at("customType").get<std::string>() == "my-ext");
    CHECK(obj.at("content").get<std::string>() == "injected content");
    CHECK(obj.at("display").get<bool>() == true);
    REQUIRE(loaded->entries[1].parent_id.has_value());
    CHECK(*loaded->entries[1].parent_id == "parent99");
}

TEST_CASE("label entry round-trips with set and clear", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "label.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    // Set a label
    REQUIRE(store->append_label_change(std::nullopt, "target-entry", std::string{"checkpoint-1"}));
    // Clear it (null label)
    REQUIRE(store->append_label_change("prev-id", "target-entry", std::nullopt));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 3);  // header + 2 labels
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Label);
    const auto& set_obj = loaded->entries[1].payload.get<util::JsonValue::object_t>();
    CHECK(set_obj.at("label").get<std::string>() == "checkpoint-1");
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::Label);
    // Null label should omit the field (Glaze skips std::nullopt by default)
    const auto raw = read_all(path);
    CHECK(raw.find("checkpoint-1") != std::string::npos);
    // Second label entry should have label absent
    auto clear_obj = loaded->entries[2].payload.get<util::JsonValue::object_t>();
    CHECK(clear_obj.find("label") == clear_obj.end());
}

TEST_CASE("compaction entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "compaction.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    auto details = util::JsonValue{util::JsonValue::object_t{{"readFiles", util::JsonValue{util::JsonValue::array_t{util::JsonValue{"a.txt"}}}}}};
    REQUIRE(store->append_compaction(std::nullopt, "summary text", "first-kept", 50000, std::move(details), true));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Compaction);
    const auto& obj = loaded->entries[1].payload.get<util::JsonValue::object_t>();
    CHECK(obj.at("summary").get<std::string>() == "summary text");
    CHECK(obj.at("firstKeptEntryId").get<std::string>() == "first-kept");
    CHECK(obj.at("tokensBefore").get<double>() == 50000.0);
    CHECK(obj.at("fromHook").get<bool>() == true);
}

TEST_CASE("branch_summary entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "branch-summary.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    auto details = util::JsonValue{util::JsonValue::object_t{{"modifiedFiles", util::JsonValue{util::JsonValue::array_t{util::JsonValue{"b.txt"}}}}}};
    REQUIRE(store->append_branch_summary("from-branch", "branch-id", "branch explored X", std::move(details), std::nullopt));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::BranchSummary);
    const auto& obj = loaded->entries[1].payload.get<util::JsonValue::object_t>();
    CHECK(obj.at("fromId").get<std::string>() == "branch-id");
    CHECK(obj.at("summary").get<std::string>() == "branch explored X");
    CHECK(obj.find("fromHook") == obj.end());  // not present when std::nullopt
}

TEST_CASE("session_info entry round-trips", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "session-info.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_session_info(std::nullopt, "Refactor auth module"));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::SessionInfo);
    CHECK(loaded->entries[1].payload.get<util::JsonValue::object_t>().at("name").get<std::string>() == "Refactor auth module");
}

TEST_CASE("mixed tree entries and messages round-trip in order", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "mixed.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    // Simulate a session: model_change → thinking_level_change → user message → assistant message
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change("mc-id", "high"));
    REQUIRE(store->append(user_message("hello")));
    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{"hi there", std::nullopt});
    assistant.provider = "openai";
    assistant.model = "gpt-4o";
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 5);  // header + 4 entries
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ModelChange);
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::ThinkingLevelChange);
    CHECK(loaded->entries[3].kind == harness::session::SessionEntryKind::Message);
    CHECK(loaded->entries[4].kind == harness::session::SessionEntryKind::Message);
    CHECK(loaded->messages.size() == 2);
    CHECK(text_from_message(loaded->messages[0]) == "hello");
}

TEST_CASE("open_existing succeeds and allows append on session with tree entries", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "resume-tree.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append(user_message("first message")));

    // Resume the session
    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append(user_message("second message")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 4);  // header + model_change + 2 messages
    CHECK(loaded->messages.size() == 2);
    CHECK(text_from_message(loaded->messages[0]) == "first message");
    CHECK(text_from_message(loaded->messages[1]) == "second message");
}

TEST_CASE("extended message types survive session append and load", "[harness][session][extended]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "extended-messages.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);

    // Append compaction summary
    ai::CompactionSummaryMessage compaction;
    compaction.summary = "Compacted 5 messages";
    compaction.tokens_before = 2000;
    compaction.timestamp = 1718000000001;
    REQUIRE(store->append(ai::MessageVariant{compaction}));

    // Append branch summary
    ai::BranchSummaryMessage branch;
    branch.summary = "Branch resolved";
    branch.from_id = "abc12345";
    branch.timestamp = 1718000000002;
    REQUIRE(store->append(ai::MessageVariant{branch}));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    // header + 2 messages
    REQUIRE(loaded->messages.size() == 2);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(loaded->messages[0]));
    CHECK(std::get<ai::CompactionSummaryMessage>(loaded->messages[0]).summary == "Compacted 5 messages");
    CHECK(std::get<ai::CompactionSummaryMessage>(loaded->messages[0]).tokens_before == 2000);

    REQUIRE(std::holds_alternative<ai::BranchSummaryMessage>(loaded->messages[1]));
    CHECK(std::get<ai::BranchSummaryMessage>(loaded->messages[1]).summary == "Branch resolved");
    CHECK(std::get<ai::BranchSummaryMessage>(loaded->messages[1]).from_id == "abc12345");
}
