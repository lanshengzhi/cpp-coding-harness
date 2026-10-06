// Spec #865 third slice (#873): MCP server over the streamable HTTP transport.
// The fixture is a real TLS MCP server (`fixtures/pi-mcp/http_server.py`)
// listening on the loopback interface under the committed test PKI
// (`tests/ai/providers/tls/`), driven by the product transport over the wire
// and converted into Agent tools through the #867 Extension Tool Source seam.
// The scripted fake provider serves the model, so no live keys or network are
// used.
//
// The acceptance case pairs "the server is registered and its tools are on the
// session's surface" with "a session assembled without the server does not
// expose them": a tool's existence is not what makes it visible, registration
// at assembly is. The separation cases pin the transport's explicit failures:
// a non-TLS URL is rejected at registration, a 200 with a non-MCP body is not
// success, a non-TLS redirect is refused, and an HTTP error status surfaces.

#include "ai/ModelStreamBridge.hpp"
#include "ai/providers/BoostBeastStreamTransport.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpHttpClient.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;

namespace {

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

// Committed test credentials (issue #638): the client trusts `test-ca.pem`
// through the `SSL_CERT_FILE` process environment, the mock server is secured
// by `test-server.pem`/`test-server-key.pem` (SAN: IP 127.0.0.1). Regeneration
// commands: tests/ai/providers/tls/README.md.
[[nodiscard]] std::string tls_fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/tests/ai/providers/tls/" + std::string{name};
}

/// One real TLS MCP server on an ephemeral loopback port. The child prints
/// `PORT=<n>` on stdout once it is listening; the destructor terminates and
/// reaps it on every scope-exit path, so no server survives a test.
class HttpFixtureServer final {
public:
    HttpFixtureServer() {
        int fds[2]{-1, -1};
        REQUIRE(::pipe(fds) == 0);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            (void)::close(fds[0]);
            (void)::dup2(fds[1], STDOUT_FILENO);
            (void)::close(fds[1]);
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)::dup2(devnull, STDERR_FILENO);
            }
            const std::string script = fixture_path("http_server.py");
            const std::string cert = tls_fixture_path("test-server.pem");
            const std::string key = tls_fixture_path("test-server-key.pem");
            ::execlp("python3",
                    "python3",
                    script.c_str(),
                    "--cert",
                    cert.c_str(),
                    "--key",
                    key.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        pid_ = child;
        (void)::close(fds[1]);
        std::string line;
        char character = '\0';
        while (::read(fds[0], &character, 1) == 1) {
            if (character == '\n') {
                break;
            }
            line.push_back(character);
        }
        (void)::close(fds[0]);
        REQUIRE(line.starts_with("PORT="));
        const std::string_view digits{line.data() + 5, line.size() - 5};
        int port = 0;
        REQUIRE(std::from_chars(digits.data(), digits.data() + digits.size(), port).ec == std::errc{});
        url_ = "https://127.0.0.1:" + std::to_string(port) + "/mcp";
    }

    HttpFixtureServer(const HttpFixtureServer&) = delete;
    HttpFixtureServer& operator=(const HttpFixtureServer&) = delete;

    ~HttpFixtureServer() {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGTERM);
            int status = 0;
            (void)::waitpid(pid_, &status, 0);
        }
    }

    [[nodiscard]] const std::string& url() const noexcept { return url_; }

private:
    pid_t pid_{-1};
    std::string url_;
};

/// Trust the committed test CA for the duration of a case so the reused HTTPS
/// transport accepts the loopback server's certificate.
[[nodiscard]] tests::EnvVarGuard trust_test_ca() {
    return tests::EnvVarGuard{"SSL_CERT_FILE", tls_fixture_path("test-ca.pem")};
}

[[nodiscard]] coding_agent::mcp::McpHttpServerConfig http_config(const HttpFixtureServer& server) {
    coding_agent::mcp::McpHttpServerConfig config;
    config.name = "echo";
    config.url = server.url();
    return config;
}

