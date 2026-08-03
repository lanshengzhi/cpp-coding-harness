#include "../../../third_party/catch2/catch_test_macros.hpp"
#include "support/ModelsFixture.hpp"

#include "coding_agent/runtime/RpcMode.hpp"
#include "coding_agent/runtime/RpcJsonl.hpp"
#include "ai/providers/FakeProvider.hpp"
#include "support/TempWorkspace.hpp"
#include "util/Json.hpp"

#include <filesystem>
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

/// A host client whose first accepted call terminates with an error/aborted
/// outcome (partial content preserved) while later calls answer normally.
class TerminalThenRecoverChatClient final : public ai::StreamingChatClient {
public:
    explicit TerminalThenRecoverChatClient(ai::AssistantStopReason reason)
        : reason_(reason) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ++request_count;
        ai::AssistantMessage message;
        message.provider = "host";
        message.api = "host";
        message.model = request.model.id;
        if (request_count == 1) {
            message.stop_reason = reason_;
            message.error_message = "host transport lost";
            message.content.emplace_back(ai::text_content("partial draft"));
            if (sink) {
                if (auto started = sink(ai::AssistantStartEvent{message}); !started) {
                    co_return std::unexpected(started.error());
                }
                if (auto delta = sink(ai::TextDeltaEvent{0, "partial draft", message}); !delta) {
                    co_return std::unexpected(delta.error());
                }
                if (auto ended = sink(ai::AssistantErrorEvent{reason_, message}); !ended) {
                    co_return std::unexpected(ended.error());
                }
            }
            co_return message;
        }
        message.stop_reason = ai::AssistantStopReason::Stop;
        message.content.emplace_back(ai::text_content("recovered answer"));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{message}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto delta = sink(ai::TextDeltaEvent{0, "recovered answer", message}); !delta) {
                co_return std::unexpected(delta.error());
            }
            if (auto done = sink(ai::AssistantDoneEvent{message.stop_reason, message}); !done) {
                co_return std::unexpected(done.error());
            }
        }
        co_return message;
    }

    int request_count{0};

private:
    ai::AssistantStopReason reason_;
};

/// A host client whose accepted call terminates with an error outcome
/// carrying a caller-chosen diagnostic, emitted before any start event.
class SecretDiagnosticChatClient final : public ai::StreamingChatClient {
public:
    explicit SecretDiagnosticChatClient(std::string terminal_diagnostic)
        : diagnostic_(std::move(terminal_diagnostic)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ai::AssistantMessage terminal;
        terminal.provider = "host";
        terminal.api = "host";
        terminal.model = request.model.id;
        terminal.stop_reason = ai::AssistantStopReason::Error;
        terminal.error_message = diagnostic_;
        if (sink) {
            if (auto ended = sink(ai::AssistantErrorEvent{terminal.stop_reason, terminal});
                !ended) {
                co_return std::unexpected(ended.error());
            }
        }
        co_return terminal;
    }

private:
    std::string diagnostic_;
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

std::size_t find_assistant_message_end_index(const std::vector<JsonObject>& records) {
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto type = records[index].find("type");
        if (type == records[index].end()) {
            continue;
        }
        const auto* type_value = type->second.get_if<std::string>();
        if (type_value == nullptr || *type_value != "message_end") {
            continue;
        }
        const auto message = records[index].find("message");
        if (message == records[index].end()) {
            continue;
        }
        const auto* message_object = message->second.get_if<JsonObject>();
        if (message_object == nullptr) {
            continue;
        }
        const auto role = message_object->find("role");
        if (role == message_object->end()) {
            continue;
        }
        if (const auto* role_value = role->second.get_if<std::string>();
            role_value && *role_value == "assistant") {
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
    std::unique_ptr<ai::StreamingChatClient> chat_client = ai::providers::make_scripted_fake_stream(),
    bool close_before_run = false,
    bool in_memory = false) {
    cch::tests::TempWorkspace workspace;

    tests::ModelsSessionOptions options;
    if (in_memory) {
        options.session_target = coding_agent::InMemorySessionTarget{};
    } else {
        options.session_target = coding_agent::ExplicitNewSessionTarget{
            workspace.path() / "rpc-session.jsonl"};
    }
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(chat_client));
    options.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    if (close_before_run) {
        created->session->close();
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
    created->session->close();
    return result;
}

} // namespace

TEST_CASE("RPC errors are redacted and bounded on a UTF-8 boundary", "[coding_agent][runtime][rpc][issue72]") {
    const std::string secret = "sk-rpc-error-secret-123456";
    const auto redacted = coding_agent::runtime::rpc_jsonl::bounded_error(
        "provider rejected " + secret + " " + std::string(600, 'x'));

    std::string multibyte = std::string(510, 'a');
    multibyte += "\xf0\x9f\x99\x82";
    multibyte += std::string(100, 'b');
    const auto bounded = coding_agent::runtime::rpc_jsonl::bounded_error(std::move(multibyte));

    CHECK(redacted.size() <= 512);
    CHECK(redacted.find(secret) == std::string::npos);
    CHECK(redacted.find("[REDACTED]") != std::string::npos);
    CHECK(bounded == std::string(510, 'a'));
}

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
    REQUIRE(data.size() == 6);
    CHECK(string_at(data, "provider") == "test-provider");
    CHECK(string_at(data, "model") == "test-model");
    CHECK(string_at(data, "sessionId") == result.session_id);
    CHECK(string_at(data, "workspace") == result.workspace);
    CHECK(static_cast<int>(data.at("messageCount").get<double>()) == 0);
    // Persisted sessions report their exact transcript path, matching pi's
    // RpcSessionState.sessionFile.
    CHECK(string_at(data, "sessionFile") ==
          (std::filesystem::path(result.workspace) / "rpc-session.jsonl").string());

