#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/EventPrinter.hpp"
#include "coding_agent/runtime/JsonEventPrinter.hpp"

#include "util/Json.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace {
using JsonValue = cch::util::JsonValue;

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input{text};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            result.push_back(line);
        }
    }
    return result;
}

JsonValue parse_line(const std::string& line) {
    auto parsed = cch::util::read_json<JsonValue>(line);
    REQUIRE(parsed.has_value());
    return *parsed;
}

JsonValue::object_t object(const JsonValue& value) {
    return value.get<JsonValue::object_t>();
}

JsonValue::object_t object_at(const JsonValue::object_t& value, const std::string& key) {
    return value.at(key).get<JsonValue::object_t>();
}

JsonValue::array_t array_at(const JsonValue::object_t& value, const std::string& key) {
    return value.at(key).get<JsonValue::array_t>();
}

std::string string_at(const JsonValue::object_t& value, const std::string& key) {
    return value.at(key).get<std::string>();
}

int int_at(const JsonValue::object_t& value, const std::string& key) {
    return static_cast<int>(value.at(key).get<double>());
}

void check_no_cpp_envelope_fields(const JsonValue::object_t& record) {
    CHECK_FALSE(record.contains("schemaVersion"));
    CHECK_FALSE(record.contains("seq"));
    CHECK_FALSE(record.contains("contentStatus"));
}

cch::ai::AssistantMessage assistant_message(std::string text) {
    cch::ai::AssistantMessage message;
    message.content.emplace_back(cch::ai::text_content(std::move(text)));
    message.api = "openai-completions";
    message.provider = "fake";
    message.model = "fake-model";
    message.usage = cch::ai::Usage{
        .input = 1,
        .output = 2,
        .cache_read = 0,
        .cache_write = 0,
        .cache_write_1h = std::nullopt,
        .reasoning = 1,
        .total_tokens = 3,
        .cost = cch::ai::UsageCost{
            .input = 0.1,
            .output = 0.2,
            .cache_read = 0.0,
            .cache_write = 0.0,
            .total = 0.3,
        },
    };
    message.stop_reason = cch::ai::AssistantStopReason::Stop;
    message.timestamp = 1234;
    return message;
}
} // namespace

