#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/AgentLoop.hpp"
#include "../../src/session/JsonlSessionStore.hpp"
#include "../../src/tools/Tools.hpp"
#include "../support/FakeChatClient.hpp"
#include "../support/TempWorkspace.hpp"

#include <boost/json.hpp>
#include <fstream>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

using namespace cch;

namespace {
session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return {"session-test", "2026-06-09T00:00:00Z", workspace.path(), "fake", "fake-model"};
}

agent::Message user_message(std::string content) {
    agent::Message message;
    message.role = agent::Role::User;
    message.content = std::move(content);
    return message;
}
}

TEST_CASE("new session writes header and appends redacted messages in order", "[session][u5]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "session.jsonl";
    auto store = session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store.ok());

    REQUIRE(store.value().append(user_message("hello sk-123456789SECRET")).ok());
    agent::Message assistant;
    assistant.role = agent::Role::Assistant;
    assistant.content = "done";
    REQUIRE(store.value().append(assistant).ok());

    auto loaded = session::JsonlSessionStore::load(path);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().messages.size() == 2);
    CHECK(loaded.value().messages[0].content.find("sk-123456789SECRET") == std::string::npos);
    CHECK(loaded.value().messages[0].content.find("[REDACTED]") != std::string::npos);
    CHECK(loaded.value().messages[1].content == "done");
}

TEST_CASE("header-only session resumes as empty history", "[session][u5]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "empty.jsonl";
    auto store = session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store.ok());

    auto loaded = session::JsonlSessionStore::load(path);

    REQUIRE(loaded.ok());
    CHECK(loaded.value().messages.empty());
}

TEST_CASE("malformed JSONL reports line number and leaves original file", "[session][u5]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "bad.jsonl";
    {
        auto store = session::JsonlSessionStore::create_new(path, metadata_for(workspace));
        REQUIRE(store.ok());
    }
    {
        std::ofstream output(path, std::ios::app);
        output << "not-json\n";
    }
#if defined(__unix__) || defined(__APPLE__)
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif

    auto loaded = session::JsonlSessionStore::load(path);

    REQUIRE_FALSE(loaded.ok());
    CHECK(loaded.error().find("line 2") != std::string::npos);
    std::ifstream input(path);
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(contents.find("not-json") != std::string::npos);
}

TEST_CASE("unknown future entry type is preserved but ignored for active history", "[session][u5]") {
    tests::TempWorkspace workspace;
    auto path = workspace.path() / "future.jsonl";
    auto store = session::JsonlSessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(store.ok());
    {
        std::ofstream output(path, std::ios::app);
        output << "{\"type\":\"future\",\"payload\":42}\n";
    }
    REQUIRE(store.value().append(user_message("known")).ok());

    auto loaded = session::JsonlSessionStore::load(path);

    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().unknown_lines.size() == 1);
    REQUIRE(loaded.value().messages.size() == 1);
    CHECK(loaded.value().messages[0].content == "known");
}

TEST_CASE("session path rejects symlink and public readable files", "[session][u5]") {
    tests::TempWorkspace workspace;
    auto real = workspace.path() / "real.jsonl";
    auto store = session::JsonlSessionStore::create_new(real, metadata_for(workspace));
    REQUIRE(store.ok());

    auto link = workspace.path() / "link.jsonl";
    std::error_code ec;
    std::filesystem::create_symlink(real, link, ec);
    if (!ec) {
        auto linked = session::JsonlSessionStore::load(link);
        CHECK_FALSE(linked.ok());
    }

#if defined(__unix__) || defined(__APPLE__)
    chmod(real.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    auto public_load = session::JsonlSessionStore::load(real);
    CHECK_FALSE(public_load.ok());
#endif
}

TEST_CASE("agent loop can persist, resume, and continue with redacted history", "[session][u5]") {
    tests::TempWorkspace workspace;
    workspace.write("note.txt", "secret token=sk-123456789SECRET");
    auto session_path = workspace.path() / "loop.jsonl";
    auto store = session::JsonlSessionStore::create_new(session_path, metadata_for(workspace));
    REQUIRE(store.ok());

    tests::FakeChatClient first_client;
    boost::json::object args;
    args["path"] = "note.txt";
    first_client.push_response(tests::tool_response("read-1", "read_file", args));
    first_client.push_response(tests::text_response("first done"));
    agent::ToolRegistry registry;
    registry.add(tools::make_read_file_tool());
    agent::LoopOptions options;
    options.workspace = workspace.path();
    options.on_message = [&](const agent::Message& message) { return store.value().append(message); };
    agent::AgentLoop first_loop(first_client, registry, options);
    REQUIRE(first_loop.run("inspect").ok());

    auto loaded = session::JsonlSessionStore::load(session_path);
    REQUIRE(loaded.ok());
    tests::FakeChatClient second_client;
    second_client.push_response(tests::text_response("continued"));
    agent::AgentLoop second_loop(second_client, agent::ToolRegistry{}, options);
    auto continued = second_loop.continue_with(loaded.value().messages, "continue");

    REQUIRE(continued.ok());
    REQUIRE(second_client.requests.size() == 1);
    bool saw_redacted_tool = false;
    for (const auto& message : second_client.requests[0].messages) {
        if (message.role == agent::Role::Tool) {
            CHECK(message.content.find("sk-123456789SECRET") == std::string::npos);
            saw_redacted_tool = true;
        }
    }
    CHECK(saw_redacted_tool);
}