[[nodiscard]] std::shared_ptr<coding_agent::mcp::McpHttpClient> connect_http_client(
        tests::RuntimeFixture& runtime, coding_agent::mcp::McpHttpServerConfig config) {
    auto client = tests::run_awaitable(runtime,
            coding_agent::mcp::McpHttpClient::connect(
                    std::move(config), std::make_shared<ai::providers::BoostBeastStreamTransport>()));
    REQUIRE(client.has_value());
    return *client;
}

/// Scripted provider that calls one named tool (with fixed arguments) on its
/// first request and answers a plain text turn afterwards, so one prompt
/// exercises the MCP discover -> call -> result round through the ordinary
/// executor path.
class McpToolRoundProvider final : public tests::ScriptedProvider {
public:
    McpToolRoundProvider(std::string tool_name, support::JsonValue arguments)
        : ScriptedProvider("fake"), tool_name_(std::move(tool_name)), arguments_(std::move(arguments)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        const std::string tool_name = tool_name_;
        const support::JsonValue arguments = arguments_;
        auto serialized = support::write_json(arguments);
        const std::string raw_arguments = serialized ? std::move(*serialized) : std::string{"{}"};
        return ai::detail::make_model_stream(
                [model = std::move(model), request, tool_name, arguments, raw_arguments](
                        ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "mcp-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling the MCP tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_mcp",
                                .name = tool_name,
                                .arguments = arguments,
                                .raw_arguments = raw_arguments,
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else {
                        round.content = {ai::text_content("done")};
                        round.stop_reason = ai::AssistantStopReason::Stop;
                    }
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return round;
                });
    }

private:
    std::string tool_name_;
    support::JsonValue arguments_;
    int request_count_{0};
};

[[nodiscard]] bool session_exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::optional<std::string> tool_result_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return ai::text_from_content(result->content);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool tool_result_is_error(const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return result->is_error;
        }
    }
    return false;
}

/// Assemble one in-memory session with the given MCP HTTP servers configured
/// and no other tool contributions.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_http_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<coding_agent::mcp::McpHttpServerConfig> servers) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_http_servers = std::move(servers);
    auto models = tests::models_from_provider(std::move(provider));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

} // namespace

TEST_CASE("connecting an MCP HTTP server lists its tools with the pi naming and schema",
        "[coding_agent][mcp][issue873][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto source =
            tests::run_awaitable(runtime, coding_agent::mcp::McpExtensionToolSource::connect_http(http_config(server)));
    REQUIRE(source.has_value());
    REQUIRE(source.value() != nullptr);
    CHECK(source.value()->server_name() == "echo");

    const auto& tools = source.value()->tools();
    REQUIRE(tools.size() == 3);
    CHECK(tools[0].server_tool_name == "echo");
    CHECK(tools[0].full_name == "mcp__echo__echo");
    CHECK(tools[0].description == "Echo the incoming text back.");
    REQUIRE(tools[0].parameters.get_if<support::JsonValue::object_t>() != nullptr);
    CHECK(tools[1].full_name == "mcp__echo__fail");
    CHECK(tools[2].full_name == "mcp__echo__sse_echo");
}

TEST_CASE("an MCP HTTP server tool is discoverable and callable in the Agent Session",
        "[coding_agent][mcp][issue873][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    // The fixture rejects a request that omits the `Mcp-Session-Id` it issued
    // at `initialize`, so a successful call also proves the client captured
    // and echoed the session id.
    support::JsonValue arguments{support::JsonValue::object_t{{"text", "hello"}}};
    auto session = make_http_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments),
            {http_config(server)});

    CHECK(session_exposes_tool(*session, "mcp__echo__echo"));

    REQUIRE(tests::run_awaitable(runtime, session->prompt("use the MCP echo tool")).has_value());
    CHECK(tool_result_text(*session, "mcp__echo__echo") == std::optional<std::string>{"hello"});
    CHECK_FALSE(tool_result_is_error(*session, "mcp__echo__echo"));

    session->close();
}

