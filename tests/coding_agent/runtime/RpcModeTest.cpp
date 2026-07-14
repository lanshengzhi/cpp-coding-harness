#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/RpcMode.hpp"
#include "ai/providers/FakeChatClient.hpp"
#include "../../support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using JsonObject = util::JsonValue::object_t;

struct TranscriptResult {
    int exit_code{0};
    std::vector<JsonObject> records;
    std::string workspace;
    std::string session_id;
};

class FailingChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest&,
        ai::AssistantEventSink) override {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "synthetic provider failure"));
    }
};

std::vector<JsonObject> parse_records(const std::string& output) {
    std::vector<JsonObject> records;
    std::istringstream lines{output};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = util::read_json<util::JsonValue>(line);
        REQUIRE(parsed.has_value());
        const auto* object = parsed->get_if<JsonObject>();
        REQUIRE(object != nullptr);
        records.push_back(*object);
    }
    return records;
}

const std::string& string_at(const JsonObject& object, std::string_view key) {
    const auto it = object.find(std::string{key});
    REQUIRE(it != object.end());
    const auto* value = it->second.get_if<std::string>();
    REQUIRE(value != nullptr);
    return *value;
}

const JsonObject* find_response(
    const std::vector<JsonObject>& records,
    std::string_view command,
    std::string_view id) {
    for (const auto& record : records) {
        if (record.contains("type") && string_at(record, "type") == "response" &&
            record.contains("command") && string_at(record, "command") == command &&
            record.contains("id") && string_at(record, "id") == id) {
            return &record;
        }
    }
    return nullptr;
}

std::size_t find_record_index(
    const std::vector<JsonObject>& records,
    std::string_view type,
    std::size_t start = 0) {
    for (std::size_t index = start; index < records.size(); ++index) {
        if (records[index].contains("type") && string_at(records[index], "type") == type) {
            return index;
        }
    }
    return records.size();
}

std::size_t find_response_index(
    const std::vector<JsonObject>& records,
    std::string_view command,
    std::string_view id) {
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        if (record.contains("type") && string_at(record, "type") == "response" &&
            record.contains("command") && string_at(record, "command") == command &&
            record.contains("id") && string_at(record, "id") == id) {
            return index;
        }
    }
    return records.size();
}

std::size_t find_first_event_index(const std::vector<JsonObject>& records) {
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (records[index].contains("type") && string_at(records[index], "type") != "response") {
            return index;
        }
    }
    return records.size();
}

int count_responses(
    const std::vector<JsonObject>& records,
    std::string_view command,
    std::string_view id) {
    int count = 0;
    for (const auto& record : records) {
        if (record.contains("type") && string_at(record, "type") == "response" &&
            record.contains("command") && string_at(record, "command") == command &&
            record.contains("id") && string_at(record, "id") == id) {
            ++count;
        }
    }
    return count;
}

TranscriptResult run_transcript(
    std::string input,
    std::unique_ptr<ai::StreamingChatClient> chat_client = ai::providers::make_scripted_fake_chat_client()) {
    cch::tests::TempWorkspace workspace;

    coding_agent::CreateAgentSessionOptions options;
    options.session_path = workspace.path() / "rpc-session.jsonl";
    options.workspace = workspace.path();
    options.chat_client = std::move(chat_client);
    options.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    std::istringstream rpc_input{std::move(input)};
    std::ostringstream rpc_output;
    const auto exit_code = coding_agent::runtime::run_rpc_mode({
        .input = rpc_input,
        .output = rpc_output,
        .session = *created->session,
        .provider = "test-provider",
        .model = "test-model",
        .workspace = workspace.path(),
    });

    TranscriptResult result{
        .exit_code = exit_code,
        .records = parse_records(rpc_output.str()),
        .workspace = workspace.path().string(),
        .session_id = created->session_id,
    };
    REQUIRE(created->session->close().has_value());
    return result;
}

} // namespace

TEST_CASE("RPC mode reports safe session state through its interface", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.records.size() == 2);

    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    REQUIRE(state->at("success").get<bool>());
    const auto& data = state->at("data").get<JsonObject>();
    REQUIRE(data.size() == 5);
    CHECK(string_at(data, "provider") == "test-provider");
    CHECK(string_at(data, "model") == "test-model");
    CHECK(string_at(data, "sessionId") == result.session_id);
    CHECK(string_at(data, "workspace") == result.workspace);
    CHECK(static_cast<int>(data.at("messageCount").get<double>()) == 0);

    const auto* shutdown = find_response(result.records, "shutdown", "stop-1");
    REQUIRE(shutdown != nullptr);
    CHECK(shutdown->at("success").get<bool>());
}

TEST_CASE("RPC mode accepts a prompt before events and exposes committed assistant text", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"last-1\",\"type\":\"get_last_assistant_text\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    const auto agent_start_index = find_record_index(result.records, "agent_start");
    const auto terminal_index = find_record_index(result.records, "runtime_terminal");
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    REQUIRE(agent_start_index < result.records.size());
    REQUIRE(terminal_index < result.records.size());
    CHECK(response_index < first_event_index);
    CHECK(first_event_index <= agent_start_index);
    CHECK(agent_start_index < terminal_index);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(result.records[terminal_index].at("success").get<bool>());
    CHECK(string_at(result.records[terminal_index], "code") == "completed");

    int previous_seq = 0;
    for (std::size_t index = first_event_index; index <= terminal_index; ++index) {
        REQUIRE(result.records[index].contains("seq"));
        const auto seq = static_cast<int>(result.records[index].at("seq").get<double>());
        CHECK(seq > previous_seq);
        previous_seq = seq;
    }

    const auto last_text_index = find_response_index(result.records, "get_last_assistant_text", "last-1");
    REQUIRE(last_text_index < result.records.size());
    CHECK(terminal_index + 1 == last_text_index);
    const auto* last_text = find_response(result.records, "get_last_assistant_text", "last-1");
    REQUIRE(last_text != nullptr);
    CHECK(string_at(last_text->at("data").get<JsonObject>(), "text") == "fake: hello");

    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);
}

