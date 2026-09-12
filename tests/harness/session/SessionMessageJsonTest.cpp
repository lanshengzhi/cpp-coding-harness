// The pi message JSON wire shape is session-owned (ADR 0056): these cases pin
// the mapping in `agent/harness/session/SessionMessageJson.hpp` between
// `cch::ai::Message` values and the pi v3 Session Format message DTOs. Moved
// here from `tests/ai/GlazeRoundTripTest.cpp` and
// `tests/ai/MessageContractTest.cpp` when the session module took ownership of
// the wire shape; the session goldens remain its byte-identity guard.

#include "agent/harness/session/SessionMessageJson.hpp"
#include "support/Json.hpp"
#include "support/JsonGlaze.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

using namespace cch;

namespace {

[[nodiscard]] support::Expected<std::string> write_message_json(const ai::MessageVariant& message) {
    return support::write_json(harness::session::detail::to_message_dto(message));
}

[[nodiscard]] support::Expected<ai::MessageVariant> read_message_json(std::string_view json) {
    auto dto = support::read_json<harness::session::detail::MessageDto>(json);
    if (!dto) {
        return std::unexpected(dto.error());
    }
    return harness::session::detail::message_from_dto(*dto, json);
}

} // namespace

TEST_CASE(
        "tool-result message round-trips linkage details and error state", "[harness][session][u2][glaze][compat-pi]") {
    auto details = support::read_json(R"({"exitCode":2,"stderr":"denied"})");
    REQUIRE(details);

    ai::ToolResultMessage result;
    result.tool_call_id = "call-1";
    result.tool_name = "read_file";
    result.content.emplace_back(ai::TextContent{
            .text = "could not read",
            .text_signature = std::nullopt,
    });
    result.content.emplace_back(ai::ImageContent{
            .data = "ZmFrZQ==",
            .mime_type = "image/png",
    });
    result.details = *details;
    result.is_error = true;
    result.timestamp = 1718000000456;

    auto json = write_message_json(ai::MessageVariant{result});
    REQUIRE(json);
    CHECK(json->find(R"("role":"toolResult")") != std::string::npos);
    CHECK(json->find(R"("toolCallId":"call-1")") != std::string::npos);
    CHECK(json->find(R"("isError":true)") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::ToolResultMessage>(*parsed));
    const auto& round_trip = std::get<ai::ToolResultMessage>(*parsed);
    CHECK(round_trip.tool_call_id == "call-1");
    CHECK(round_trip.tool_name == "read_file");
    CHECK(round_trip.is_error);
    CHECK(round_trip.timestamp == 1718000000456);
    REQUIRE(round_trip.content.size() == 2);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    CHECK(std::get<ai::TextContent>(round_trip.content[0]).text == "could not read");
    REQUIRE(std::holds_alternative<ai::ImageContent>(round_trip.content[1]));
    CHECK(std::get<ai::ImageContent>(round_trip.content[1]).mime_type == "image/png");
    REQUIRE(round_trip.details);
    const auto& detail_object = round_trip.details->get<support::JsonValue::object_t>();
    CHECK(static_cast<int>(detail_object.at("exitCode").get<double>()) == 2);
    CHECK(detail_object.at("stderr").get_string() == "denied");
}

