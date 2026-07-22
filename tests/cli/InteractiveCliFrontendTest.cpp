#include "../../third_party/catch2/catch_test_macros.hpp"

#include "cli/InteractiveCliFrontend.hpp"
#include "cli/JsonCliRenderer.hpp"
#include "cli/TextCliRenderer.hpp"

#include "ai/providers/FakeChatClient.hpp"
#include "../../include/cch/coding_agent/Sdk.hpp"
#include "util/Json.hpp"
#include "../support/TempWorkspace.hpp"

#include <sstream>
#include <streambuf>
#include <string>
#include <utility>

using namespace cch;

namespace {

class FailingStreambuf final : public std::streambuf {
protected:
    int_type overflow(int_type) override { return traits_type::eof(); }
};

/// A host client whose accepted call streams a partial text delta, terminates
/// with one error/aborted event carrying the final Assistant Message, and
/// returns that same message as a successful C++ value.
class TerminalOutcomeChatClient final : public ai::StreamingChatClient {
public:
    TerminalOutcomeChatClient(
        ai::AssistantStopReason reason,
        std::string diagnostic,
        std::string partial = "partial draft")
        : reason_(reason), diagnostic_(std::move(diagnostic)), partial_(std::move(partial)) {}

    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ++request_count;
        ai::AssistantMessage terminal;
        terminal.provider = "host";
        terminal.api = "host";
        terminal.model = request.model;
        terminal.stop_reason = reason_;
        terminal.error_message = diagnostic_;
        terminal.content.emplace_back(ai::text_content(partial_));
        if (sink) {
            if (auto started = sink(ai::AssistantStartEvent{terminal}); !started) {
                co_return std::unexpected(started.error());
            }
            if (auto delta = sink(ai::TextDeltaEvent{0, partial_, terminal}); !delta) {
                co_return std::unexpected(delta.error());
            }
            if (auto ended = sink(ai::AssistantErrorEvent{reason_, terminal}); !ended) {
                co_return std::unexpected(ended.error());
            }
        }
        co_return terminal;
    }

    int request_count{0};

private:
    ai::AssistantStopReason reason_;
    std::string diagnostic_;
    std::string partial_;
};

