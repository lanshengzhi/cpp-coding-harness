#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/harness/session/SessionResume.hpp"
#include "util/Json.hpp"
#include "../../support/TempWorkspace.hpp"

#include <fstream>
#include <sstream>
#include <variant>

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
        const auto& text = std::get<ai::TextContent>(
            std::get<std::vector<ai::Content>>(user->content).front());
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

util::JsonValue complete_assistant_value() {
    return util::JsonValue{util::JsonValue::object_t{
        {"role", util::JsonValue{"assistant"}},
        {"content", util::JsonValue{util::JsonValue::array_t{
            util::JsonValue{util::JsonValue::object_t{
                {"type", util::JsonValue{"text"}},
                {"text", util::JsonValue{"persisted answer"}},
            }},
        }}},
        {"api", util::JsonValue{"openai-completions"}},
        {"provider", util::JsonValue{"openai-compatible"}},
        {"model", util::JsonValue{"gpt-test"}},
        {"usage", util::JsonValue{util::JsonValue::object_t{
            {"input", util::JsonValue{11}},
            {"output", util::JsonValue{7}},
            {"cacheRead", util::JsonValue{3}},
            {"cacheWrite", util::JsonValue{2}},
            {"reasoning", util::JsonValue{5}},
            {"totalTokens", util::JsonValue{23}},
            {"cost", util::JsonValue{util::JsonValue::object_t{
                {"input", util::JsonValue{0.11}},
                {"output", util::JsonValue{0.07}},
                {"cacheRead", util::JsonValue{0.03}},
                {"cacheWrite", util::JsonValue{0.02}},
                {"total", util::JsonValue{0.23}},
            }}},
        }}},
        {"stopReason", util::JsonValue{"stop"}},
        {"timestamp", util::JsonValue{1718000000123.0}},
    }};
}

void write_resume_fixture(
    const std::filesystem::path& path,
    util::JsonValue assistant) {
    util::JsonValue header{util::JsonValue::object_t{
        {"type", util::JsonValue{"session"}},
        {"version", util::JsonValue{3}},
        {"id", util::JsonValue{"session-usage"}},
        {"timestamp", util::JsonValue{"2026-06-10T00:00:00.000Z"}},
        {"cwd", util::JsonValue{path.parent_path().string()}},
        {"provider", util::JsonValue{"openai-compatible"}},
        {"model", util::JsonValue{"gpt-test"}},
    }};
    util::JsonValue entry{util::JsonValue::object_t{
        {"type", util::JsonValue{"message"}},
        {"entryId", util::JsonValue{"msg00001"}},
        {"message", std::move(assistant)},
    }};
    auto header_json = util::write_json(header);
    auto entry_json = util::write_json(entry);
    REQUIRE(header_json);
    REQUIRE(entry_json);

    std::ofstream output(path);
    output << *header_json << '\n' << *entry_json << '\n';
    output.close();
    make_private(path);
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
    assistant.api = "openai-completions";
    assistant.provider = "openai-compatible";
    assistant.model = "gpt-test";
    assistant.error_message = "provider error kimi_api_key=kimi-error-secret";
    assistant.timestamp = 1718000000123;
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

    std::vector<harness::session::CustomMessageEntryContentBlock> custom_content;
    custom_content.emplace_back(ai::text_content("custom token=custom-secret"));
    custom_content.emplace_back(ai::ImageContent{"aW1hZ2UtYnl0ZXM=", "image/png"});
    REQUIRE(store->append_custom_message_entry(
        std::nullopt,
        "redaction-test",
        std::move(custom_content),
        true,
        std::nullopt));

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
    CHECK(raw.find("custom-secret") == std::string::npos);
    CHECK(raw.find("aW1hZ2UtYnl0ZXM=") != std::string::npos);
    CHECK(raw.find("[REDACTED]") != std::string::npos);

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->messages.size() == 4);
    CHECK(text_from_message(loaded->messages[1]).find("[REDACTED]") != std::string::npos);
}