TEST_CASE("assistant content round-trips text, thinking, and tool-call variants",
        "[harness][session][u2][glaze][compat-pi]") {
    ai::AssistantMessage msg;
    msg.content.emplace_back(ai::TextContent{
            .text = "hello",
            .text_signature = std::nullopt,
    });
    msg.content.emplace_back(ai::ThinkingContent{
            .thinking = "reasoning",
            .thinking_signature = std::nullopt,
            .redacted = false,
    });
    msg.content.emplace_back(ai::ToolCallContent{
            .id = "call-1",
            .name = "bash",
            .arguments = std::nullopt,
            .raw_arguments = R"({"cmd":"ls"})",
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
    });
    msg.api = "openai-completions";
    msg.provider = "openai";
    msg.model = "gpt-test";
    msg.stop_reason = ai::AssistantStopReason::ToolUse;
    msg.timestamp = 1718000000123;

    auto json = write_message_json(ai::MessageVariant{msg});
    REQUIRE(json);
    CHECK(json->find(R"("type":"text")") != std::string::npos);
    CHECK(json->find(R"("type":"thinking")") != std::string::npos);
    CHECK(json->find(R"("type":"toolCall")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*parsed);
    REQUIRE(round_trip.content.size() == 3);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    REQUIRE(std::holds_alternative<ai::ThinkingContent>(round_trip.content[1]));
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(round_trip.content[2]));
}

TEST_CASE("assistant message round-trips diagnostics and cacheWrite1h", "[harness][session][u2][glaze][compat-pi]") {
    ai::AssistantMessage msg;
    msg.content.emplace_back(ai::TextContent{
            .text = "test",
            .text_signature = std::nullopt,
    });
    msg.api = "openai-completions";
    msg.provider = "openai";
    msg.model = "gpt-test";
    msg.stop_reason = ai::AssistantStopReason::Stop;
    msg.timestamp = 1718000000123;

    // Add diagnostics
    ai::DiagnosticErrorInfo err_info;
    err_info.name = "NetworkError";
    err_info.message = "connection refused";
    err_info.code = "ECONNREFUSED";
    ai::DiagnosticEntry diag;
    diag.type = "provider_error";
    diag.timestamp = 1718000000000;
    diag.error = std::move(err_info);
    msg.diagnostics = std::vector<ai::DiagnosticEntry>{std::move(diag)};

    // Add usage with cacheWrite1h
    ai::Usage usage;
    usage.input = 100;
    usage.output = 50;
    usage.cache_read = 10;
    usage.cache_write = 20;
    usage.cache_write_1h = 15;
    usage.reasoning = 12;
    usage.total_tokens = 150;
    msg.usage = std::move(usage);

    auto json = write_message_json(ai::MessageVariant{msg});
    REQUIRE(json);
    CHECK(json->find(R"("diagnostics")") != std::string::npos);
    CHECK(json->find(R"("cacheWrite1h")") != std::string::npos);
    CHECK(json->find(R"("type":"provider_error")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*parsed);
    REQUIRE(round_trip.diagnostics.has_value());
    REQUIRE(round_trip.diagnostics->size() == 1);
    CHECK((*round_trip.diagnostics)[0].type == "provider_error");
    REQUIRE((*round_trip.diagnostics)[0].error.has_value());
    CHECK((*round_trip.diagnostics)[0].error->name == "NetworkError");
    CHECK(round_trip.usage.cache_write_1h == 15);
    CHECK(round_trip.usage.reasoning == 12);
}

TEST_CASE("ContentDto rejects toolCall for non-assistant context", "[harness][session][u2][glaze][compat-pi]") {
    harness::session::detail::ContentDto dto;
    dto.type = "toolCall";
    dto.id = "call-1";
    dto.name = "read";
    dto.rawArguments = R"({"path":"test"})";

    // detail::content_from_dto rejects toolCall (only valid in assistant content)
    auto result = harness::session::detail::content_from_dto(dto, "test");
    REQUIRE(!result);
    CHECK(result.error().message.find("toolCall") != std::string::npos);

    // detail::assistant_content_from_dto accepts toolCall
    auto assistant_result = harness::session::detail::assistant_content_from_dto(dto, "test");
    REQUIRE(assistant_result);
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(*assistant_result));
}

TEST_CASE("assistant_content_from_dto rejects image content", "[harness][session][u2][glaze][compat-pi]") {
    harness::session::detail::ContentDto dto;
    dto.type = "image";
    dto.data = "ZmFrZQ==";
    dto.mimeType = "image/png";

    auto result = harness::session::detail::assistant_content_from_dto(dto, "test");
    REQUIRE(!result);
    CHECK(result.error().message.find("image") != std::string::npos);
}

TEST_CASE("Glaze rejects invalid UTF-8 in message JSON", "[harness][session][u2][glaze][compat-pi]") {
    auto json = write_message_json(ai::MessageVariant{ai::user_text_message("valid text")});
    REQUIRE(json);
    const auto text_offset = json->find("valid text");
    REQUIRE(text_offset != std::string::npos);
    json->replace(text_offset, std::string_view{"valid text"}.size(), std::string{"\xc0\x80", 2});

    const auto parsed = read_message_json(*json);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
}

TEST_CASE("Glaze rejects out-of-range message integers", "[harness][session][u2][glaze][compat-pi]") {
    auto json = write_message_json(ai::MessageVariant{ai::user_text_message("hello")});
    REQUIRE(json);
    const std::string marker = R"("timestamp":)";
    const auto value_offset = json->find(marker);
    REQUIRE(value_offset != std::string::npos);
    const auto number_offset = value_offset + marker.size();
    const auto number_end = json->find_first_not_of("-0123456789", number_offset);
    REQUIRE(number_end != std::string::npos);
    json->replace(number_offset, number_end - number_offset, "9223372036854775808");

    const auto parsed = read_message_json(*json);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
}

TEST_CASE("UserMessage string and block-array alternatives round-trip with four-way distinction",
        "[harness][session][glaze][issue365][compat-pi]") {
    const auto round_trip = [](ai::UserMessage message) -> ai::UserMessage {
        auto json = write_message_json(ai::MessageVariant{message});
        REQUIRE(json);
        auto parsed = read_message_json(*json);
        REQUIRE(parsed);
        REQUIRE(std::holds_alternative<ai::UserMessage>(*parsed));
        return std::get<ai::UserMessage>(std::move(*parsed));
    };

    const auto string_message = round_trip(ai::UserMessage{
            .content = std::string{"hello model"},
            .timestamp = 1718000000000,
    });
    REQUIRE(std::holds_alternative<std::string>(string_message.content));
    CHECK(std::get<std::string>(string_message.content) == "hello model");

    const auto block_message = round_trip(ai::UserMessage{
            .content = std::vector<ai::Content>{ai::text_content("hello model")},
            .timestamp = 1718000000001,
    });
    REQUIRE(std::holds_alternative<std::vector<ai::Content>>(block_message.content));
    const auto& blocks = std::get<std::vector<ai::Content>>(block_message.content);
    REQUIRE(blocks.size() == 1);
    CHECK(std::get<ai::TextContent>(blocks[0]).text == "hello model");

    const auto empty_string = round_trip(ai::UserMessage{
            .content = std::string{},
            .timestamp = 1718000000002,
    });
    REQUIRE(std::holds_alternative<std::string>(empty_string.content));
    CHECK(std::get<std::string>(empty_string.content).empty());

    const auto empty_array = round_trip(ai::UserMessage{
            .content = std::vector<ai::Content>{},
            .timestamp = 1718000000003,
    });
    REQUIRE(std::holds_alternative<std::vector<ai::Content>>(empty_array.content));
    CHECK(std::get<std::vector<ai::Content>>(empty_array.content).empty());

    // The JSON surface writes the string alternative as a JSON string and the
    // vector alternative as a JSON array; empty string vs empty array stay
    // distinct on the wire.
    auto string_json = write_message_json(
            ai::MessageVariant{ai::UserMessage{.content = std::string{"hello model"}, .timestamp = 1718000000004}});
    REQUIRE(string_json);
    CHECK(string_json->find(R"("content":"hello model")") != std::string::npos);

    auto array_json = write_message_json(ai::MessageVariant{ai::UserMessage{
            .content = std::vector<ai::Content>{ai::text_content("hello model")}, .timestamp = 1718000000005}});
    REQUIRE(array_json);
    CHECK(array_json->find(R"("content":[{"type":"text")") != std::string::npos);

    auto empty_string_json = write_message_json(
            ai::MessageVariant{ai::UserMessage{.content = std::string{}, .timestamp = 1718000000006}});
    REQUIRE(empty_string_json);
    CHECK(empty_string_json->find(R"("content":"")") != std::string::npos);

    auto empty_array_json = write_message_json(
            ai::MessageVariant{ai::UserMessage{.content = std::vector<ai::Content>{}, .timestamp = 1718000000007}});
    REQUIRE(empty_array_json);
    CHECK(empty_array_json->find(R"("content":[])") != std::string::npos);
}

TEST_CASE("session-style user message JSON with string content loads the string alternative",
        "[harness][session][glaze][issue365][compat-pi]") {
    const auto parsed =
            read_message_json(R"({"role":"user","content":"resumed string message","timestamp":1718000000000})");
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::UserMessage>(*parsed));
    const auto& user = std::get<ai::UserMessage>(*parsed);
    REQUIRE(std::holds_alternative<std::string>(user.content));
    CHECK(std::get<std::string>(user.content) == "resumed string message");
}

TEST_CASE("user text message serializes through explicit Glaze content tags",
        "[harness][session][u2][glaze][compat-pi]") {
    ai::MessageVariant original = ai::UserMessage{
            .content = std::vector<ai::Content>{ai::TextContent{
                    .text = "hello model",
                    .text_signature = "sig-1",
            }},
            .timestamp = 1718000000000,
    };

    auto json = write_message_json(original);
    REQUIRE(json);
    CHECK(json->find(R"("role":"user")") != std::string::npos);
    CHECK(json->find(R"("type":"text")") != std::string::npos);
    CHECK(json->find(R"("timestamp":1718000000000)") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::UserMessage>(*parsed));

    const auto& user = std::get<ai::UserMessage>(*parsed);
    CHECK(user.timestamp == 1718000000000);
    REQUIRE(std::get<std::vector<ai::Content>>(user.content).size() == 1);
    REQUIRE(std::holds_alternative<ai::TextContent>(std::get<std::vector<ai::Content>>(user.content)[0]));
    CHECK(std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(user.content)[0]).text == "hello model");
    REQUIRE(std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(user.content)[0]).text_signature);
    CHECK(*std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(user.content)[0]).text_signature == "sig-1");
}