/// A host client whose first accepted call terminates with an error outcome
/// while later calls answer normally, so tests can verify the Agent Session
/// remains usable after a terminal outcome.
class RecoveringChatClient final : public ai::StreamingChatClient {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::AssistantMessage>> stream(
        const ai::StreamChatRequest& request,
        ai::AssistantEventSink sink) override {
        ++request_count;
        ai::AssistantMessage message;
        message.provider = "host";
        message.api = "host";
        message.model = request.model;
        if (request_count == 1) {
            message.stop_reason = ai::AssistantStopReason::Error;
            message.error_message = "first transport lost";
            if (sink) {
                if (auto ended = sink(ai::AssistantErrorEvent{message.stop_reason, message});
                    !ended) {
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
};

[[nodiscard]] std::size_t count_occurrences(
    const std::string& text,
    const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

coding_agent::CreateAgentSessionOptions fake_in_memory_options(
    const std::filesystem::path& workspace) {
    coding_agent::CreateAgentSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace;
    options.chat_client = ai::providers::make_scripted_fake_chat_client();
    options.builtin_tools = coding_agent::SdkBuiltinTools{
        .read = false,
        .write = false,
        .edit_file = false,
        .bash = false,
    };
    return options;
}

struct CreatedSession {
    coding_agent::CreateAgentSessionResult created;
};

CreatedSession make_session(const tests::TempWorkspace& workspace) {
    auto created = coding_agent::create_agent_session(fake_in_memory_options(workspace.path()));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

CreatedSession make_session(
    const tests::TempWorkspace& workspace,
    std::unique_ptr<ai::StreamingChatClient> chat_client) {
    auto options = fake_in_memory_options(workspace.path());
    options.chat_client = std::move(chat_client);
    auto created = coding_agent::create_agent_session(std::move(options));
    REQUIRE(created.has_value());
    return CreatedSession{std::move(*created)};
}

[[nodiscard]] std::vector<util::JsonValue::object_t> parse_json_lines(
    const std::string& text) {
    std::vector<util::JsonValue::object_t> records;
    std::istringstream lines{text};
    for (std::string line; std::getline(lines, line);) {
        if (line.empty()) {
            continue;
        }
        auto parsed = util::read_json<util::JsonValue>(line);
        REQUIRE(parsed.has_value());
        const auto* object = parsed->get_if<util::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        records.push_back(*object);
    }
    return records;
}

[[nodiscard]] std::size_t find_record_index(
    const std::vector<util::JsonValue::object_t>& records,
    const std::string& type) {
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto it = records[index].find("type");
        if (it != records[index].end()) {
            if (const auto* value = it->second.get_if<std::string>(); value && *value == type) {
                return index;
            }
        }
    }
    return records.size();
}

[[nodiscard]] std::size_t find_assistant_message_end_index(
    const std::vector<util::JsonValue::object_t>& records) {
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
        const auto* message_object = message->second.get_if<util::JsonValue::object_t>();
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

} // namespace

TEST_CASE(
    "interactive json frontend exits 2 when the session header cannot be written",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    FailingStreambuf failing;
    std::ostream broken_output{&failing};
    std::istringstream input;
    std::ostringstream error;
    cli::JsonCliRenderer renderer{broken_output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = broken_output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 2);
    CHECK(error.str().find("event printer failed: ") != std::string::npos);
}

TEST_CASE(
    "interactive frontend exits 2 when event subscription fails",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);
    REQUIRE(session.created.session->close().has_value());

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 2);
    CHECK(error.str().find("could not subscribe event renderer: ") != std::string::npos);
}

TEST_CASE(
    "interactive text frontend reports a /clear stream failure",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    FailingStreambuf failing;
    std::ostream broken_output{&failing};
    std::istringstream input;
    std::ostringstream error;
    cli::TextCliRenderer renderer{broken_output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = broken_output, .error = error,
            .repl = false, .prompt = "/clear"}};

    CHECK(frontend.run() == 1);
    CHECK(error.str().find("failed to clear terminal") != std::string::npos);
}

TEST_CASE(
    "interactive text frontend renders one prompt through the event subscription",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("fake: hello") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text frontend presents an accepted error terminal outcome once",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_unique<TerminalOutcomeChatClient>(
            ai::AssistantStopReason::Error, "host transport lost"));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("[assistant] partial draft") != std::string::npos);
    CHECK(count_occurrences(output.str(), "[error] host transport lost") == 1);
    CHECK(output.str().find("[completed]") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text frontend presents an accepted aborted terminal outcome once",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_unique<TerminalOutcomeChatClient>(
            ai::AssistantStopReason::Aborted, "host cancelled the request"));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(count_occurrences(output.str(), "[aborted] host cancelled the request") == 1);
    CHECK(output.str().find("[completed]") != std::string::npos);
    CHECK(output.str().find("[error]") == std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text frontend redacts and bounds terminal diagnostics",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-hostsecret123456789";
    std::string diagnostic = "provider rejected " + secret + " detail ";
    diagnostic += std::string(9000, 'x');
    auto session = make_session(
        workspace,
        std::make_unique<TerminalOutcomeChatClient>(
            ai::AssistantStopReason::Error, diagnostic));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find(secret) == std::string::npos);
    CHECK(output.str().find("[REDACTED]") != std::string::npos);
    // The presented diagnostic stays inside the bounded-output budget even
    // though the raw provider diagnostic far exceeds it.
    CHECK(output.str().size() < diagnostic.size());
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text frontend redacts and bounds partial terminal output",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    const std::string secret = "sk-partialsecret123456789";
    std::string partial = "draft contains " + secret + " ";
    partial += std::string(9000, 'x');
    auto session = make_session(
        workspace,
        std::make_unique<TerminalOutcomeChatClient>(
            ai::AssistantStopReason::Error,
            "transport failed",
            partial));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find(secret) == std::string::npos);
    CHECK(output.str().find("[assistant] draft contains [REDACTED]") != std::string::npos);
    CHECK(output.str().size() < partial.size());
    CHECK(count_occurrences(output.str(), "[error] transport failed") == 1);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text REPL stays usable after an accepted terminal outcome",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto client = std::make_unique<RecoveringChatClient>();
    auto* probe = client.get();
    auto session = make_session(workspace, std::move(client));

    std::istringstream input{"first\nsecond\nexit\n"};
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = true, .prompt = {}}};

    CHECK(frontend.run() == 0);
    CHECK(probe->request_count == 2);
    CHECK(count_occurrences(output.str(), "[error] first transport lost") == 1);
    CHECK(output.str().find("[assistant] recovered answer") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive text frontend presents slash command results and shutdown",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input{"/session\n/quit\n"};
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = true, .prompt = {}}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("Session: ") != std::string::npos);
    CHECK(output.str().find("Shutting down.") != std::string::npos);
}

TEST_CASE(
    "interactive text frontend clears the terminal for /clear",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input{"/clear\nexit\n"};
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = true, .prompt = {}}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("\033[2J\033[H") != std::string::npos);
}

