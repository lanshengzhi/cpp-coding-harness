// AI message-model contract cases: passive value shapes and the LLM message
// conversion helpers, none of which touch a JSON surface. The pi message JSON
// cases moved to `tests/harness/session/SessionMessageJsonTest.cpp` when the
// session module took ownership of that wire shape (ADR 0056).

#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <type_traits>
#include <variant>

using namespace cch;

TEST_CASE("AI contracts are aggregate-friendly passive value types", "[ai][u3][contract][compat-pi]") {
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

TEST_CASE("UserMessage content is a passive sum type with string and block-array alternatives",
        "[ai][u3][contract][issue365][compat-pi]") {
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

// ── LLM conversion tests ──

TEST_CASE("bash_execution_to_user_message produces formatted text", "[ai][extended][convert][compat-pi]") {
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

TEST_CASE("bash_execution_to_user_message reports non-zero exit code", "[ai][extended][convert][compat-pi]") {
    ai::BashExecutionMessage bash;
    bash.command = "false";
    bash.output = "";
    bash.exit_code = 1;
    bash.timestamp = 1718000000002;

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("Command exited with code 1") != std::string::npos);
}

TEST_CASE("bash_execution_to_user_message reports cancellation", "[ai][extended][convert][compat-pi]") {
    ai::BashExecutionMessage bash;
    bash.command = "sleep 999";
    bash.output = "";
    bash.cancelled = true;

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("(command cancelled)") != std::string::npos);
}

TEST_CASE("bash_execution_to_user_message reports truncation with path", "[ai][extended][convert][compat-pi]") {
    ai::BashExecutionMessage bash;
    bash.command = "cat huge.log";
    bash.output = "truncated...";
    bash.truncated = true;
    bash.full_output_path = "/tmp/bash-output-12345.txt";

    auto msg = ai::bash_execution_to_user_message(bash);
    const auto& text = std::get<ai::TextContent>(std::get<std::vector<ai::Content>>(msg.content)[0]);
    CHECK(text.text.find("[Output truncated. Full output: /tmp/bash-output-12345.txt]") != std::string::npos);
}

TEST_CASE("compaction_summary_to_user_message wraps with prefix and suffix", "[ai][extended][convert][compat-pi]") {
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

TEST_CASE("branch_summary_to_user_message wraps with prefix and suffix", "[ai][extended][convert][compat-pi]") {
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

TEST_CASE("custom_message_to_user_message preserves ordered text and image blocks",
        "[ai][extended][convert][issue22][compat-pi]") {
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

TEST_CASE("custom_message_to_user_message preserves empty content", "[ai][extended][convert][issue22][compat-pi]") {
    ai::CustomMessage custom;
    custom.custom_type = "ext";
    custom.timestamp = 1718000000006;

    auto msg = ai::custom_message_to_user_message(custom);

    CHECK(std::get<std::vector<ai::Content>>(msg.content).empty());
    CHECK(msg.timestamp == 1718000000006);
}

TEST_CASE("default-constructed AI contracts are empty passive values", "[ai][contract][issue372][compat-pi]") {
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