TEST_CASE("assistant text and tool-call content round-trip in order with metadata",
        "[harness][session][u2][ae2][compat-pi]") {
    auto arguments = support::read_json(R"({"path":"README.md","limit":20})");
    REQUIRE(arguments);

    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{
            .text = "I'll read it",
            .text_signature = std::nullopt,
    });
    assistant.content.emplace_back(ai::ToolCallContent{
            .id = "call-1",
            .name = "read_file",
            .arguments = *arguments,
            .raw_arguments = R"({"path":"README.md","limit":20})",
            .thought_signature = std::nullopt,
            .arguments_valid = true,
            .argument_error = std::nullopt,
    });
    assistant.api = "openai-completions";
    assistant.provider = "openai";
    assistant.model = "gpt-test";
    assistant.response_model = "gpt-test-2026-06";
    assistant.response_id = "resp-1";
    assistant.usage = ai::Usage{
            .input = 10,
            .output = 5,
            .cache_read = 2,
            .cache_write = 1,
            .cache_write_1h = std::nullopt,
            .reasoning = 3,
            .total_tokens = 18,
            .cost =
                    ai::UsageCost{
                            .input = 0.1,
                            .output = 0.2,
                            .cache_read = 0.03,
                            .cache_write = 0.04,
                            .total = 0.37,
                    },
    };
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1718000000123;

    auto json = write_message_json(ai::MessageVariant{assistant});
    REQUIRE(json);
    CHECK(json->find(R"("role":"assistant")") != std::string::npos);
    CHECK(json->find(R"("type":"toolCall")") != std::string::npos);
    CHECK(json->find(R"("stopReason":"toolUse")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*parsed);

    REQUIRE(round_trip.content.size() == 2);
    REQUIRE(std::holds_alternative<ai::TextContent>(round_trip.content[0]));
    CHECK(std::get<ai::TextContent>(round_trip.content[0]).text == "I'll read it");
    REQUIRE(std::holds_alternative<ai::ToolCallContent>(round_trip.content[1]));
    const auto& call = std::get<ai::ToolCallContent>(round_trip.content[1]);
    CHECK(call.id == "call-1");
    CHECK(call.name == "read_file");
    CHECK(call.raw_arguments == R"({"path":"README.md","limit":20})");
    REQUIRE(call.arguments);
    const auto& object = call.arguments->get<support::JsonValue::object_t>();
    CHECK(object.at("path").get_string() == "README.md");
    CHECK(static_cast<int>(object.at("limit").get<double>()) == 20);

    CHECK(round_trip.api == "openai-completions");
    CHECK(round_trip.provider == "openai");
    CHECK(round_trip.model == "gpt-test");
    REQUIRE(round_trip.response_model);
    CHECK(*round_trip.response_model == "gpt-test-2026-06");
    REQUIRE(round_trip.response_id);
    CHECK(*round_trip.response_id == "resp-1");
    CHECK(round_trip.usage.input == 10);
    CHECK(round_trip.usage.output == 5);
    CHECK(round_trip.usage.cache_read == 2);
    CHECK(round_trip.usage.cache_write == 1);
    CHECK(round_trip.usage.reasoning == 3);
    CHECK(round_trip.usage.total_tokens == 18);
    CHECK(round_trip.usage.cost.total == 0.37);
    CHECK(round_trip.stop_reason == ai::AssistantStopReason::ToolUse);
    CHECK(round_trip.timestamp == 1718000000123);
}

TEST_CASE("assistant pending stop reason and raw stop reason round-trip",
        "[harness][session][glaze][issue374][compat-pi]") {
    const auto parsed = read_message_json(
            R"({"role":"assistant","content":[{"type":"text","text":"in flight"}],"api":"anthropic-messages","provider":"kimi-coding","model":"kimi-for-coding","usage":{"input":1,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":1,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"stopReason":"pending","rawStopReason":"pause_turn","timestamp":1718000000000})");
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& assistant = std::get<ai::AssistantMessage>(*parsed);
    CHECK(assistant.stop_reason == ai::AssistantStopReason::Pending);
    REQUIRE(assistant.raw_stop_reason);
    CHECK(*assistant.raw_stop_reason == "pause_turn");

    auto json = write_message_json(*parsed);
    REQUIRE(json);
    CHECK(json->find(R"("stopReason":"pending")") != std::string::npos);
    CHECK(json->find(R"("rawStopReason":"pause_turn")") != std::string::npos);

    auto re_parsed = read_message_json(*json);
    REQUIRE(re_parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*re_parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*re_parsed);
    CHECK(round_trip.stop_reason == ai::AssistantStopReason::Pending);
    REQUIRE(round_trip.raw_stop_reason);
    CHECK(*round_trip.raw_stop_reason == "pause_turn");
}