TEST_CASE(
    "interactive text frontend reports a prompt failure on the error stream",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    auto rejecting = session.created.session->subscribe(
        [](const agent::AgentLifecycleEvent& event) -> util::ExpectedVoid {
            if (std::holds_alternative<agent::AgentStartEvent>(event)) {
                return std::unexpected(util::make_error(
                    util::ErrorCode::Unknown,
                    "synthetic subscriber failure"));
            }
            return {};
        });
    REQUIRE(rejecting.has_value());

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 1);
    CHECK(error.str().find("loop failed: synthetic subscriber failure") != std::string::npos);
}

TEST_CASE(
    "interactive text frontend repl skips empty lines and user bash",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input{"\n!ls\nexit\n"};
    std::ostringstream output;
    std::ostringstream error;
    cli::TextCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = true, .prompt = {}}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("Shell passthrough (!) is not yet implemented.") != std::string::npos);
    CHECK(output.str().find("> ") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive json frontend emits a session header and event records",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::JsonCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);

    std::istringstream records{output.str()};
    std::string header_line;
    REQUIRE(std::getline(records, header_line));
    const auto header = util::read_json<util::JsonValue>(header_line);
    REQUIRE(header.has_value());
    const auto* header_object = header->get_if<util::JsonValue::object_t>();
    REQUIRE(header_object != nullptr);
    CHECK(header_object->at("type").get<std::string>() == "session");
    CHECK(header_object->at("id").get<std::string>() == session.created.metadata.session_id);
    CHECK(output.str().find("agent_start") != std::string::npos);
    CHECK(output.str().find("fake: hello") != std::string::npos);
    CHECK(error.str().empty());
}

TEST_CASE(
    "interactive json frontend carries the terminal outcome in ordinary lifecycle records",
    "[cli][frontend][issue16]") {
    tests::TempWorkspace workspace;
    auto session = make_session(
        workspace,
        std::make_unique<TerminalOutcomeChatClient>(
            ai::AssistantStopReason::Error, "host transport lost"));

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::JsonCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "hello"}};

    CHECK(frontend.run() == 0);
    CHECK(error.str().empty());

    const auto records = parse_json_lines(output.str());
    REQUIRE(records.size() > 1);
    CHECK(records.front().at("type").get<std::string>() == "session");
    CHECK(find_record_index(records, "runtime_terminal") == records.size());

    const auto message_end_index = find_assistant_message_end_index(records);
    const auto turn_end_index = find_record_index(records, "turn_end");
    const auto agent_end_index = find_record_index(records, "agent_end");
    REQUIRE(message_end_index < records.size());
    REQUIRE(turn_end_index < records.size());
    REQUIRE(agent_end_index < records.size());
    CHECK(message_end_index < turn_end_index);
    CHECK(turn_end_index < agent_end_index);

    const auto& ended_message = records[message_end_index].at("message").get<util::JsonValue::object_t>();
    CHECK(ended_message.at("role").get<std::string>() == "assistant");
    CHECK(ended_message.at("stopReason").get<std::string>() == "error");
    CHECK(ended_message.at("errorMessage").get<std::string>() == "host transport lost");

    const auto& turn_end = records[turn_end_index];
    const auto& turn_message = turn_end.at("message").get<util::JsonValue::object_t>();
    CHECK(turn_message.at("stopReason").get<std::string>() == "error");
    CHECK(turn_message.at("errorMessage").get<std::string>() == "host transport lost");
    CHECK(turn_end.at("toolResults").get<util::JsonValue::array_t>().empty());

    const auto& agent_messages = records[agent_end_index].at("messages").get<util::JsonValue::array_t>();
    REQUIRE_FALSE(agent_messages.empty());
    const auto& final_message = agent_messages.back().get<util::JsonValue::object_t>();
    CHECK(final_message.at("role").get<std::string>() == "assistant");
    CHECK(final_message.at("stopReason").get<std::string>() == "error");
    CHECK(final_message.at("errorMessage").get<std::string>() == "host transport lost");
}

TEST_CASE(
    "interactive json frontend suppresses frontend command output",
    "[cli][frontend]") {
    tests::TempWorkspace workspace;
    auto session = make_session(workspace);

    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    cli::JsonCliRenderer renderer{output, error};
    cli::InteractiveCliFrontend frontend{
        *session.created.session,
        renderer,
        session.created.metadata,
        cli::InteractiveCliFrontendConfig{
            .input = input, .output = output, .error = error,
            .repl = false, .prompt = "/session"}};

    CHECK(frontend.run() == 0);
    CHECK(output.str().find("Session: ") == std::string::npos);
    CHECK(output.str().find("\033[2J\033[H") == std::string::npos);

    // Every emitted line stays a JSON protocol record.
    std::istringstream records{output.str()};
    for (std::string line; std::getline(records, line);) {
        if (line.empty()) {
            continue;
        }
        CHECK(util::read_json<util::JsonValue>(line).has_value());
    }
}
