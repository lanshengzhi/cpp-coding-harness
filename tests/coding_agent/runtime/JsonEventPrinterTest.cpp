#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/JsonEventPrinter.hpp"

#include "util/Json.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace {
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

cch::util::JsonValue parse_line(const std::string& line) {
    auto parsed = cch::util::read_json<cch::util::JsonValue>(line);
    REQUIRE(parsed.has_value());
    return *parsed;
}

const cch::util::JsonValue::object_t& object(const cch::util::JsonValue& value) {
    return value.get<cch::util::JsonValue::object_t>();
}

std::string string_at(const cch::util::JsonValue::object_t& value, const std::string& key) {
    return value.at(key).get<std::string>();
}

int int_at(const cch::util::JsonValue::object_t& value, const std::string& key) {
    return static_cast<int>(value.at(key).get<double>());
}
} // namespace

TEST_CASE("JSON event printer emits session header with schema metadata", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    cch::harness::session::SessionMetadata metadata;
    metadata.session_id = "session-1";
    metadata.created_at = "2026-06-20T00:00:00Z";
    metadata.workspace = "/tmp/workspace";
    metadata.provider = "fake";
    metadata.model = "fake-model";

    auto printed = printer.print_session_header(metadata);

    REQUIRE(printed.has_value());
    auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    auto record = object(parse_line(emitted.front()));
    CHECK(string_at(record, "type") == "session");
    CHECK(int_at(record, "schemaVersion") == 1);
    CHECK(int_at(record, "seq") == 1);
    CHECK(string_at(record, "id") == "session-1");
    CHECK(string_at(record, "cwd") == "/tmp/workspace");
    CHECK(record.find("baseUrl") == record.end());
    CHECK(record.find("apiKeyEnv") == record.end());
}

TEST_CASE("JSON event printer emits escaped assistant text delta", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    auto printed = printer.print_agent_event(cch::agent::MessageUpdateEvent{2, "hello\n\"json\"\r"});

    REQUIRE(printed.has_value());
    auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    auto record = object(parse_line(emitted.front()));
    CHECK(string_at(record, "type") == "message_update");
    CHECK(int_at(record, "turn") == 2);
    const auto& assistant = record.at("assistantMessageEvent").get<cch::util::JsonValue::object_t>();
    CHECK(string_at(assistant, "type") == "text_delta");
    CHECK(string_at(assistant, "delta") == "hello\n\"json\"\r");
}

TEST_CASE("JSON event printer omits prompt and tool result bodies by policy", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    REQUIRE(printer.print_agent_event(cch::agent::AgentStartEvent{"secret prompt TOKEN=abc123"}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolExecutionEndEvent{1, "call-1", "bash", true, "SECRET=abc123\n/path/to/private"}).has_value());

    auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 2);
    auto start = object(parse_line(emitted[0]));
    CHECK(string_at(start, "type") == "agent_start");
    CHECK(start.find("prompt") == start.end());
    CHECK(emitted[0].find("secret prompt") == std::string::npos);

    auto tool = object(parse_line(emitted[1]));
    CHECK(string_at(tool, "type") == "tool_execution_end");
    CHECK(string_at(tool, "toolCallId") == "call-1");
    CHECK(tool.find("content") == tool.end());
    CHECK(emitted[1].find("SECRET=abc123") == std::string::npos);
    const auto& status = tool.at("contentStatus").get<cch::util::JsonValue::object_t>();
    CHECK(string_at(status, "status") == "omitted");
    CHECK(string_at(status, "reason") == "unsupported_in_v1");
}

TEST_CASE("JSON event printer skips unsupported sensitive stream variants", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    REQUIRE(printer.print_agent_event(cch::agent::ThinkingUpdateEvent{1, 0, "private reasoning"}).has_value());
    REQUIRE(printer.print_agent_event(cch::agent::ToolCallStreamUpdateEvent{1, 1, "{\"token\":\"abc\"}"}).has_value());

    CHECK(output.str().empty());
}

TEST_CASE("JSON event printer emits durable terminal records", "[coding-agent][json-events]") {
    std::ostringstream output;
    cch::coding_agent::runtime::JsonEventPrinter printer{output};

    REQUIRE(printer.print_terminal(false, "max_turns_exceeded", "max turns exceeded").has_value());

    auto emitted = lines(output.str());
    REQUIRE(emitted.size() == 1);
    auto record = object(parse_line(emitted.front()));
    CHECK(string_at(record, "type") == "runtime_terminal");
    CHECK(string_at(record, "code") == "max_turns_exceeded");
    CHECK(record.at("success").get<bool>() == false);
}