TEST_CASE("assistant message without rawStopReason round-trips as absence",
        "[harness][session][glaze][issue374][compat-pi]") {
    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::TextContent{
            .text = "answer",
            .text_signature = std::nullopt,
    });
    assistant.api = "openai-responses";
    assistant.provider = "deepseek";
    assistant.model = "deepseek-v4-flash";
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.timestamp = 1718000000000;

    auto json = write_message_json(ai::MessageVariant{assistant});
    REQUIRE(json);
    CHECK(json->find("rawStopReason") == std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    CHECK(std::get<ai::AssistantMessage>(*parsed).raw_stop_reason == std::nullopt);
}

TEST_CASE("assistant JSON requires a supported stop reason", "[harness][session][u2][glaze][issue18][compat-pi]") {
    const auto missing = read_message_json(
            R"({"role":"assistant","content":[{"type":"text","text":"answer"}],"api":"openai-completions","provider":"openai","model":"gpt-test","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"timestamp":1718000000000})");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == support::ErrorCode::JsonParse);
    CHECK(missing.error().detail.find("stopReason") != std::string::npos);

    const auto unsupported = read_message_json(
            R"({"role":"assistant","content":[{"type":"text","text":"answer"}],"api":"openai-completions","provider":"openai","model":"gpt-test","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"stopReason":"future_reason","timestamp":1718000000000})");
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == support::ErrorCode::JsonParse);
    CHECK(unsupported.error().detail.find("future_reason") != std::string::npos);
}