TEST_CASE(
    "Session Resume restores complete assistant identity and usage",
    "[harness][session][resume][issue17][issue19]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "complete-usage.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);

    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{"persisted answer", std::nullopt});
    assistant.api = "openai-completions";
    assistant.provider = "openai-compatible";
    assistant.model = "gpt-test";
    assistant.response_model = "routed-model";
    assistant.response_id = "response-123";
    assistant.usage = ai::Usage{
        .input = 11,
        .output = 7,
        .cache_read = 3,
        .cache_write = 2,
        .cache_write_1h = 1,
        .reasoning = 5,
        .total_tokens = 23,
        .cost = ai::UsageCost{
            .input = 0.11,
            .output = 0.07,
            .cache_read = 0.03,
            .cache_write = 0.02,
            .total = 0.23,
        },
    };
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.timestamp = 1718000000123;
    REQUIRE(store->append(ai::MessageVariant{assistant}));

    const auto persisted = read_all(path);
    CHECK(persisted.find(R"("api":"openai-completions")") != std::string::npos);
    CHECK(persisted.find(R"("provider":"openai-compatible")") != std::string::npos);
    CHECK(persisted.find(R"("model":"gpt-test")") != std::string::npos);
    CHECK(persisted.find(R"("responseModel":"routed-model")") != std::string::npos);
    CHECK(persisted.find(R"("responseId":"response-123")") != std::string::npos);
    CHECK(persisted.find(R"("timestamp":1718000000123)") != std::string::npos);
    CHECK(persisted.find(R"("usage":)") != std::string::npos);
    CHECK(persisted.find(R"("input":11)") != std::string::npos);
    CHECK(persisted.find(R"("output":7)") != std::string::npos);
    CHECK(persisted.find(R"("cacheRead":3)") != std::string::npos);
    CHECK(persisted.find(R"("cacheWrite":2)") != std::string::npos);
    CHECK(persisted.find(R"("reasoning":5)") != std::string::npos);
    CHECK(persisted.find(R"("totalTokens":23)") != std::string::npos);
    CHECK(persisted.find(R"("cost":)") != std::string::npos);

    auto resumed = harness::session::resume_session(path);
    REQUIRE(resumed);
    REQUIRE(resumed->history.size() == 1);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(resumed->history[0]));
    const auto& restored = std::get<ai::AssistantMessage>(resumed->history[0]);
    CHECK(restored.api == "openai-completions");
    CHECK(restored.provider == "openai-compatible");
    CHECK(restored.model == "gpt-test");
    CHECK(restored.response_model == "routed-model");
    CHECK(restored.response_id == "response-123");
    CHECK(restored.timestamp == 1718000000123);
    CHECK(restored.usage.input == 11);
    CHECK(restored.usage.output == 7);
    CHECK(restored.usage.cache_read == 3);
    CHECK(restored.usage.cache_write == 2);
    CHECK(restored.usage.cache_write_1h == 1);
    CHECK(restored.usage.reasoning == 5);
    CHECK(restored.usage.total_tokens == 23);
    CHECK(restored.usage.cost.input == 0.11);
    CHECK(restored.usage.cost.output == 0.07);
    CHECK(restored.usage.cost.cache_read == 0.03);
    CHECK(restored.usage.cost.cache_write == 0.02);
    CHECK(restored.usage.cost.total == 0.23);
}

