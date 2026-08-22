#include "ai/glaze/AiJson.hpp"
#include "support/Json.hpp"

#include <cch/ai/StreamEvent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <variant>

using namespace cch;

TEST_CASE("AI contracts are aggregate-friendly passive value types", "[ai][u3][contract]") {
    static_assert(std::is_aggregate_v<ai::TextContent>);
    static_assert(std::is_aggregate_v<ai::ToolCallContent>);
    static_assert(std::is_aggregate_v<ai::UsageCost>);
    static_assert(std::is_aggregate_v<ai::Usage>);
    static_assert(std::is_aggregate_v<ai::AssistantMessage>);
    static_assert(std::is_same_v<decltype(ai::AssistantMessage::usage), ai::Usage>);
    static_assert(std::is_same_v<decltype(ai::Usage::reasoning), std::optional<std::int64_t>>);
    static_assert(std::is_aggregate_v<ai::ToolResultMessage>);
    static_assert(std::is_aggregate_v<ai::BashExecutionMessage>);
    static_assert(std::is_aggregate_v<ai::CustomMessage>);
    static_assert(std::is_aggregate_v<ai::BranchSummaryMessage>);
    static_assert(std::is_aggregate_v<ai::CompactionSummaryMessage>);
    static_assert(std::is_aggregate_v<support::Error>);

    ai::ToolCallContent call{
        .id = "call-1",
        .name = "read_file",
        .arguments = std::nullopt,
        .raw_arguments = R"({"path":"README.md"})",
        .thought_signature = std::nullopt,
        .arguments_valid = true,
        .argument_error = std::nullopt,
    };
    ai::AssistantMessage assistant{
        .content = {ai::TextContent{.text = "checking", .text_signature = std::nullopt}, call},
        .api = "openai-completions",
        .provider = "openai-compatible",
        .model = "gpt-test",
        .response_model = std::nullopt,
        .response_id = std::nullopt,
        .usage = {},
        .stop_reason = ai::AssistantStopReason::ToolUse,
        .error_message = std::nullopt,
        .diagnostics = std::nullopt,
        .timestamp = 1718000000999,
    };
    support::Error error{
        .code = support::ErrorCode::Validation,
        .message = "invalid contract",
        .detail = "field is required",
        .context = std::string{"message"},
    };

    REQUIRE(assistant.content.size() == 2);
    CHECK(std::holds_alternative<ai::ToolCallContent>(assistant.content[1]));
    CHECK(std::get<ai::ToolCallContent>(assistant.content[1]).name == "read_file");
    CHECK(error.code == support::ErrorCode::Validation);
    REQUIRE(error.context);
    CHECK(*error.context == "message");
}

TEST_CASE(
    "UserMessage content is a passive sum type with string and block-array alternatives",
    "[ai][u3][contract][issue365]") {
    static_assert(std::is_aggregate_v<ai::UserMessage>);
    static_assert(std::is_same_v<
                  decltype(ai::UserMessage::content),
                  std::variant<std::string, std::vector<ai::Content>>>);

    ai::UserMessage string_message{
        .content = std::string{"plain text"},
        .timestamp = 1718000000000,
    };
    REQUIRE(std::holds_alternative<std::string>(string_message.content));
    CHECK(std::get<std::string>(string_message.content) == "plain text");

    ai::UserMessage block_message{
        .content = std::vector<ai::Content>{ai::text_content("block text")},
        .timestamp = 1718000000001,
    };
    REQUIRE(std::holds_alternative<std::vector<ai::Content>>(block_message.content));
    const auto& blocks = std::get<std::vector<ai::Content>>(block_message.content);
    REQUIRE(blocks.size() == 1);
    CHECK(std::get<ai::TextContent>(blocks[0]).text == "block text");
}

