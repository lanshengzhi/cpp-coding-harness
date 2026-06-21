#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "ai/providers/SseParser.hpp"

using namespace cch;

TEST_CASE("SSE parser emits one event from a complete frame", "[ai][provider][sse][u3]") {
    ai::providers::SseParser parser;

    auto events = parser.append("event: message\ndata: {\"text\":\"hello\"}\n\n");

    REQUIRE(events);
    REQUIRE(events->size() == 1);
    CHECK((*events)[0].event == "message");
    CHECK((*events)[0].data == R"({"text":"hello"})");
    CHECK_FALSE((*events)[0].done);
}

TEST_CASE("SSE parser preserves events across fragmented CRLF buffers", "[ai][provider][sse][u3]") {
    ai::providers::SseParser parser;

    auto first = parser.append("eve");
    REQUIRE(first);
    CHECK(first->empty());

    auto second = parser.append("nt: update\r\nda");
    REQUIRE(second);
    CHECK(second->empty());

    auto third = parser.append("ta: part-1\r\ndata: part-2\r\n\r\n");
    REQUIRE(third);
    REQUIRE(third->size() == 1);
    CHECK((*third)[0].event == "update");
    CHECK((*third)[0].data == "part-1\npart-2");
}

TEST_CASE("SSE parser ignores comments and joins multiple data lines", "[ai][provider][sse][u3]") {
    ai::providers::SseParser parser;

    auto events = parser.append(": keep-alive\ndata: first\ndata: second\nid: ignored\n\n");

    REQUIRE(events);
    REQUIRE(events->size() == 1);
    CHECK((*events)[0].event == "message");
    CHECK((*events)[0].data == "first\nsecond");
}

TEST_CASE("SSE parser marks OpenAI done sentinel", "[ai][provider][sse][u3]") {
    ai::providers::SseParser parser;

    auto events = parser.append("data: [DONE]\n\n");

    REQUIRE(events);
    REQUIRE(events->size() == 1);
    CHECK((*events)[0].done);
    CHECK((*events)[0].data == "[DONE]");
}

TEST_CASE("SSE parser can dispatch a final unterminated event on finish", "[ai][provider][sse][u3]") {
    ai::providers::SseParser parser;
    auto events = parser.append("data: final");
    REQUIRE(events);
    CHECK(events->empty());

    auto final = parser.finish();
    REQUIRE(final);
    REQUIRE(*final);
    CHECK((*final)->data == "final");
}
