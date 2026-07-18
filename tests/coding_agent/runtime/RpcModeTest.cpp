#include "../../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/runtime/RpcMode.hpp"
#include "ai/providers/FakeChatClient.hpp"
#include "../../support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <functional>
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

class BusyProbeChatClient final : public ai::StreamingChatClient {
public:
    std::function<void()> on_stream;

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest&,
        ai::AssistantEventSink) override {
        on_stream();
        co_return ai::assistant_text_message("outer completed");
    }
};

class SyncCountingBuffer final : public std::stringbuf {
public:
    int sync_count{0};

protected:
    int sync() override {
        ++sync_count;
        return std::stringbuf::sync();
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
    std::unique_ptr<ai::StreamingChatClient> chat_client = ai::providers::make_scripted_fake_chat_client(),
    bool close_before_run = false) {
    cch::tests::TempWorkspace workspace;

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-session.jsonl"};
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
    if (close_before_run) {
        REQUIRE(created->session->close().has_value());
    }

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

TEST_CASE("RPC mode acknowledges an accepted prompt before direct events", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"last-1\",\"type\":\"get_last_assistant_text\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    const auto agent_start_index = find_record_index(result.records, "agent_start");
    const auto agent_end_index = find_record_index(result.records, "agent_end");
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    REQUIRE(agent_start_index < result.records.size());
    REQUIRE(agent_end_index < result.records.size());
    CHECK(response_index < first_event_index);
    CHECK(first_event_index == agent_start_index);
    CHECK(agent_start_index < agent_end_index);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());

    for (std::size_t index = first_event_index; index <= agent_end_index; ++index) {
        CHECK_FALSE(result.records[index].contains("schemaVersion"));
        CHECK_FALSE(result.records[index].contains("seq"));
        CHECK_FALSE(result.records[index].contains("contentStatus"));
    }

    const auto last_text_index = find_response_index(result.records, "get_last_assistant_text", "last-1");
    REQUIRE(last_text_index < result.records.size());
    CHECK(agent_end_index + 1 == last_text_index);
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

TEST_CASE("RPC mode flushes direct events while a prompt is running", "[coding-agent][runtime][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto chat_client = std::make_unique<BusyProbeChatClient>();
    auto* stream_probe = chat_client.get();

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-flush-session.jsonl"};
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

    SyncCountingBuffer buffer;
    std::ostream output{&buffer};
    int flushes_before_provider = 0;
    stream_probe->on_stream = [&] {
        flushes_before_provider = buffer.sync_count;
    };
    std::istringstream input{
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n"};

    const auto exit_code = coding_agent::runtime::run_rpc_mode({
        .input = input,
        .output = output,
        .session = *created->session,
        .provider = "test-provider",
        .model = "test-model",
        .workspace = workspace.path(),
    });

    REQUIRE(exit_code == 0);
    CHECK(flushes_before_provider >= 5);
    CHECK(find_response(parse_records(buffer.str()), "shutdown", "stop-1") != nullptr);
    REQUIRE(created->session->close().has_value());
}

TEST_CASE("RPC mode reuses direct event serialization across prompts", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"first\"}\n"
        "{\"id\":\"prompt-2\",\"type\":\"prompt\",\"message\":\"second\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    REQUIRE(find_response(result.records, "prompt", "prompt-1") != nullptr);
    REQUIRE(find_response(result.records, "prompt", "prompt-2") != nullptr);

    int agent_start_count = 0;
    for (const auto& record : result.records) {
        if (string_at(record, "type") == "response") {
            continue;
        }
        CHECK_FALSE(record.contains("schemaVersion"));
        CHECK_FALSE(record.contains("seq"));
        CHECK_FALSE(record.contains("contentStatus"));
        CHECK(string_at(record, "type") != "runtime_terminal");
        if (string_at(record, "type") == "agent_start") {
            ++agent_start_count;
        }
    }
    CHECK(agent_start_count == 2);
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

TEST_CASE("RPC mode reports accepted prompt failures only through events", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"fail\"}\n"
        "{\"id\":\"later\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        std::make_unique<FailingChatClient>());

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    const auto agent_end_index = find_record_index(result.records, "agent_end");
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    REQUIRE(agent_end_index < result.records.size());
    CHECK(response_index < first_event_index);
    CHECK(first_event_index <= agent_end_index);
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());
    CHECK(find_response(result.records, "get_state", "later") != nullptr);
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
}

TEST_CASE("RPC mode does not report rejection after accepted subscriber failure", "[coding-agent][runtime][rpc]") {
    cch::tests::TempWorkspace workspace;
    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-subscriber-failure.jsonl"};
    options.workspace = workspace.path();
    options.chat_client = ai::providers::make_scripted_fake_chat_client();
    options.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());

    auto rejecting = created->session->subscribe(
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "synthetic subscriber failure"));
            }
            return {};
        });
    REQUIRE(rejecting.has_value());

    std::istringstream input{
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n"};
    std::ostringstream output;
    const auto exit_code = coding_agent::runtime::run_rpc_mode({
        .input = input,
        .output = output,
        .session = *created->session,
        .provider = "test-provider",
        .model = "test-model",
        .workspace = workspace.path(),
    });

    REQUIRE(exit_code == 0);
    const auto records = parse_records(output.str());
    REQUIRE(records.size() == 3);
    const auto* prompt = find_response(records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK(prompt->at("success").get<bool>());
    CHECK(count_responses(records, "prompt", "prompt-1") == 1);
    CHECK(find_response(records, "get_state", "state-1") != nullptr);
    CHECK(find_response(records, "shutdown", "stop-1") != nullptr);
    CHECK(find_first_event_index(records) == records.size());
    REQUIRE(created->session->close().has_value());
}

TEST_CASE("RPC mode treats slash-shaped input as an ordinary prompt", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"/exit\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto agent_start_index = find_record_index(result.records, "agent_start");
    REQUIRE(response_index < result.records.size());
    REQUIRE(agent_start_index < result.records.size());
    CHECK(response_index < agent_start_index);
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());

    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(state->at("success").get<bool>());
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);
}

