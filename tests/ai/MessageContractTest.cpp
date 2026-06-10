#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../src/agent/Message.hpp"
#include "../../src/ai/Message.hpp"
#include "../../src/ai/StopReason.hpp"
#include "../../src/llm/ChatClient.hpp"

using namespace cch;

TEST_CASE("user text message adapts to AI content blocks and back", "[ai][u2]") {
    agent::Message legacy;
    legacy.role = agent::Role::User;
    legacy.content = "hello model";

    auto message = agent::to_ai_message(legacy);

    CHECK(message.role == ai::MessageRole::User);
    REQUIRE(message.content.size() == 1);
    CHECK(message.content[0].kind == ai::ContentKind::Text);
    CHECK(message.content[0].text == "hello model");

    auto round_trip = agent::message_from_ai(message);
    CHECK(round_trip.role == agent::Role::User);
    CHECK(round_trip.content == "hello model");
}

TEST_CASE("assistant text and tool-call blocks survive legacy adapter round trip", "[ai][u2][ae2]") {
    agent::Message legacy;
    legacy.role = agent::Role::Assistant;
    legacy.content = "I'll read it";
    agent::ToolCall call;
    call.id = "call-1";
    call.name = "read_file";
    call.arguments = {{"path", "README.md"}};
    call.raw_arguments = R"({"path":"README.md"})";
    legacy.tool_calls.push_back(call);

    auto message = agent::to_ai_message(legacy);

    CHECK(message.role == ai::MessageRole::Assistant);
    REQUIRE(message.content.size() == 2);
    CHECK(message.content[0].kind == ai::ContentKind::Text);
    CHECK(message.content[0].text == "I'll read it");
    CHECK(message.content[1].kind == ai::ContentKind::ToolCall);
    CHECK(message.content[1].tool_call.id == "call-1");
    CHECK(message.content[1].tool_call.name == "read_file");
    CHECK(message.content[1].tool_call.raw_arguments == R"({"path":"README.md"})");

    auto round_trip = agent::message_from_ai(message);
    CHECK(round_trip.role == agent::Role::Assistant);
    CHECK(round_trip.content == "I'll read it");
    REQUIRE(round_trip.tool_calls.size() == 1);
    CHECK(round_trip.tool_calls[0].id == "call-1");
    CHECK(round_trip.tool_calls[0].name == "read_file");
    CHECK(std::string(round_trip.tool_calls[0].arguments.at("path").as_string()) == "README.md");
}

TEST_CASE("tool-result message preserves linkage metadata through AI adapter", "[ai][u2]") {
    ai::Message message;
    message.role = ai::MessageRole::ToolResult;
    message.tool_call_id = "call-1";
    message.tool_name = "read_file";
    message.is_error = true;
    message.content.push_back(ai::ContentBlock::from_text("could not read"));

    auto legacy = agent::message_from_ai(message);

    CHECK(legacy.role == agent::Role::Tool);
    CHECK(legacy.tool_call_id == "call-1");
    CHECK(legacy.tool_name == "read_file");
    CHECK(legacy.is_error);
    CHECK(legacy.content == "could not read");

    auto round_trip = agent::to_ai_message(legacy);
    CHECK(round_trip.role == ai::MessageRole::ToolResult);
    CHECK(round_trip.tool_call_id == "call-1");
    CHECK(round_trip.tool_name == "read_file");
    CHECK(round_trip.is_error);
}

TEST_CASE("legacy chat request adapts to AI context without losing model data", "[ai][u2]") {
    llm::ChatRequest legacy;
    legacy.model = "gpt-test";
    legacy.messages.push_back({agent::Role::User, "hello"});
    legacy.tools.push_back({"read_file", "Read a file", {}});

    auto request = llm::to_ai_chat_request(legacy);

    CHECK(request.context.model == "gpt-test");
    REQUIRE(request.context.messages.size() == 1);
    CHECK(request.context.messages[0].role == ai::MessageRole::User);
    CHECK(request.context.messages[0].content[0].text == "hello");
    REQUIRE(request.context.tools.size() == 1);
    CHECK(request.context.tools[0].name == "read_file");

    auto round_trip = llm::chat_request_from_ai(request);
    CHECK(round_trip.model == "gpt-test");
    CHECK(round_trip.messages[0].content == "hello");
    CHECK(round_trip.tools[0].name == "read_file");
}

TEST_CASE("stop reasons map current strings without conflating outcomes", "[ai][u2]") {
    CHECK(ai::stop_reason_from_string("stop") == ai::StopReason::Stop);
    CHECK(ai::stop_reason_from_string("tool_calls") == ai::StopReason::ToolUse);
    CHECK(ai::stop_reason_from_string("tool_use") == ai::StopReason::ToolUse);
    CHECK(ai::stop_reason_from_string("length") == ai::StopReason::Length);
    CHECK(ai::stop_reason_from_string("error") == ai::StopReason::Error);
    CHECK(ai::stop_reason_from_string("aborted") == ai::StopReason::Aborted);
    CHECK(ai::stop_reason_from_string("max_turns_exceeded") == ai::StopReason::Error);
    CHECK(ai::stop_reason_from_string("unrecognized") == ai::StopReason::Unknown);

    CHECK(ai::to_string(ai::StopReason::Stop) == "stop");
    CHECK(ai::to_string(ai::StopReason::ToolUse) == "tool_use");
    CHECK(ai::to_string(ai::StopReason::Length) == "length");
    CHECK(ai::to_string(ai::StopReason::Error) == "error");
    CHECK(ai::to_string(ai::StopReason::Aborted) == "aborted");
    CHECK(ai::to_string(ai::StopReason::Unknown) == "unknown");
}