TEST_CASE(
    "UserMessage string and block-array alternatives round-trip with four-way distinction",
    "[ai][glaze][issue365]") {
    const auto round_trip = [](ai::UserMessage message) -> ai::UserMessage {
        auto json = ai::glaze::write_message_json(ai::MessageVariant{message});
        REQUIRE(json);
        auto parsed = ai::glaze::read_message_json(*json);
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
    auto string_json = ai::glaze::write_message_json(ai::MessageVariant{
        ai::UserMessage{.content = std::string{"hello model"}, .timestamp = 1718000000004}});
    REQUIRE(string_json);
    CHECK(string_json->find(R"("content":"hello model")") != std::string::npos);

    auto array_json = ai::glaze::write_message_json(ai::MessageVariant{
        ai::UserMessage{
            .content = std::vector<ai::Content>{ai::text_content("hello model")},
            .timestamp = 1718000000005}});
    REQUIRE(array_json);
    CHECK(array_json->find(R"("content":[{"type":"text")") != std::string::npos);

    auto empty_string_json = ai::glaze::write_message_json(ai::MessageVariant{
        ai::UserMessage{.content = std::string{}, .timestamp = 1718000000006}});
    REQUIRE(empty_string_json);
    CHECK(empty_string_json->find(R"("content":"")") != std::string::npos);

    auto empty_array_json = ai::glaze::write_message_json(ai::MessageVariant{
        ai::UserMessage{.content = std::vector<ai::Content>{}, .timestamp = 1718000000007}});
    REQUIRE(empty_array_json);
    CHECK(empty_array_json->find(R"("content":[])") != std::string::npos);
}

TEST_CASE(
    "session-style user message JSON with string content loads the string alternative",
    "[ai][glaze][issue365]") {
    const auto parsed = ai::glaze::read_message_json(
        R"({"role":"user","content":"resumed string message","timestamp":1718000000000})");
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::UserMessage>(*parsed));
    const auto& user = std::get<ai::UserMessage>(*parsed);
    REQUIRE(std::holds_alternative<std::string>(user.content));
    CHECK(std::get<std::string>(user.content) == "resumed string message");
}

TEST_CASE("user text message serializes through explicit Glaze content tags", "[ai][u2][glaze]") {
    ai::MessageVariant original = ai::UserMessage{
        .content = std::vector<ai::Content>{ai::TextContent{
            .text = "hello model",
            .text_signature = "sig-1",
        }},
        .timestamp = 1718000000000,
    };

    auto json = ai::glaze::write_message_json(original);
    REQUIRE(json);
    CHECK(json->find(R"("role":"user")") != std::string::npos);
    CHECK(json->find(R"("type":"text")") != std::string::npos);
    CHECK(json->find(R"("timestamp":1718000000000)") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
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

TEST_CASE("assistant text and tool-call content round-trip in order with metadata", "[ai][u2][ae2]") {
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
        .cost = ai::UsageCost{
            .input = 0.1,
            .output = 0.2,
            .cache_read = 0.03,
            .cache_write = 0.04,
            .total = 0.37,
        },
    };
    assistant.stop_reason = ai::AssistantStopReason::ToolUse;
    assistant.timestamp = 1718000000123;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{assistant});
    REQUIRE(json);
    CHECK(json->find(R"("role":"assistant")") != std::string::npos);
    CHECK(json->find(R"("type":"toolCall")") != std::string::npos);
    CHECK(json->find(R"("stopReason":"toolUse")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
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

TEST_CASE("assistant pending stop reason and raw stop reason round-trip", "[ai][glaze][issue374]") {
    const auto parsed = ai::glaze::read_message_json(
        R"({"role":"assistant","content":[{"type":"text","text":"in flight"}],"api":"anthropic-messages","provider":"kimi-coding","model":"kimi-for-coding","usage":{"input":1,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":1,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"stopReason":"pending","rawStopReason":"pause_turn","timestamp":1718000000000})");
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    const auto& assistant = std::get<ai::AssistantMessage>(*parsed);
    CHECK(assistant.stop_reason == ai::AssistantStopReason::Pending);
    REQUIRE(assistant.raw_stop_reason);
    CHECK(*assistant.raw_stop_reason == "pause_turn");

    auto json = ai::glaze::write_message_json(*parsed);
    REQUIRE(json);
    CHECK(json->find(R"("stopReason":"pending")") != std::string::npos);
    CHECK(json->find(R"("rawStopReason":"pause_turn")") != std::string::npos);

    auto re_parsed = ai::glaze::read_message_json(*json);
    REQUIRE(re_parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*re_parsed));
    const auto& round_trip = std::get<ai::AssistantMessage>(*re_parsed);
    CHECK(round_trip.stop_reason == ai::AssistantStopReason::Pending);
    REQUIRE(round_trip.raw_stop_reason);
    CHECK(*round_trip.raw_stop_reason == "pause_turn");
}

TEST_CASE("assistant message without rawStopReason round-trips as absence", "[ai][glaze][issue374]") {
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

    auto json = ai::glaze::write_message_json(ai::MessageVariant{assistant});
    REQUIRE(json);
    CHECK(json->find("rawStopReason") == std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::AssistantMessage>(*parsed));
    CHECK(std::get<ai::AssistantMessage>(*parsed).raw_stop_reason == std::nullopt);
}

TEST_CASE("assistant JSON requires a supported stop reason", "[ai][u2][glaze][issue18]") {
    const auto missing = ai::glaze::read_message_json(
        R"({"role":"assistant","content":[{"type":"text","text":"answer"}],"api":"openai-completions","provider":"openai","model":"gpt-test","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"timestamp":1718000000000})");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == support::ErrorCode::JsonParse);
    CHECK(missing.error().detail.find("stopReason") != std::string::npos);

    const auto unsupported = ai::glaze::read_message_json(
        R"({"role":"assistant","content":[{"type":"text","text":"answer"}],"api":"openai-completions","provider":"openai","model":"gpt-test","usage":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"totalTokens":0,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0,"total":0}},"stopReason":"future_reason","timestamp":1718000000000})");
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error().code == support::ErrorCode::JsonParse);
    CHECK(unsupported.error().detail.find("future_reason") != std::string::npos);
}