TEST_CASE("RPC mode stops after the shutdown response", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"later\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n");

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto agent_start_index = find_record_index(result.records, "agent_start");
    REQUIRE(response_index < result.records.size());
    REQUIRE(agent_start_index < result.records.size());
    CHECK(response_index < agent_start_index);
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(find_response(result.records, "get_state", "later") != nullptr);
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
}

TEST_CASE("RPC mode returns a correlated error when a closed session rejects prompt preflight", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        ai::providers::make_scripted_fake_chat_client(),
        true);

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.records.size() == 2);
    const auto* prompt = find_response(result.records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK_FALSE(prompt->at("success").get<bool>());
    CHECK(string_at(*prompt, "error") == "session is closed");
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
    CHECK(find_first_event_index(result.records) == result.records.size());
}

TEST_CASE("RPC mode returns a correlated error when a busy session rejects prompt preflight", "[coding-agent][runtime][rpc]") {
    cch::tests::TempWorkspace workspace;
    auto chat_client = std::make_unique<BusyProbeChatClient>();
    auto* busy_probe = chat_client.get();

    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-busy-session.jsonl"};
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

    int nested_exit_code = -1;
    std::string nested_output;
    busy_probe->on_stream = [&] {
        std::istringstream input{
            "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"nested\"}\n"
            "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n"};
        std::ostringstream output;
        nested_exit_code = coding_agent::runtime::run_rpc_mode({
            .input = input,
            .output = output,
            .session = *created->session,
            .provider = "test-provider",
            .model = "test-model",
            .workspace = workspace.path(),
        });
        nested_output = output.str();
    };

    REQUIRE(created->session->prompt("outer").has_value());
    REQUIRE(nested_exit_code == 0);
    const auto records = parse_records(nested_output);
    REQUIRE(records.size() == 2);
    const auto* prompt = find_response(records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK_FALSE(prompt->at("success").get<bool>());
    CHECK(string_at(*prompt, "error") == "session is busy (prompt already in flight)");
    CHECK(find_response(records, "shutdown", "stop-1") != nullptr);
    CHECK(find_first_event_index(records) == records.size());
    REQUIRE(created->session->close().has_value());
}