TEST_CASE("unknown content discriminator returns a typed JSON error", "[harness][session][u2][glaze][compat-pi]") {
    auto parsed = read_message_json(
            R"({"role":"user","content":[{"type":"audio","data":"AAAA"}],"timestamp":1718000000000})");

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
    CHECK(parsed.error().message == "unknown content discriminator");
    CHECK(parsed.error().detail.find("audio") != std::string::npos);
}

TEST_CASE("missing required content payload fields return typed JSON errors",
        "[harness][session][u2][glaze][compat-pi]") {
    auto missing_text = read_message_json(R"({"role":"user","content":[{"type":"text"}],"timestamp":1718000000000})");
    REQUIRE_FALSE(missing_text);
    CHECK(missing_text.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_text.error().detail.find("text") != std::string::npos);

    auto missing_tool_id = read_message_json(
            R"({"role":"assistant","content":[{"type":"toolCall","name":"read_file","rawArguments":"{}"}],"api":"openai-completions","provider":"openai","model":"gpt-test","timestamp":1718000000000})");
    REQUIRE_FALSE(missing_tool_id);
    CHECK(missing_tool_id.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_tool_id.error().detail.find("id") != std::string::npos);

    auto missing_tool_result_link = read_message_json(
            R"({"role":"toolResult","content":[{"type":"text","text":"ok"}],"timestamp":1718000000000})");
    REQUIRE_FALSE(missing_tool_result_link);
    CHECK(missing_tool_result_link.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_tool_result_link.error().detail.find("toolCallId") != std::string::npos);
}

// ── Extended message type round-trip tests ──

