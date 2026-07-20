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