TEST_CASE(
    "Session Resume rejects incomplete assistant identity and non-real timestamps",
    "[harness][session][resume][issue19]") {
    tests::TempWorkspace workspace;
    int fixture_index = 0;
    const auto rejected = [&](util::JsonValue message, std::string expected_detail) {
        const auto path = workspace.path() /
            ("incomplete-identity-" + std::to_string(fixture_index++) + ".jsonl");
        write_resume_fixture(path, std::move(message));
        auto resumed = harness::session::resume_session(path);
        REQUIRE_FALSE(resumed);
        CHECK(resumed.error().code == util::ErrorCode::JsonParse);
        CHECK(resumed.error().detail.find(expected_detail) != std::string::npos);
    };

    for (const auto& field : {"api", "provider", "model"}) {
        auto missing = complete_assistant_value();
        missing.get_object().erase(field);
        rejected(std::move(missing), field);

        auto empty = complete_assistant_value();
        empty.at(field) = util::JsonValue{""};
        rejected(std::move(empty), field);
    }

    auto missing_timestamp = complete_assistant_value();
    missing_timestamp.get_object().erase("timestamp");
    rejected(std::move(missing_timestamp), "timestamp");

    for (const auto timestamp : {0, -1, 1718000000}) {
        auto invalid = complete_assistant_value();
        invalid.at("timestamp") = util::JsonValue{timestamp};
        rejected(std::move(invalid), "timestamp");
    }
}

TEST_CASE(
    "Session Resume rejects missing required assistant usage fields",
    "[harness][session][resume][issue17]") {
    tests::TempWorkspace workspace;
    int fixture_index = 0;
    const auto rejected = [&](util::JsonValue message, std::string expected_field) {
        const auto path = workspace.path() /
            ("incomplete-usage-" + std::to_string(fixture_index++) + ".jsonl");
        write_resume_fixture(path, std::move(message));
        auto resumed = harness::session::resume_session(path);
        REQUIRE_FALSE(resumed);
        CHECK(resumed.error().code == util::ErrorCode::JsonParse);
        CHECK(resumed.error().detail.find(expected_field) != std::string::npos);
    };

    auto missing_usage = complete_assistant_value();
    missing_usage.get_object().erase("usage");
    rejected(std::move(missing_usage), "usage");

    for (const auto& field : {"input", "output", "cacheRead", "cacheWrite", "totalTokens"}) {
        auto message = complete_assistant_value();
        message.at("usage").get_object().erase(field);
        rejected(std::move(message), field);
    }

    auto missing_cost = complete_assistant_value();
    missing_cost.at("usage").get_object().erase("cost");
    rejected(std::move(missing_cost), "cost");

    for (const auto& field : {"input", "output", "cacheRead", "cacheWrite", "total"}) {
        auto message = complete_assistant_value();
        message.at("usage").at("cost").get_object().erase(field);
        rejected(std::move(message), field);
    }
}

TEST_CASE(
    "Session Resume propagates missing and unsupported assistant stop reasons",
    "[harness][session][resume][issue18]") {
    tests::TempWorkspace workspace;
    int fixture_index = 0;
    const auto rejected = [&](util::JsonValue message, std::string expected_detail) {
        const auto path = workspace.path() /
            ("invalid-stop-reason-" + std::to_string(fixture_index++) + ".jsonl");
        write_resume_fixture(path, std::move(message));
        auto resumed = harness::session::resume_session(path);
        REQUIRE_FALSE(resumed);
        CHECK(resumed.error().code == util::ErrorCode::JsonParse);
        CHECK(resumed.error().detail.find(expected_detail) != std::string::npos);
    };

    auto missing = complete_assistant_value();
    missing.get_object().erase("stopReason");
    rejected(std::move(missing), "stopReason");

    auto unsupported = complete_assistant_value();
    unsupported.at("stopReason") = util::JsonValue{"future_reason"};
    rejected(std::move(unsupported), "future_reason");
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
    CHECK(std::holds_alternative<std::monostate>(loaded->entries[1].value));
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
    REQUIRE(std::holds_alternative<harness::session::ModelChangeValue>(loaded->entries[1].value));
    const auto& model_value = std::get<harness::session::ModelChangeValue>(loaded->entries[1].value);
    CHECK(model_value.provider == "openai");
    CHECK(model_value.model_id == "gpt-4o");
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::ThinkingLevelChange);
    REQUIRE(loaded->entries[2].parent_id.has_value());
    CHECK(*loaded->entries[2].parent_id == "model001");
    REQUIRE(std::holds_alternative<harness::session::ThinkingLevelChangeValue>(loaded->entries[2].value));
    const auto& thinking_value = std::get<harness::session::ThinkingLevelChangeValue>(loaded->entries[2].value);
    CHECK(thinking_value.thinking_level == "high");
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

