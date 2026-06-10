#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <cch/harness/session/JsonlSessionStore.hpp>
#include "../../support/TempWorkspace.hpp"

#include <fstream>
#include <variant>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cch;

namespace {
harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-10T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

ai::MessageVariant user_message(std::string content) {
    return ai::MessageVariant{ai::user_text_message(std::move(content))};
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

TEST_CASE("Glaze JSONL session rejects symlink and public readable files", "[harness][session][u7]") {
    tests::TempWorkspace workspace;
    auto real = workspace.path() / "real.jsonl";
    auto store = harness::session::JsonlSessionStore::create_new(real, metadata_for(workspace));
    REQUIRE(store);

#if defined(__unix__) || defined(__APPLE__)
    auto link = workspace.path() / "link.jsonl";
    symlink(real.c_str(), link.c_str());
    auto linked = harness::session::JsonlSessionStore::load(link);
    REQUIRE_FALSE(linked);
    CHECK(linked.error().message.find("symlink") != std::string::npos);

    chmod(real.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
    auto public_load = harness::session::JsonlSessionStore::load(real);
    REQUIRE_FALSE(public_load);
    CHECK(public_load.error().message.find("readable") != std::string::npos);
#endif
}