TEST_CASE("an MCP HTTP tool answered over an SSE stream is called like any other",
        "[coding_agent][mcp][issue873][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{{"text", "streamed"}}};
    auto session = make_http_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__sse_echo", arguments),
            {http_config(server)});

    REQUIRE(tests::run_awaitable(runtime, session->prompt("use the SSE MCP tool")).has_value());
    CHECK(tool_result_text(*session, "mcp__echo__sse_echo") == std::optional<std::string>{"sse:streamed"});
    CHECK_FALSE(tool_result_is_error(*session, "mcp__echo__sse_echo"));

    session->close();
}

TEST_CASE("a non-TLS MCP HTTP URL is rejected at registration", "[coding_agent][mcp][issue873][spec]") {
    coding_agent::mcp::McpHttpServerConfig insecure;
    insecure.name = "insecure";
    insecure.url = "http://127.0.0.1:9/mcp";

    auto valid = coding_agent::mcp::validate_mcp_http_server_config(insecure);
    REQUIRE_FALSE(valid.has_value());
    CHECK(valid.error().code == support::ErrorCode::Validation);
    CHECK(valid.error().message.find("https") != std::string::npos);

    tests::RuntimeFixture runtime;
    // The gate runs before any socket work, so no server is needed.
    auto refused = tests::run_awaitable(runtime,
            coding_agent::mcp::McpHttpClient::connect(
                    insecure, std::make_shared<ai::providers::BoostBeastStreamTransport>()));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::Validation);

    // Session Assembly fails explicitly rather than dropping the configured
    // server or falling back to plaintext.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_http_servers = {insecure};
    auto models = tests::models_from_provider(std::make_shared<McpToolRoundProvider>(
            "mcp__echo__echo", support::JsonValue{support::JsonValue::object_t{}}));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == support::ErrorCode::Validation);
    CHECK(created.error().message.find("insecure") != std::string::npos);
}

TEST_CASE("an HTTP 200 with a non-MCP body is not success", "[coding_agent][mcp][issue873][protocol]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    auto response =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/non_mcp_body")));

    REQUIRE_FALSE(response.has_value());
    // The body parsed as JSON but carried no JSON-RPC reply, so the client
    // rejects it explicitly instead of reporting an empty success.
    CHECK(response.error().detail.find("no matching id") != std::string::npos);
}

TEST_CASE("an HTTP redirect to a non-TLS target is refused", "[coding_agent][mcp][issue873][protocol]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    auto response =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/redirect")));

    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().message.find("redirect") != std::string::npos);
    CHECK(response.error().detail.find("never follows a redirect") != std::string::npos);
    CHECK(response.error().detail.find("http://127.0.0.1:1/mcp") != std::string::npos);
}

TEST_CASE("an MCP HTTP server error status surfaces as an explicit error", "[coding_agent][mcp][issue873][protocol]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    auto response =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/http_status")));

    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().message.find("500") != std::string::npos);
    CHECK(response.error().detail.find("boom") != std::string::npos);
}

TEST_CASE("an MCP HTTP JSON-RPC error is reported as the tool error", "[coding_agent][mcp][issue873][protocol]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    auto response =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/json_rpc_error")));

    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().message == "server exploded");
}

TEST_CASE("an MCP HTTP tool is visible to the session only when its server is registered at assembly",
        "[coding_agent][mcp][issue873][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    support::JsonValue arguments{support::JsonValue::object_t{{"text", "hello"}}};

    // A session with no HTTP MCP server configured does not expose the tool,
    // even though the fixture server exists and offers it.
    auto without = make_http_session(
            runtime, workspace, std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments), {});
    CHECK_FALSE(session_exposes_tool(*without, "mcp__echo__echo"));
    without->close();

    // A session assembled with the server exposes it. Together the two cases
    // separate registration-at-assembly (the property) from the server's
    // existence.
    auto with = make_http_session(runtime,
            workspace,
            std::make_shared<McpToolRoundProvider>("mcp__echo__echo", arguments),
            {http_config(server)});
    CHECK(session_exposes_tool(*with, "mcp__echo__echo"));
    with->close();
}