TEST_CASE("Glaze JSONL session rejects an absolute parent path containing a symlink", "[harness][session][u7]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    const auto real_directory = workspace.path() / "real";
    const auto linked_directory = workspace.path() / "linked";
    std::filesystem::create_directory(real_directory);
    REQUIRE(::symlink(real_directory.c_str(), linked_directory.c_str()) == 0);

    const auto target = linked_directory / "nested" / "session.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(target, metadata_for(workspace));

    REQUIRE_FALSE(store);
    CHECK((store.error().message + store.error().detail).find("symlink") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(real_directory / "nested"));
#else
    SUCCEED("symbolic-link parent assertion is not available on this platform");
#endif
}

TEST_CASE("Glaze JSONL append rejects a parent replaced by a symlink", "[harness][session][u7]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    const auto original_directory = workspace.path() / "sessions";
    const auto moved_directory = workspace.path() / "moved-sessions";
    const auto outside_directory = workspace.path() / "outside";
    std::filesystem::create_directory(original_directory);
    std::filesystem::create_directory(outside_directory);
    const auto path = original_directory / "session.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);

    std::filesystem::rename(original_directory, moved_directory);
    REQUIRE(::symlink(outside_directory.c_str(), original_directory.c_str()) == 0);
    const auto outside_file = outside_directory / path.filename();
    {
        std::ofstream file(outside_file);
        file << "outside";
    }
    make_private(outside_file);

    auto appended = store->append(user_message("must not escape"));

    REQUIRE_FALSE(appended);
    CHECK((appended.error().message + appended.error().detail).find("symlink") != std::string::npos);
    CHECK(read_all(outside_file) == "outside");
#else
    SUCCEED("symbolic-link append assertion is not available on this platform");
#endif
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

TEST_CASE("Glaze JSONL session create_new retains exclusive file creation", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    const auto path = workspace.path() / "session.jsonl";

    auto first = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(first);
    auto second = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));

    REQUIRE_FALSE(second);
    CHECK(second.error().message.find("already exists") != std::string::npos);
}

TEST_CASE("Glaze JSONL session create_new rejects a symbolic link final target", "[harness][session][u7]") {
#if defined(__unix__) || defined(__APPLE__)
    tests::TempWorkspace workspace;
    const auto outside = workspace.path() / "outside.jsonl";
    {
        std::ofstream file(outside);
        file << "outside";
    }
    const auto link = workspace.path() / "link.jsonl";
    REQUIRE(::symlink(outside.c_str(), link.c_str()) == 0);

    auto store = harness::session::JsonlSessionStore::create_new(link, metadata_for(workspace));

    REQUIRE_FALSE(store);
    CHECK(store.error().message == "refusing to follow symlink session path");
#else
    SUCCEED("symbolic-link assertion is not available on this platform");
#endif
}

// --- U5: v3 typed entry load/write tests ---