    const auto* shutdown = find_response(result.records, "shutdown", "stop-1");
    REQUIRE(shutdown != nullptr);
    CHECK(shutdown->at("success").get<bool>());
}

TEST_CASE("RPC mode omits sessionFile for an in-memory session", "[coding-agent][runtime][rpc]") {
    const auto result = run_transcript(
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        ai::providers::make_scripted_fake_stream(),
        false,
        true);

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.records.size() == 2);

    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    REQUIRE(state->at("success").get<bool>());
    const auto& data = state->at("data").get<JsonObject>();
    // In-memory operation is identified by the absent key, never an empty path.
    CHECK_FALSE(data.contains("sessionFile"));
    REQUIRE(data.size() == 5);
    CHECK(string_at(data, "sessionId") == result.session_id);
    CHECK(string_at(data, "workspace") == result.workspace);
}

TEST_CASE(
    "RPC mode acknowledges an accepted prompt before direct events",
    "[coding-agent][runtime][rpc][issue17][issue19]") {
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

    const auto message_update_index = find_record_index(result.records, "message_update");
    REQUIRE(message_update_index < result.records.size());
    const auto& assistant = result.records[message_update_index].at("message").get<JsonObject>();
    CHECK(string_at(assistant, "api") == "scripted-fake");
    CHECK(string_at(assistant, "provider") == "fake");
    CHECK(string_at(assistant, "model") == "host-client");
    CHECK(assistant.at("timestamp").get<double>() > 0.0);
    const auto& usage = assistant.at("usage").get<JsonObject>();
    CHECK(usage.at("input").get<double>() == 0.0);
    CHECK(usage.at("output").get<double>() == 0.0);
    CHECK(usage.at("cacheRead").get<double>() == 0.0);
    CHECK(usage.at("cacheWrite").get<double>() == 0.0);
    CHECK(usage.at("totalTokens").get<double>() == 0.0);
    CHECK_FALSE(usage.contains("reasoning"));
    const auto& cost = usage.at("cost").get<JsonObject>();
    CHECK(cost.at("input").get<double>() == 0.0);
    CHECK(cost.at("output").get<double>() == 0.0);
    CHECK(cost.at("cacheRead").get<double>() == 0.0);
    CHECK(cost.at("cacheWrite").get<double>() == 0.0);
    CHECK(cost.at("total").get<double>() == 0.0);

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

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-flush-session.jsonl"};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(chat_client));
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
    created->session->close();
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

TEST_CASE("RPC mode keeps one event stream after a weak subscriber failure", "[coding-agent][runtime][rpc][issue36]") {
    cch::tests::TempWorkspace workspace;
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-subscriber-failure.jsonl"};
    options.workspace = workspace.path();
    options.models = ai::providers::make_scripted_fake_models();
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
    REQUIRE(records.size() == 14);
    const auto* prompt = find_response(records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK(prompt->at("success").get<bool>());
    CHECK(count_responses(records, "prompt", "prompt-1") == 1);
    const auto* state = find_response(records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);
    CHECK(find_response(records, "shutdown", "stop-1") != nullptr);
    CHECK(find_record_index(records, "agent_start") < records.size());
    CHECK(find_record_index(records, "agent_end") < records.size());
    CHECK_FALSE(static_cast<bool>(*rejecting));
    created->session->close();
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

TEST_CASE("RPC mode reports an accepted error terminal outcome only through events", "[coding-agent][runtime][rpc][issue16]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"state-1\",\"type\":\"get_state\"}\n"
        "{\"id\":\"prompt-2\",\"type\":\"prompt\",\"message\":\"again\"}\n"
        "{\"id\":\"last-1\",\"type\":\"get_last_assistant_text\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        std::make_unique<TerminalThenRecoverChatClient>(ai::AssistantStopReason::Error));

    REQUIRE(result.exit_code == 0);

    // Exactly one successful acknowledgement, emitted before the prompt's events.
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK(response_index < first_event_index);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());

    // The terminal outcome rides the ordinary lifecycle records in order.
    const auto message_end_index = find_assistant_message_end_index(result.records);
    const auto turn_end_index = find_record_index(result.records, "turn_end");
    const auto agent_end_index = find_record_index(result.records, "agent_end");
    REQUIRE(message_end_index < result.records.size());
    REQUIRE(turn_end_index < result.records.size());
    REQUIRE(agent_end_index < result.records.size());
    CHECK(message_end_index < turn_end_index);
    CHECK(turn_end_index < agent_end_index);

    const auto& ended = result.records[message_end_index].at("message").get<JsonObject>();
    CHECK(string_at(ended, "stopReason") == "error");
    CHECK(string_at(ended, "errorMessage") == "host transport lost");
    const auto& turn_message = result.records[turn_end_index].at("message").get<JsonObject>();
    CHECK(string_at(turn_message, "stopReason") == "error");
    CHECK(result.records[turn_end_index].at("toolResults").get<util::JsonValue::array_t>().empty());

    // Later commands keep working after the terminal outcome.
    const auto* state = find_response(result.records, "get_state", "state-1");
    REQUIRE(state != nullptr);
    CHECK(state->at("success").get<bool>());
    CHECK(static_cast<int>(state->at("data").get<JsonObject>().at("messageCount").get<double>()) == 2);
    const auto* second = find_response(result.records, "prompt", "prompt-2");
    REQUIRE(second != nullptr);
    CHECK(second->at("success").get<bool>());
    CHECK(count_responses(result.records, "prompt", "prompt-2") == 1);
    const auto* last = find_response(result.records, "get_last_assistant_text", "last-1");
    REQUIRE(last != nullptr);
    CHECK(string_at(last->at("data").get<JsonObject>(), "text") == "recovered answer");
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
}

TEST_CASE("RPC mode reports an accepted aborted terminal outcome only through events", "[coding-agent][runtime][rpc][issue16]") {
    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        std::make_unique<TerminalThenRecoverChatClient>(ai::AssistantStopReason::Aborted));

    REQUIRE(result.exit_code == 0);
    const auto response_index = find_response_index(result.records, "prompt", "prompt-1");
    const auto first_event_index = find_first_event_index(result.records);
    REQUIRE(response_index < result.records.size());
    REQUIRE(first_event_index < result.records.size());
    CHECK(result.records[response_index].at("success").get<bool>());
    CHECK(response_index < first_event_index);
    CHECK(count_responses(result.records, "prompt", "prompt-1") == 1);
    CHECK(find_record_index(result.records, "runtime_terminal") == result.records.size());

    const auto message_end_index = find_assistant_message_end_index(result.records);
    REQUIRE(message_end_index < result.records.size());
    const auto& ended = result.records[message_end_index].at("message").get<JsonObject>();
    CHECK(string_at(ended, "stopReason") == "aborted");
    CHECK(string_at(ended, "errorMessage") == "host transport lost");
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
}

TEST_CASE("RPC mode redacts and bounds terminal diagnostics in event records", "[coding-agent][runtime][rpc][issue16]") {
    const std::string secret = "sk-rpc-terminal-secret-123456";
    std::string diagnostic = "provider rejected " + secret + " ";
    diagnostic += std::string(9000, 'x');

    const auto result = run_transcript(
        "{\"id\":\"prompt-1\",\"type\":\"prompt\",\"message\":\"hello\"}\n"
        "{\"id\":\"stop-1\",\"type\":\"shutdown\"}\n",
        std::make_unique<SecretDiagnosticChatClient>(diagnostic));

    REQUIRE(result.exit_code == 0);
    const auto message_end_index = find_assistant_message_end_index(result.records);
    REQUIRE(message_end_index < result.records.size());
    const auto& ended = result.records[message_end_index].at("message").get<JsonObject>();
    const auto& presented = string_at(ended, "errorMessage");
    CHECK(presented.find(secret) == std::string::npos);
    CHECK(presented.find("[REDACTED]") != std::string::npos);
    CHECK(presented.size() <= 8192);
    CHECK(find_response(result.records, "shutdown", "stop-1") != nullptr);
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
        ai::providers::make_scripted_fake_stream(),
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

    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::ExplicitNewSessionTarget{
        workspace.path() / "rpc-busy-session.jsonl"};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_stream(std::move(chat_client));
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

    REQUIRE(created->session->prompt_blocking("outer").has_value());
    REQUIRE(nested_exit_code == 0);
    const auto records = parse_records(nested_output);
    REQUIRE(records.size() == 2);
    const auto* prompt = find_response(records, "prompt", "prompt-1");
    REQUIRE(prompt != nullptr);
    CHECK_FALSE(prompt->at("success").get<bool>());
    CHECK(string_at(*prompt, "error") == "session is busy (prompt already in flight)");
    CHECK(find_response(records, "shutdown", "stop-1") != nullptr);
    CHECK(find_first_event_index(records) == records.size());
    created->session->close();
}