TEST_CASE("JSON event printer emits the exact v3 session header", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    cch::harness::session::SessionMetadata metadata;
    metadata.session_id = "session-1";
    metadata.created_at = "2026-06-20T00:00:00Z";
    metadata.workspace = "/tmp/workspace";
    metadata.provider = "fake";
    metadata.model = "fake-model";

    REQUIRE(printer.print_session_header(metadata).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    const auto& record = object(parse_line(emitted.front()));
    CHECK(record.size() == 5);
    CHECK(string_at(record, "type") == "session");
    CHECK(int_at(record, "version") == 3);
    CHECK(string_at(record, "id") == "session-1");
    CHECK(string_at(record, "timestamp") == "2026-06-20T00:00:00Z");
    CHECK(string_at(record, "cwd") == "/tmp/workspace");
    check_no_cpp_envelope_fields(record);
}

TEST_CASE(
    "JSON event printer emits direct semantically complete agent events",
    "[coding-agent][json-events][issue17][issue19]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    auto user = cch::ai::user_text_message("hello", 1000);
    auto assistant = assistant_message("answer");
    JsonValue::object_t tool_arguments;
    tool_arguments.emplace("path", JsonValue{"README.md"});
    assistant.content.emplace_back(cch::ai::tool_call_content(
        "call-1", "read", R"({"path":"README.md"})", JsonValue{tool_arguments}));
    auto tool_result = cch::ai::tool_result_message("call-1", "read", "file text", false, 1200);

    JsonValue::object_t args;
    args.emplace("path", JsonValue{"README.md"});

    REQUIRE(printer.print_agent_event(cch::agent::AgentStartEvent{}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::TurnStartEvent{}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::MessageStartEvent{cch::ai::MessageVariant{user}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::MessageUpdateEvent{
        cch::ai::MessageVariant{assistant},
        cch::ai::AssistantStreamEvent{cch::ai::TextDeltaEvent{0, "answer", assistant}}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolExecutionStartEvent{
        "call-1", "read", JsonValue{args}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolExecutionEndEvent{
        "call-1",
        "read",
        cch::agent::AsyncToolExecutionResult{
            tool_result.content,
            std::nullopt,
            false,
            true},
        false}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::MessageEndEvent{cch::ai::MessageVariant{tool_result}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::TurnEndEvent{
        cch::ai::MessageVariant{assistant}, {tool_result}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::AgentEndEvent{
        {cch::ai::MessageVariant{user}, cch::ai::MessageVariant{assistant}, cch::ai::MessageVariant{tool_result}}}).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 9);
    for (const auto& line : emitted) {
        check_no_cpp_envelope_fields(object(parse_line(line)));
        CHECK(line.find("runtime_terminal") == std::string::npos);
    }

    const auto& message_start = object(parse_line(emitted[2]));
    CHECK(string_at(message_start, "type") == "message_start");
    const auto& start_message = object_at(message_start, "message");
    CHECK(string_at(start_message, "role") == "user");
    CHECK(string_at(object(array_at(start_message, "content").front()), "text") == "hello");

    const auto& message_update = object(parse_line(emitted[3]));
    CHECK(string_at(message_update, "type") == "message_update");
    const auto& update_message = object_at(message_update, "message");
    CHECK(string_at(update_message, "role") == "assistant");
    CHECK(string_at(update_message, "api") == "openai-completions");
    CHECK(string_at(update_message, "provider") == "fake");
    CHECK(string_at(update_message, "model") == "fake-model");
    CHECK(int_at(update_message, "timestamp") == 1234);
    CHECK(update_message.contains("usage"));
    const auto& update_usage = object_at(update_message, "usage");
    CHECK(int_at(update_usage, "input") == 1);
    CHECK(int_at(update_usage, "output") == 2);
    CHECK(int_at(update_usage, "reasoning") == 1);
    CHECK(int_at(update_usage, "totalTokens") == 3);
    const auto& update_cost = object_at(update_usage, "cost");
    CHECK(update_cost.at("input").get<double>() == 0.1);
    CHECK(update_cost.at("output").get<double>() == 0.2);
    CHECK(update_cost.at("cacheRead").get<double>() == 0.0);
    CHECK(update_cost.at("cacheWrite").get<double>() == 0.0);
    CHECK(update_cost.at("total").get<double>() == 0.3);
    CHECK(string_at(update_message, "stopReason") == "stop");
    const auto update_content = array_at(update_message, "content");
    REQUIRE(update_content.size() == 2);
    const auto tool_call = object(update_content[1]);
    CHECK(string_at(tool_call, "type") == "toolCall");
    CHECK(tool_call.contains("arguments"));
    CHECK_FALSE(tool_call.contains("rawArguments"));
    CHECK_FALSE(tool_call.contains("argumentsValid"));
    CHECK_FALSE(tool_call.contains("argumentError"));
    const auto& assistant_event = object_at(message_update, "assistantMessageEvent");
    CHECK(string_at(assistant_event, "type") == "text_delta");
    CHECK(int_at(assistant_event, "contentIndex") == 0);
    CHECK(string_at(assistant_event, "delta") == "answer");
    const auto& partial = object_at(assistant_event, "partial");
    CHECK(string_at(partial, "role") == "assistant");
    CHECK(string_at(partial, "api") == "openai-completions");
    CHECK(string_at(partial, "provider") == "fake");
    CHECK(string_at(partial, "model") == "fake-model");
    CHECK(int_at(partial, "timestamp") == 1234);
    CHECK(int_at(object_at(partial, "usage"), "reasoning") == 1);

    const auto& tool_start = object(parse_line(emitted[4]));
    CHECK(string_at(tool_start, "type") == "tool_execution_start");
    CHECK(string_at(tool_start, "toolCallId") == "call-1");
    CHECK(string_at(tool_start, "toolName") == "read");
    CHECK(string_at(object_at(tool_start, "args"), "path") == "README.md");

    const auto& tool_end = object(parse_line(emitted[5]));
    CHECK(string_at(tool_end, "type") == "tool_execution_end");
    CHECK(tool_end.at("isError").get<bool>() == false);
    const auto& result = object_at(tool_end, "result");
    CHECK(string_at(object(array_at(result, "content").front()), "text") == "file text");
    CHECK(result.at("details").holds<JsonValue::null_t>());
    CHECK(result.at("terminate").get<bool>() == true);

    const auto& turn_end = object(parse_line(emitted[7]));
    CHECK(string_at(turn_end, "type") == "turn_end");
    CHECK(string_at(object_at(turn_end, "message"), "role") == "assistant");
    REQUIRE(array_at(turn_end, "toolResults").size() == 1);
    CHECK(string_at(object(array_at(turn_end, "toolResults").front()), "role") == "toolResult");

    const auto& agent_end = object(parse_line(emitted[8]));
    CHECK(string_at(agent_end, "type") == "agent_end");
    REQUIRE(array_at(agent_end, "messages").size() == 3);
}

TEST_CASE("JSON event printer preserves payload structure while redacting and bounding output", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    const std::string long_text(9000, 'x');
    const std::string boundary_secret = std::string(8189, 'x') + "sk-ABCDEFGHIJK" + long_text;
    auto user = cch::ai::user_text_message(boundary_secret, 1000);

    JsonValue::array_t oversized_array;
    for (int value = 0; value < 300; ++value) {
        oversized_array.emplace_back(value);
    }

    JsonValue::object_t args;
    args.emplace("token", JsonValue{"super-secret"});
    args.emplace("nested", JsonValue{JsonValue::object_t{{"note", JsonValue{"password=hunter2"}}}});
    args.emplace("many", JsonValue{oversized_array});

    JsonValue::object_t details;
    details.emplace("authorization", JsonValue{"Bearer hidden"});
    details.emplace("safe", JsonValue{"value"});

    REQUIRE(printer.print_agent_event(cch::agent::MessageStartEvent{cch::ai::MessageVariant{user}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolExecutionStartEvent{
        "call-1", "bash", JsonValue{args}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolExecutionEndEvent{
        "call-1",
        "bash",
        cch::agent::AsyncToolExecutionResult{
            {cch::ai::text_content("API_KEY='single-quoted-secret value' " + long_text)},
            JsonValue{details},
            false,
            false},
        false}).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 3);
    CHECK(output.str().find("super-secret") == std::string::npos);
    CHECK(output.str().find("sk-ABCDEFGHIJK") == std::string::npos);
    CHECK(output.str().find("hunter2") == std::string::npos);
    CHECK(output.str().find("Bearer hidden") == std::string::npos);
    CHECK(output.str().find("single-quoted-secret") == std::string::npos);

    const auto& message = object_at(object(parse_line(emitted[0])), "message");
    const auto redacted_user_text = string_at(object(array_at(message, "content").front()), "text");
    CHECK(redacted_user_text.find("[REDACTED]") != std::string::npos);
    CHECK(redacted_user_text.size() <= 8192);

    const auto& start_args = object_at(object(parse_line(emitted[1])), "args");
    CHECK(string_at(start_args, "token") == "[REDACTED]");
    CHECK(string_at(object_at(start_args, "nested"), "note").find("[REDACTED]") != std::string::npos);
    CHECK(array_at(start_args, "many").size() == 256);

    const auto& result = object_at(object(parse_line(emitted[2])), "result");
    const auto result_text = string_at(object(array_at(result, "content").front()), "text");
    CHECK(result_text.find("[REDACTED]") != std::string::npos);
    CHECK(result_text.size() <= 8192);
    CHECK(string_at(object_at(result, "details"), "authorization") == "[REDACTED]");
    CHECK(string_at(object_at(result, "details"), "safe") == "value");
}

TEST_CASE(
    "text and JSON frontends share bounded redaction for tool errors",
    "[coding_agent][runtime][tool-errors][issue72]") {
    const std::string secret = "sk-tool-error-secret-123456";
    const cch::agent::AgentLifecycleEvent event = cch::agent::ToolExecutionEndEvent{
        "call-1",
        "hostile-tool",
        cch::agent::AsyncToolExecutionResult{
            .content = {cch::ai::text_content("tool failed with " + secret + " " + std::string(9000, 'x'))},
            .details = std::nullopt,
            .is_error = false,
            .terminate = false},
        true};

    std::ostringstream text_output;
    cch::coding_agent::runtime::print_agent_event(event, text_output);
    std::ostringstream json_output;
    cch::coding_agent::runtime::JsonEventPrinter json_printer{json_output};
    REQUIRE(json_printer.print_agent_event(event).has_value());

    auto presented_text = text_output.str();
    const auto content_start = presented_text.find('\n') + 1;
    presented_text = presented_text.substr(content_start);
    REQUIRE(!presented_text.empty());
    presented_text.pop_back();

    const auto record = object(parse_line(lines(json_output.str()).front()));
    const auto& result = object_at(record, "result");
    const auto json_text = string_at(object(array_at(result, "content").front()), "text");
    CHECK(presented_text == json_text);
    CHECK(presented_text.find(secret) == std::string::npos);
    CHECK(presented_text.find("[REDACTED]") != std::string::npos);
    CHECK(presented_text.size() <= 8192);
}

TEST_CASE("JSON event printer keeps bounded text valid UTF-8", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    std::string text(8191, 'x');
    text += "€";
    REQUIRE(printer.print_agent_event(cch::agent::MessageStartEvent{
        cch::ai::MessageVariant{cch::ai::user_text_message(std::move(text), 1000)}}).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    const auto record = object(parse_line(emitted.front()));
    const auto message = object_at(record, "message");
    const auto bounded_text = string_at(object(array_at(message, "content").front()), "text");
    CHECK(bounded_text.size() <= 8192);
}

TEST_CASE("JSON event printer redacts and bounds terminal diagnostics and partial content", "[coding-agent][json-events][issue16]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    const std::string secret = "sk-terminal-secret-123456";
    auto terminal = assistant_message("partial draft " + secret);
    terminal.stop_reason = cch::ai::AssistantStopReason::Error;
    terminal.error_message = "provider failed with " + secret + " " + std::string(9000, 'x');

    REQUIRE(printer.print_agent_event(cch::agent::MessageEndEvent{
        cch::ai::MessageVariant{terminal}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::TurnEndEvent{
        cch::ai::MessageVariant{terminal}, {}}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::AgentEndEvent{
        {cch::ai::MessageVariant{terminal}}}).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 3);
    for (const auto& line : emitted) {
        CHECK(line.find(secret) == std::string::npos);
        CHECK(line.find("runtime_terminal") == std::string::npos);
    }

    const auto& message_end = object(parse_line(emitted[0]));
    const auto& ended = object_at(message_end, "message");
    CHECK(string_at(ended, "stopReason") == "error");
    const auto diagnostic = string_at(ended, "errorMessage");
    CHECK(diagnostic.find("[REDACTED]") != std::string::npos);
    CHECK(diagnostic.size() <= 8192);
    const auto partial_text = string_at(object(array_at(ended, "content").front()), "text");
    CHECK(partial_text == "partial draft [REDACTED]");

    const auto& turn_end = object(parse_line(emitted[1]));
    CHECK(string_at(object_at(turn_end, "message"), "stopReason") == "error");
    CHECK(array_at(turn_end, "toolResults").empty());

    const auto& agent_end = object(parse_line(emitted[2]));
    REQUIRE(array_at(agent_end, "messages").size() == 1);
    const auto& final_message = object(array_at(agent_end, "messages").front());
    CHECK(string_at(final_message, "stopReason") == "error");
    CHECK(string_at(final_message, "errorMessage").find("[REDACTED]") != std::string::npos);
}

TEST_CASE("JSON event printer enforces an aggregate record budget", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    std::vector<cch::ai::MessageVariant> messages;
    for (int index = 0; index < 256; ++index) {
        messages.emplace_back(cch::ai::user_text_message(std::string(8192, 'x'), index));
    }
    REQUIRE(printer.print_agent_event(cch::agent::AgentEndEvent{std::move(messages)}).has_value());

    const auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().size() <= 512 * 1024);
    const auto record = object(parse_line(emitted.front()));
    CHECK(string_at(record, "type") == "agent_end");
    CHECK(record.contains("messages"));
}