namespace {
bool is_hex8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

template <typename Value>
const Value& require_entry_value(const harness::session::SessionEntry& entry) {
    REQUIRE(std::holds_alternative<Value>(entry.value));
    return std::get<Value>(entry.value);
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

TEST_CASE("serializer wire test keeps current JSONL field names", "[harness][session][wire]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "wire-fields.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_model_change(std::nullopt, "openai", "gpt-4o"));
    REQUIRE(store->append_thinking_level_change(std::nullopt, "high"));
    REQUIRE(store->append_active_tools_change(std::nullopt, {"read"}));
    REQUIRE(store->append_custom_entry(std::nullopt, "my-ext", util::JsonValue{nullptr}));
    REQUIRE(store->append_custom_message_entry(std::nullopt, "my-ext", "context", true, std::nullopt));
    REQUIRE(store->append_label_change(std::nullopt, "target-entry", std::string{"checkpoint"}));
    REQUIRE(store->append_compaction(std::nullopt, "summary", "first-kept", 123, std::nullopt, true));
    REQUIRE(store->append_branch_summary(std::nullopt, "from-entry", "branch summary", std::nullopt, false));
    REQUIRE(store->append_session_info(std::nullopt, "Session name"));
    REQUIRE(store->append_leaf(std::nullopt, "leaf-target"));

    const auto raw = read_all(path);
    CHECK(raw.find(R"("modelId":"gpt-4o")") != std::string::npos);
    CHECK(raw.find(R"("thinkingLevel":"high")") != std::string::npos);
    CHECK(raw.find(R"("tools":["read"])") != std::string::npos);
    CHECK(raw.find(R"("customType":"my-ext")") != std::string::npos);
    CHECK(raw.find(R"("targetId":"target-entry")") != std::string::npos);
    CHECK(raw.find(R"("firstKeptEntryId":"first-kept")") != std::string::npos);
    CHECK(raw.find(R"("tokensBefore":123)") != std::string::npos);
    CHECK(raw.find(R"("fromId":"from-entry")") != std::string::npos);
    CHECK(raw.find(R"("name":"Session name")") != std::string::npos);
    CHECK(raw.find(R"("targetId":"leaf-target")") != std::string::npos);
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
    const auto& value = require_entry_value<harness::session::ModelChangeValue>(loaded->entries[1]);
    CHECK(value.provider == "openai");
    CHECK(value.model_id == "gpt-4o");
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
    const auto& value = require_entry_value<harness::session::ThinkingLevelChangeValue>(loaded->entries[1]);
    CHECK(value.thinking_level == "high");
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
    const auto& value = require_entry_value<harness::session::ActiveToolsChangeValue>(loaded->entries[1]);
    REQUIRE(value.active_tool_names.size() == 3);
    CHECK(value.active_tool_names[0] == "read");
    CHECK(value.active_tool_names[1] == "write");
    CHECK(value.active_tool_names[2] == "bash");
}

TEST_CASE("wire parser accepts activeToolNames compatibility field", "[harness][session][wire]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "tools-change-compat.jsonl";
    {
        std::ofstream output(path);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"sess-v3\",\"timestamp\":\"2026-06-16T00:00:00.000Z\",\"cwd\":\""
               << workspace.path().string() << "\"}\n";
        output << "{\"type\":\"active_tools_change\",\"id\":\"tools001\",\"parentId\":null,\"timestamp\":\"2026-06-16T00:00:01.000Z\",\"activeToolNames\":[\"read\",\"bash\"]}\n";
    }
    make_private(path);

    auto loaded = harness::session::JsonlSessionStore::load(path);

    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::ActiveToolsChange);
    const auto& value = require_entry_value<harness::session::ActiveToolsChangeValue>(loaded->entries[1]);
    REQUIRE(value.active_tool_names.size() == 2);
    CHECK(value.active_tool_names[0] == "read");
    CHECK(value.active_tool_names[1] == "bash");
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
    const auto& value = require_entry_value<harness::session::CustomEntryValue>(loaded->entries[1]);
    CHECK(value.custom_type == "my-ext");
    CHECK(value.data.get<util::JsonValue::object_t>().at("count").get<double>() == 42.0);
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
    const auto& value = require_entry_value<harness::session::CustomMessageEntryValue>(loaded->entries[1]);
    CHECK(value.custom_type == "my-ext");
    REQUIRE(std::holds_alternative<std::string>(value.content));
    CHECK(std::get<std::string>(value.content) == "injected content");
    CHECK(value.display == true);
    REQUIRE(value.details.has_value());
    CHECK(value.details->get<util::JsonValue::object_t>().at("key").get<std::string>() == "val");
    REQUIRE(loaded->entries[1].parent_id.has_value());
    CHECK(*loaded->entries[1].parent_id == "parent99");
    CHECK(read_all(path).find("\"content\":\"injected content\"") != std::string::npos);
}