TEST_CASE("RPC mode treats user bash prefixes as ordinary prompts", "[coding-agent][runtime][rpc][user-bash]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"!echo visible\"}\n"
        "{\"id\":\"prompt-2\",\"type\":\"prompt\",\"message\":\"!!echo excluded-later\"}\n"
        "{\"id\":\"last-1\",\"type\":\"get_last_assistant_text\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    REQUIRE(find_response(result.records, "prompt", "prompt-1") != nullptr);
    REQUIRE(find_response(result.records, "prompt", "prompt-2") != nullptr);
    const auto* last = find_response(result.records, "get_last_assistant_text", "last-1");
    REQUIRE(last != nullptr);
    CHECK(string_at(last->at("data").get<JsonObject>(), "text") == "fake: !!echo excluded-later");
}

TEST_CASE("RPC mode keeps one event sequence across prompts", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"first\"}\n"
        "{\"id\":\"prompt-2\",\"type\":\"prompt\",\"message\":\"second\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    REQUIRE(find_response(result.records, "prompt", "prompt-1") != nullptr);
    REQUIRE(find_response(result.records, "prompt", "prompt-2") != nullptr);

    int terminal_count = 0;
    int previous_seq = 0;
    for (const auto& record : result.records) {
        if (string_at(record, "type") == "response") {
            continue;
        }
        REQUIRE(record.contains("seq"));
        const auto seq = static_cast<int>(record.at("seq").get<double>());
        CHECK(seq > previous_seq);
        previous_seq = seq;
        if (string_at(record, "type") == "runtime_terminal") {
            ++terminal_count;
        }
    }
    CHECK(terminal_count == 2);
}

TEST_CASE("RPC mode recovers from invalid records and processes the next command", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "\n"
        "{bad json}\n"
        "[]\n"
        "{}\n"
        "{\"id\":5,\"type\":\"get_state\"}\n"
        "{\"id\":\"prompt-1\",\"type\":\"prompt\"}\n"
        "{\"id\":\"prompt-2\",\"type\":\"prompt\",\"message\":\"\"}\n"
        "{\"id\":\"prompt-3\",\"type\":\"prompt\",\"message\":7}\n"
        "{\"id\":\"unsupported-1\",\"type\":\"set_model\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n");

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.records.size() == 10);
    const std::vector<std::string> expected_commands{
        "invalid",
        "parse",
        "invalid",
        "invalid",
        "get_state",
        "prompt",
        "prompt",
        "prompt",
        "set_model",
        "get_state",
    };
    for (std::size_t index = 0; index < result.records.size(); ++index) {
        CHECK(string_at(result.records[index], "type") == "response");
        CHECK(string_at(result.records[index], "command") == expected_commands[index]);
        CHECK(result.records[index].at("success").get<bool>() == (index + 1 == result.records.size()));
    }

    const auto* recovered = find_response(result.records, "get_state", "state-1");
    REQUIRE(recovered != nullptr);
    CHECK(recovered->at("success").get<bool>());
}

TEST_CASE("RPC mode uses LF framing and processes the final record at EOF", "[coding-agent][runtime][rpc]") {
    std::string input = "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello";
    input += "\xE2\x80\xA8";
    input += "there\"}\r\n{\"id\":\"state-1\",\"type\":\"get_state\"}";

    const auto result = run_transcript(std::move(input));

    REQUIRE(result.exit_code == 0);
    const auto* prompt = find_response(result.records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK(prompt->at("success").get<bool>());
    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(state->at("success").get<bool>());
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);

    int response_count = 0;
    for (const auto& record : result.records) {
        if (string_at(record, "type") == "response") {
            ++response_count;
        }
    }
    CHECK(response_count == 2);
}

TEST_CASE("RPC mode terminates after an accepted prompt fails", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"fail\"}\n"
        "{\"id\":\"later\",\"type\":\"get_state\"}\n",
        std::make_unique<FailingChatClient>());

    REQUIRE(result.exit_code != 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    const auto terminal_index = find_record_index(result.records, "runtime_terminal");
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    REQUIRE(terminal_index < result.records.size());
    CHECK(response_index < first_event_index);
    CHECK(first_event_index <= terminal_index);
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK_FALSE(result.records[terminal_index].at("success").get<bool>());
    CHECK(terminal_index + 1 == result.records.size());
    CHECK(find_response(result.records, "get_state", "later") == nullptr);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
}

TEST_CASE("RPC mode treats slash-shaped input as an ordinary prompt", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"/exit\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto terminal_index = find_record_index(result.records, "runtime_terminal");
    REQUIRE(response_index < result.records.size());
    REQUIRE(terminal_index < result.records.size());
    CHECK(response_index < terminal_index);
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK(result.records[terminal_index].at("success").get<bool>());
    CHECK(string_at(result.records[terminal_index], "code") == "completed");

    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(state->at("success").get<bool>());
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);
}

TEST_CASE("RPC mode stops after a prompt requests shutdown", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"later\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto terminal_index = find_record_index(result.records, "runtime_terminal");
    REQUIRE(response_index < result.records.size());
    REQUIRE(terminal_index < result.records.size());
    CHECK(response_index < terminal_index);
    CHECK(result.records[terminal_index].at("success").get<bool>());
    CHECK(string_at(result.records[terminal_index], "code") == "completed");
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(find_response(result.records, "get_state", "later") != nullptr);
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
}