TEST_CASE("unknown content discriminator returns a typed JSON error", "[ai][u2][glaze]") {
    auto parsed = ai::glaze::read_message_json(
        R"({"role":"user","content":[{"type":"audio","data":"AAAA"}],"timestamp":1718000000000})");

    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().code == support::ErrorCode::JsonParse);
    CHECK(parsed.error().message == "unknown content discriminator");
    CHECK(parsed.error().detail.find("audio") != std::string::npos);
}

TEST_CASE("missing required content payload fields return typed JSON errors", "[ai][u2][glaze]") {
    auto missing_text = ai::glaze::read_message_json(
        R"({"role":"user","content":[{"type":"text"}],"timestamp":1718000000000})");
    REQUIRE_FALSE(missing_text);
    CHECK(missing_text.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_text.error().detail.find("text") != std::string::npos);

    auto missing_tool_id = ai::glaze::read_message_json(
        R"({"role":"assistant","content":[{"type":"toolCall","name":"read_file","rawArguments":"{}"}],"api":"openai-completions","provider":"openai","model":"gpt-test","timestamp":1718000000000})");
    REQUIRE_FALSE(missing_tool_id);
    CHECK(missing_tool_id.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_tool_id.error().detail.find("id") != std::string::npos);

    auto missing_tool_result_link = ai::glaze::read_message_json(
        R"({"role":"toolResult","content":[{"type":"text","text":"ok"}],"timestamp":1718000000000})");
    REQUIRE_FALSE(missing_tool_result_link);
    CHECK(missing_tool_result_link.error().code == support::ErrorCode::JsonParse);
    CHECK(missing_tool_result_link.error().detail.find("toolCallId") != std::string::npos);
}

// ── Extended message type round-trip tests ──

TEST_CASE("BashExecutionMessage serializes and deserializes round-trip", "[ai][extended][glaze]") {
    ai::BashExecutionMessage bash;
    bash.command = "echo hello";
    bash.output = "hello\n";
    bash.exit_code = 0;
    bash.cancelled = false;
    bash.truncated = false;
    bash.full_output_path = std::nullopt;
    bash.exclude_from_context = false;
    bash.timestamp = 1718000000001;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{bash});
    REQUIRE(json);
    CHECK(json->find(R"("role":"bashExecution")") != std::string::npos);
    CHECK(json->find(R"("command":"echo hello")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
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

TEST_CASE("BashExecutionMessage with optional fields null round-trips", "[ai][extended][glaze]") {
    ai::BashExecutionMessage bash;
    bash.command = "ls";
    bash.output = "";
    bash.timestamp = 1718000000002;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{bash});
    REQUIRE(json);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::BashExecutionMessage>(*parsed));
    const auto& rt = std::get<ai::BashExecutionMessage>(*parsed);
    CHECK_FALSE(rt.exit_code.has_value());
    CHECK_FALSE(rt.full_output_path.has_value());
    CHECK(rt.cancelled == false);
}