TEST_CASE("BashExecutionMessage serializes and deserializes round-trip",
        "[harness][session][extended][glaze][compat-pi]") {
    ai::BashExecutionMessage bash;
    bash.command = "echo hello";
    bash.output = "hello\n";
    bash.exit_code = 0;
    bash.cancelled = false;
    bash.truncated = false;
    bash.full_output_path = std::nullopt;
    bash.exclude_from_context = false;
    bash.timestamp = 1718000000001;

    auto json = write_message_json(ai::MessageVariant{bash});
    REQUIRE(json);
    CHECK(json->find(R"("role":"bashExecution")") != std::string::npos);
    CHECK(json->find(R"("command":"echo hello")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::BashExecutionMessage>(*parsed));
    const auto& rt = std::get<ai::BashExecutionMessage>(*parsed);
    CHECK(rt.command == "echo hello");
    CHECK(rt.output == "hello\n");
    REQUIRE(rt.exit_code);
    CHECK(*rt.exit_code == 0);
    CHECK(rt.cancelled == false);
    CHECK(rt.timestamp == 1718000000001);
}

TEST_CASE("BashExecutionMessage with optional fields null round-trips",
        "[harness][session][extended][glaze][compat-pi]") {
    ai::BashExecutionMessage bash;
    bash.command = "ls";
    bash.output = "";
    bash.timestamp = 1718000000002;

    auto json = write_message_json(ai::MessageVariant{bash});
    REQUIRE(json);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::BashExecutionMessage>(*parsed));
    const auto& rt = std::get<ai::BashExecutionMessage>(*parsed);
    CHECK_FALSE(rt.exit_code.has_value());
    CHECK_FALSE(rt.full_output_path.has_value());
    CHECK(rt.cancelled == false);
}

TEST_CASE("CompactionSummaryMessage serializes and deserializes round-trip",
        "[harness][session][extended][glaze][compat-pi]") {
    ai::CompactionSummaryMessage compaction;
    compaction.summary = "Compacted 10 messages";
    compaction.tokens_before = 5000;
    compaction.timestamp = 1718000000003;

    auto json = write_message_json(ai::MessageVariant{compaction});
    REQUIRE(json);
    CHECK(json->find(R"("role":"compactionSummary")") != std::string::npos);
    CHECK(json->find(R"("tokensBefore":5000)") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(*parsed));
    const auto& rt = std::get<ai::CompactionSummaryMessage>(*parsed);
    CHECK(rt.summary == "Compacted 10 messages");
    CHECK(rt.tokens_before == 5000);
    CHECK(rt.timestamp == 1718000000003);
}

TEST_CASE("BranchSummaryMessage serializes and deserializes round-trip",
        "[harness][session][extended][glaze][compat-pi]") {
    ai::BranchSummaryMessage branch;
    branch.summary = "Branch resolved";
    branch.from_id = "abc12345";
    branch.timestamp = 1718000000004;

    auto json = write_message_json(ai::MessageVariant{branch});
    REQUIRE(json);
    CHECK(json->find(R"("role":"branchSummary")") != std::string::npos);
    CHECK(json->find(R"("fromId":"abc12345")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::BranchSummaryMessage>(*parsed));
    const auto& rt = std::get<ai::BranchSummaryMessage>(*parsed);
    CHECK(rt.summary == "Branch resolved");
    CHECK(rt.from_id == "abc12345");
    CHECK(rt.timestamp == 1718000000004);
}

TEST_CASE("CustomMessage serializes and deserializes round-trip", "[harness][session][extended][glaze][compat-pi]") {
    ai::CustomMessage custom;
    custom.custom_type = "my-extension";
    custom.content.emplace_back(ai::TextContent{
            .text = "hello from extension",
            .text_signature = std::nullopt,
    });
    custom.display = true;
    custom.timestamp = 1718000000005;

    auto json = write_message_json(ai::MessageVariant{custom});
    REQUIRE(json);
    CHECK(json->find(R"("role":"custom")") != std::string::npos);
    CHECK(json->find(R"("customType":"my-extension")") != std::string::npos);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(*parsed));
    const auto& rt = std::get<ai::CustomMessage>(*parsed);
    CHECK(rt.custom_type == "my-extension");
    CHECK(rt.display == true);
    REQUIRE(rt.content.size() == 1);
    CHECK(std::get<ai::TextContent>(rt.content[0]).text == "hello from extension");
    CHECK(rt.timestamp == 1718000000005);
}

TEST_CASE("CustomMessage with display false round-trips", "[harness][session][extended][glaze][compat-pi]") {
    ai::CustomMessage custom;
    custom.custom_type = "hidden-ext";
    custom.display = false;
    custom.timestamp = 1718000000006;

    auto json = write_message_json(ai::MessageVariant{custom});
    REQUIRE(json);

    auto parsed = read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(*parsed));
    CHECK(std::get<ai::CustomMessage>(*parsed).display == false);
}