TEST_CASE(
    "Session Resume preserves ordered custom_message text and image content",
    "[harness][session][wire][issue28]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "custom-msg-images.jsonl";
    {
        std::ofstream output(path);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"sess-v3\",\"timestamp\":\"2026-07-22T00:00:00.000Z\",\"cwd\":\""
               << workspace.path().string() << "\"}\n";
        output << R"json({"type":"custom_message","id":"custom01","parentId":null,"timestamp":"2026-07-22T00:00:01.234Z","customType":"extension-image","content":[{"type":"text","text":"before"},{"type":"image","data":"cG5nLWJ5dGVz","mimeType":"image/png"},{"type":"text","text":"after"},{"type":"image","data":"d2VicC1ieXRlcw==","mimeType":"image/webp"}],"display":true})json"
               << '\n';
    }
    make_private(path);

    auto resumed = harness::session::resume_session(path);

    REQUIRE(resumed);
    REQUIRE(resumed->history.size() == 1);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(resumed->history[0]));
    const auto& custom = std::get<ai::CustomMessage>(resumed->history[0]);
    CHECK(custom.custom_type == "extension-image");
    CHECK(custom.timestamp == 1784678401234);
    REQUIRE(custom.content.size() == 4);
    CHECK(std::get<ai::TextContent>(custom.content[0]).text == "before");
    CHECK(std::get<ai::ImageContent>(custom.content[1]).mime_type == "image/png");
    CHECK(std::get<ai::ImageContent>(custom.content[1]).data == "cG5nLWJ5dGVz");
    CHECK(std::get<ai::TextContent>(custom.content[2]).text == "after");
    CHECK(std::get<ai::ImageContent>(custom.content[3]).mime_type == "image/webp");
    CHECK(std::get<ai::ImageContent>(custom.content[3]).data == "d2VicC1ieXRlcw==");
}

TEST_CASE(
    "pi v3 custom_message rejects content blocks outside text and image",
    "[harness][session][wire][issue28]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "custom-msg-thinking.jsonl";
    {
        std::ofstream output(path);
        output << "{\"type\":\"session\",\"version\":3,\"id\":\"sess-v3\",\"timestamp\":\"2026-07-22T00:00:00.000Z\",\"cwd\":\""
               << workspace.path().string() << "\"}\n";
        output << R"json({"type":"custom_message","id":"custom01","parentId":null,"timestamp":"2026-07-22T00:00:01.234Z","customType":"extension-thinking","content":[{"type":"thinking","thinking":"not valid custom content"}],"display":true})json"
               << '\n';
    }
    make_private(path);

    auto resumed = harness::session::resume_session(path);

    CHECK_FALSE(resumed.has_value());
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
    const auto& set_value = require_entry_value<harness::session::LabelEntryValue>(loaded->entries[1]);
    CHECK(set_value.target_id == "target-entry");
    REQUIRE(set_value.label.has_value());
    CHECK(*set_value.label == "checkpoint-1");
    CHECK(loaded->entries[2].kind == harness::session::SessionEntryKind::Label);
    // Null label should omit the field (Glaze skips std::nullopt by default)
    const auto raw = read_all(path);
    CHECK(raw.find("checkpoint-1") != std::string::npos);
    const auto& clear_value = require_entry_value<harness::session::LabelEntryValue>(loaded->entries[2]);
    CHECK(clear_value.target_id == "target-entry");
    CHECK_FALSE(clear_value.label.has_value());
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
    const auto& value = require_entry_value<harness::session::CompactionEntryValue>(loaded->entries[1]);
    CHECK(value.summary == "summary text");
    CHECK(value.first_kept_entry_id == "first-kept");
    CHECK(value.tokens_before == 50000);
    REQUIRE(value.details.has_value());
    const auto& read_files = value.details->get<util::JsonValue::object_t>()
        .at("readFiles")
        .get<util::JsonValue::array_t>();
    REQUIRE(read_files.size() == 1);
    CHECK(read_files[0].get<std::string>() == "a.txt");
    REQUIRE(value.from_hook.has_value());
    CHECK(*value.from_hook == true);
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
    const auto& value = require_entry_value<harness::session::BranchSummaryEntryValue>(loaded->entries[1]);
    CHECK(value.from_id == "branch-id");
    CHECK(value.summary == "branch explored X");
    REQUIRE(value.details.has_value());
    const auto& modified_files = value.details->get<util::JsonValue::object_t>()
        .at("modifiedFiles")
        .get<util::JsonValue::array_t>();
    REQUIRE(modified_files.size() == 1);
    CHECK(modified_files[0].get<std::string>() == "b.txt");
    CHECK_FALSE(value.from_hook.has_value());
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
    const auto& value = require_entry_value<harness::session::SessionInfoEntryValue>(loaded->entries[1]);
    CHECK(value.name == "Refactor auth module");
}