TEST_CASE("CompactionSummaryMessage serializes and deserializes round-trip", "[ai][extended][glaze]") {
    ai::CompactionSummaryMessage compaction;
    compaction.summary = "Compacted 10 messages";
    compaction.tokens_before = 5000;
    compaction.timestamp = 1718000000003;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{compaction});
    REQUIRE(json);
    CHECK(json->find(R"("role":"compactionSummary")") != std::string::npos);
    CHECK(json->find(R"("tokensBefore":5000)") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CompactionSummaryMessage>(*parsed));
    const auto& rt = std::get<ai::CompactionSummaryMessage>(*parsed);
    CHECK(rt.summary == "Compacted 10 messages");
    CHECK(rt.tokens_before == 5000);
    CHECK(rt.timestamp == 1718000000003);
}

TEST_CASE("BranchSummaryMessage serializes and deserializes round-trip", "[ai][extended][glaze]") {
    ai::BranchSummaryMessage branch;
    branch.summary = "Branch resolved";
    branch.from_id = "abc12345";
    branch.timestamp = 1718000000004;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{branch});
    REQUIRE(json);
    CHECK(json->find(R"("role":"branchSummary")") != std::string::npos);
    CHECK(json->find(R"("fromId":"abc12345")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::BranchSummaryMessage>(*parsed));
    const auto& rt = std::get<ai::BranchSummaryMessage>(*parsed);
    CHECK(rt.summary == "Branch resolved");
    CHECK(rt.from_id == "abc12345");
    CHECK(rt.timestamp == 1718000000004);
}

TEST_CASE("CustomMessage serializes and deserializes round-trip", "[ai][extended][glaze]") {
    ai::CustomMessage custom;
    custom.custom_type = "my-extension";
    custom.content.emplace_back(ai::TextContent{
        .text = "hello from extension",
        .text_signature = std::nullopt,
    });
    custom.display = true;
    custom.timestamp = 1718000000005;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{custom});
    REQUIRE(json);
    CHECK(json->find(R"("role":"custom")") != std::string::npos);
    CHECK(json->find(R"("customType":"my-extension")") != std::string::npos);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(*parsed));
    const auto& rt = std::get<ai::CustomMessage>(*parsed);
    CHECK(rt.custom_type == "my-extension");
    CHECK(rt.display == true);
    REQUIRE(rt.content.size() == 1);
    CHECK(std::get<ai::TextContent>(rt.content[0]).text == "hello from extension");
    CHECK(rt.timestamp == 1718000000005);
}

TEST_CASE("CustomMessage with display false round-trips", "[ai][extended][glaze]") {
    ai::CustomMessage custom;
    custom.custom_type = "hidden-ext";
    custom.display = false;
    custom.timestamp = 1718000000006;

    auto json = ai::glaze::write_message_json(ai::MessageVariant{custom});
    REQUIRE(json);

    auto parsed = ai::glaze::read_message_json(*json);
    REQUIRE(parsed);
    REQUIRE(std::holds_alternative<ai::CustomMessage>(*parsed));
    CHECK(std::get<ai::CustomMessage>(*parsed).display == false);
}

// ── LLM conversion tests ──

TEST_CASE("bash_execution_to_user_message produces formatted text", "[ai][extended][convert]") {
    ai::BashExecutionMessage bash;
    bash.command = "echo hello";
    bash.output = "hello\n";
    bash.exit_code = 0;
    bash.timestamp = 1718000000001;

    auto msg = ai::bash_execution_to_user_message(bash);
    CHECK(msg.timestamp == 1718000000001);
    REQUIRE(std::get<std::vector<ai::Content>>(msg.content).size() == 1);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("Ran `echo hello`") != std::string::npos);
    CHECK(text.text.find("```\nhello") != std::string::npos);
}

TEST_CASE("bash_execution_to_user_message reports non-zero exit code", "[ai][extended][convert]") {
    ai::BashExecutionMessage bash;
    bash.command = "false";
    bash.output = "";
    bash.exit_code = 1;
    bash.timestamp = 1718000000002;

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("Command exited with code 1") != std::string::npos);
}

TEST_CASE("bash_execution_to_user_message reports cancellation", "[ai][extended][convert]") {
    ai::BashExecutionMessage bash;
    bash.command = "sleep 999";
    bash.output = "";
    bash.cancelled = true;

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("(command cancelled)") != std::string::npos);
}

