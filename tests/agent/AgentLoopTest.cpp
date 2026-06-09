#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/AgentLoop.hpp"
#include "../support/FakeChatClient.hpp"

using namespace cch;

TEST_CASE("one-shot prompt records user and assistant messages", "[agent][u1]") {
    tests::FakeChatClient client;
    client.push_response(tests::text_response("hello from fake"));

    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("say hello");

    REQUIRE(result.ok());
    CHECK(result.value().final_text == "hello from fake");
    REQUIRE(result.value().messages.size() == 2);
    CHECK(result.value().messages[0].role == agent::Role::User);
    CHECK(result.value().messages[0].content == "say hello");
    CHECK(result.value().messages[1].role == agent::Role::Assistant);
    CHECK(client.requests.size() == 1);
    CHECK(client.requests[0].messages[0].content == "say hello");
}

TEST_CASE("empty assistant text completes without formatting crash", "[agent][u1]") {
    tests::FakeChatClient client;
    client.push_response(tests::text_response(""));

    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("empty is ok");

    REQUIRE(result.ok());
    CHECK(result.value().final_text.empty());
    CHECK(result.value().stop_reason == "stop");
}

TEST_CASE("chat client error becomes structured loop failure", "[agent][u1]") {
    tests::FakeChatClient client;
    client.fail_next("provider unavailable");

    agent::ToolRegistry registry;
    agent::AgentLoop loop(client, registry);

    auto result = loop.run("will fail");

    REQUIRE_FALSE(result.ok());
    CHECK(result.error() == "provider unavailable");
}