TEST_CASE("leaf entry round-trips as typed value", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "leaf-entry.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append_leaf(std::nullopt, "leaf-target"));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->entries.size() == 2);
    CHECK(loaded->entries[1].kind == harness::session::SessionEntryKind::Leaf);
    const auto& value = require_entry_value<harness::session::LeafEntryValue>(loaded->entries[1]);
    REQUIRE(value.target_id.has_value());
    CHECK(*value.target_id == "leaf-target");
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
    assistant.api = "openai-completions";
    assistant.provider = "openai";
    assistant.model = "gpt-4o";
    assistant.timestamp = 1718000000123;
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

TEST_CASE("open_existing appends after stale latest leaf marker at last navigable entry", "[harness][session][u9]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "stale-leaf-append.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store);
    REQUIRE(store->append(user_message("first")));
    REQUIRE(store->append(user_message("second")));
    REQUIRE(store->append(user_message("third")));

    auto pre = harness::session::JsonlSessionStore::load(path);
    REQUIRE(pre);
    REQUIRE(pre->entries.size() >= 4);
    const auto first_id = pre->entries[1].entry_id;
    const auto third_id = pre->entries[3].entry_id;

    auto resumed = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(resumed);
    REQUIRE(resumed->append_leaf(std::nullopt, first_id));
    REQUIRE(resumed->append_leaf(std::nullopt, "missing-entry"));

    auto reopened = harness::session::JsonlSessionStore::open_existing(path);
    REQUIRE(reopened);
    REQUIRE(reopened->append(user_message("after stale leaf")));

    auto loaded = harness::session::JsonlSessionStore::load(path);
    REQUIRE(loaded);

    const harness::session::SessionEntry* appended = nullptr;
    const harness::session::SessionEntry* latest_leaf = nullptr;
    for (const auto& entry : loaded->entries) {
        if (entry.kind == harness::session::SessionEntryKind::Message &&
            entry.message.has_value() &&
            text_from_message(*entry.message) == "after stale leaf") {
            appended = &entry;
        }
        if (entry.kind == harness::session::SessionEntryKind::Leaf) {
            latest_leaf = &entry;
        }
    }

    REQUIRE(appended != nullptr);
    REQUIRE(appended->parent_id.has_value());
    CHECK(*appended->parent_id == third_id);
    REQUIRE(latest_leaf != nullptr);
    const auto& leaf = require_entry_value<harness::session::LeafEntryValue>(*latest_leaf);
    REQUIRE(leaf.target_id.has_value());
    CHECK(*leaf.target_id == appended->entry_id);
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