TEST_CASE("bash_execution_to_user_message reports truncation with path", "[ai][extended][convert]") {
    ai::BashExecutionMessage bash;
    bash.command = "cat huge.log";
    bash.output = "truncated...";
    bash.truncated = true;
    bash.full_output_path = "/tmp/bash-output-12345.txt";

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("[Output truncated. Full output: /tmp/bash-output-12345.txt]") != std::string::npos);
}

TEST_CASE("compaction_summary_to_user_message wraps with prefix and suffix", "[ai][extended][convert]") {
    ai::CompactionSummaryMessage compaction;
    compaction.summary = "Previous 20 messages compacted";
    compaction.tokens_before = 8000;
    compaction.timestamp = 1718000000003;

    auto msg = ai::compaction_summary_to_user_message(compaction);
    CHECK(msg.timestamp == 1718000000003);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find(std::string{ai::kCompactionSummaryPrefix} + "Previous 20 messages compacted" +
                         std::string{ai::kCompactionSummarySuffix}) != std::string::npos);
}

TEST_CASE("branch_summary_to_user_message wraps with prefix and suffix", "[ai][extended][convert]") {
    ai::BranchSummaryMessage branch;
    branch.summary = "Branch work completed";
    branch.from_id = "abc12345";
    branch.timestamp = 1718000000004;

    auto msg = ai::branch_summary_to_user_message(branch);
    CHECK(msg.timestamp == 1718000000004);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find(std::string{ai::kBranchSummaryPrefix} + "Branch work completed" +
                         std::string{ai::kBranchSummarySuffix}) != std::string::npos);
}

TEST_CASE(
    "custom_message_to_user_message preserves ordered text and image blocks",
    "[ai][extended][convert][issue22]") {
    ai::CustomMessage custom;
    custom.custom_type = "ext";
    custom.content.emplace_back(ai::TextContent{
        .text = "part1",
        .text_signature = std::nullopt,
    });
    custom.content.emplace_back(ai::ImageContent{
        .data = "aW1hZ2U=",
        .mime_type = "image/png",
    });
    custom.content.emplace_back(ai::TextContent{
        .text = "part2",
        .text_signature = std::nullopt,
    });
    custom.timestamp = 1718000000005;

    auto msg = ai::custom_message_to_user_message(custom);

    CHECK(msg.timestamp == 1718000000005);
    REQUIRE(std::get<std::vector<ai::Content>>(msg.content).size() == 3);
    CHECK(std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]).text == "part1");
    CHECK(std::get<ai::ImageContent>(std::get<std::vector<ai::Content>>(msg.content)[1]).data == "aW1hZ2U=");
    CHECK(std::get<ai::ImageContent>(std::get<std::vector<ai::Content>>(msg.content)[1]).mime_type == "image/png");
    CHECK(std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[2]).text == "part2");
}

TEST_CASE("custom_message_to_user_message preserves empty content", "[ai][extended][convert][issue22]") {
    ai::CustomMessage custom;
    custom.custom_type = "ext";
    custom.timestamp = 1718000000006;

    auto msg = ai::custom_message_to_user_message(custom);

    CHECK(std::get<std::vector<ai::Content>>(msg.content).empty());
    CHECK(msg.timestamp == 1718000000006);
}

TEST_CASE("default-constructed AI contracts are empty passive values", "[ai][contract][issue372]") {
    ai::Tool tool;
    CHECK(tool.name.empty());
    CHECK(tool.description.empty());
    CHECK(tool.parameters.holds<support::JsonValue::null_t>());

    ai::TextContent text;
    CHECK(text.text.empty());
    CHECK_FALSE(text.text_signature.has_value());

    ai::ToolCallContent call;
    CHECK(call.id.empty());
    CHECK_FALSE(call.arguments.has_value());
    CHECK(call.arguments_valid);

    ai::UserMessage user;
    CHECK(std::holds_alternative<std::string>(user.content));
    CHECK(std::get<std::string>(user.content).empty());

    ai::AssistantMessage assistant;
    CHECK(assistant.content.empty());
    CHECK(assistant.api.empty());
    CHECK(assistant.stop_reason == ai::AssistantStopReason::Stop);

    ai::AiContext context;
    CHECK(context.system_prompt == std::nullopt);
    CHECK(context.messages.empty());
    CHECK(context.tools.empty());

    ai::AssistantStartEvent start;
    CHECK(start.partial.content.empty());

    ai::AssistantErrorEvent error;
    CHECK(error.error.content.empty());
    CHECK_FALSE(error.failure.has_value());
}
