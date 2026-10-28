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
// a non-http(s) URL is rejected at registration, a 200 with a non-MCP body is
// not success, an HTTP error status surfaces, and an untrusted certificate is
// refused.
//
// The pi-v1.3.0 transport slice (#1103, ADR 0075 supersedes ADR 0054's
// TLS-only/no-redirect rules for MCP) drives the frozen audit-1052
// `transports.json` observations through the public client over loopback
// fixtures: plain HTTP connects, a same-origin 307 keeps the request
// Authorization, a cross-origin 307 is followed without it (the target's 401
// surfaces as the re-login error), and the fetch redirect ceiling and
// non-HTTP(S) Location rule fail explicitly. Each new case names the
// registered observation or frozen undici rule it compares.

#include "ai/ModelStreamBridge.hpp"
#include "support/BoostBeastStreamTransport.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/mcp/McpExtensionToolSource.hpp"
#include "coding_agent/mcp/McpHttpServerConfig.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/McpTestKit.hpp"

#include <cch/ai/Content.hpp>
#include <cch/mcp/HttpClient.hpp>
#include <cch/mcp/HttpClient.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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

using namespace std::chrono_literals;

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

/// Spawn options for one real MCP fixture server: TLS under the committed test
/// PKI or plain HTTP (both frozen-accepted schemes), an optional request
/// trace, and extra child environment for the redirect/authorization switches.
struct HttpFixtureOptions {
    std::filesystem::path trace_path{};
    bool tls{true};
    std::vector<std::pair<std::string, std::string>> env{};
};

/// One real HTTP(S) MCP server on an ephemeral loopback port. The child prints
/// `PORT=<n>` on stdout once it is listening; the destructor terminates and
/// reaps it on every scope-exit path, so no server survives a test.
class HttpFixtureServer final {
public:
    explicit HttpFixtureServer(HttpFixtureOptions options = {}) {
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
            if (!options.trace_path.empty()) {
                (void)::setenv("PIKE_MCP_TRACE", options.trace_path.string().c_str(), 1);
            }
            for (const auto& [name, value] : options.env) {
                (void)::setenv(name.c_str(), value.c_str(), 1);
            }
            const std::string script = fixture_path("http_server.py");
            if (options.tls) {
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
            } else {
                ::execlp("python3", "python3", script.c_str(), static_cast<char*>(nullptr));
            }
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
        url_ = (options.tls ? "https://127.0.0.1:" : "http://127.0.0.1:") + std::to_string(port) + "/mcp";
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

/// The `scheme://authority` of an absolute fixture URL, for building
/// redirect entrypoints on the same origin.
[[nodiscard]] std::string url_origin(std::string_view url) {
    const auto scheme_end = url.find("://");
    REQUIRE(scheme_end != std::string_view::npos);
    const auto path_start = url.find('/', scheme_end + 3);
    return std::string{url.substr(0, path_start)};
}

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

[[nodiscard]] std::shared_ptr<cch::mcp::HttpClient> connect_http_client(
        tests::RuntimeFixture& runtime, coding_agent::mcp::McpHttpServerConfig config) {
    auto client = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config), std::make_shared<support::BoostBeastStreamTransport>()));
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

// The shared fixture-trace and tool-call helpers live in `tests/support`
// (§11.5), so the stdio and HTTP session tests cannot drift apart.
using cch::tests::first_text_content;
using cch::tests::tools_call_params;
using cch::tests::trace_contains;
using cch::tests::wait_for;

/// Start one `tools/call` for `tool_name` over HTTP, wait until the fixture
/// server has it in flight (its trace line), request cancellation through the
/// caller's stop token, and return the call's terminal outcome. Waiting for the
/// server's `hang-start` line guarantees the cancellation hits a running call
/// rather than a queued one.
[[nodiscard]] boost::asio::awaitable<support::Expected<support::JsonValue>> cancel_in_flight_http_call(
        std::shared_ptr<cch::mcp::HttpClient> client,
        std::filesystem::path trace,
        std::stop_source& cancel,
        std::string tool_name) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto first = std::make_shared<std::optional<support::Expected<support::JsonValue>>>();
    boost::asio::co_spawn(
            executor,
            [client,
                    first,
                    token = cancel.get_token(),
                    params = tools_call_params(std::move(tool_name),
                            support::JsonValue::object_t{})]() mutable -> boost::asio::awaitable<void> {
                first->emplace(co_await support::detail::await_async_result(
                        client->request("tools/call", std::move(params), {.stop_token = token})));
                co_return;
            },
            boost::asio::detached);
    co_await wait_for(executor, [&] { return trace_contains(trace, "hang-start"); }, 10s);
    cancel.request_stop();
    co_await wait_for(executor, [&] { return first->has_value(); }, 10s);
    if (!first->has_value()) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Timeout, "the cancelled HTTP call never completed"));
    }
    co_return std::move(**first);
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
    REQUIRE(tools.size() == 8);
    CHECK(tools[0].server_tool_name == "echo");
    CHECK(tools[0].full_name == "mcp__echo__echo");
    CHECK(tools[0].description == "Echo the incoming text back.");
    REQUIRE(tools[0].parameters.get_if<support::JsonValue::object_t>() != nullptr);
    CHECK(tools[1].full_name == "mcp__echo__fail");
    CHECK(tools[2].full_name == "mcp__echo__sse_echo");
    CHECK(tools[3].full_name == "mcp__echo__hang");
    CHECK(tools[4].full_name == "mcp__echo__slow");
    CHECK(tools[5].full_name == "mcp__echo__crash");
    CHECK(tools[6].full_name == "mcp__echo__structured");
    CHECK(tools[7].full_name == "mcp__echo__sse_structured");
}

TEST_CASE("an MCP HTTP tools/call preserves actual wire structuredContent order",
        "[coding_agent][mcp][issue873][issue1101][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto source =
            tests::run_awaitable(runtime, coding_agent::mcp::McpExtensionToolSource::connect_http(http_config(server)));
    REQUIRE(source.has_value());
    auto tools = source.value()->load_tools();
    REQUIRE(tools.has_value());
    const auto structured = std::find_if(tools->begin(), tools->end(), [](const auto& tool) {
        return tool.definition.name == "mcp__echo__structured";
    });
    REQUIRE(structured != tools->end());

    auto executed = runtime.run(structured->context_execute(support::JsonValue{support::JsonValue::object_t{}},
            coding_agent::extensions::ExtensionToolContext{},
            std::stop_token{}));
    REQUIRE(executed.has_value());
    REQUIRE(executed->content.size() == 1);
    const auto* text = std::get_if<ai::TextContent>(&executed->content.front());
    REQUIRE(text != nullptr);
    CHECK(text->text == "{\n  \"ok\": true,\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ]\n}");
    CHECK_FALSE(executed->is_error);
    REQUIRE(executed->details.has_value());
    const auto* details = executed->details->get_if<support::JsonValue::object_t>();
    REQUIRE(details != nullptr);
    const auto block = details->find("content");
    REQUIRE(block != details->end());
    CHECK(block->second.get_array().empty());
    const auto value = details->find("structuredContent");
    REQUIRE(value != details->end());
    CHECK(value->second.at("ok").get_boolean());
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

TEST_CASE("an MCP HTTP URL must be http or https", "[coding_agent][mcp][issue873][issue1103][spec]") {
    // pi v1.3.0 `validateMcpServerConfig` (audit-1052 `transports.json`,
    // "application config accepts http and https but rejects another scheme"):
    // both `http` and `https` URLs are accepted — the plain-HTTP connect case
    // below is the acceptance half — and any other scheme is rejected at
    // registration with pi's message. This case replaces the former TLS-only
    // refusal (ADR 0054, superseded for MCP by ADR 0075), which asserted an
    // `http://` URL was rejected here; the frozen behavior accepts it.
    coding_agent::mcp::McpHttpServerConfig rejected;
    rejected.name = "rejected";
    rejected.url = "ftp://127.0.0.1/mcp";

    auto valid = cch::mcp::validate_http_server_config(rejected);
    REQUIRE_FALSE(valid.has_value());
    CHECK(valid.error().code == support::ErrorCode::Validation);
    CHECK(valid.error().message.find("url must be an http or https URL") != std::string::npos);

    tests::RuntimeFixture runtime;
    // The gate runs before any socket work, so no server is needed.
    auto refused = tests::run_awaitable(
            runtime, cch::mcp::HttpClient::connect(rejected, std::make_shared<support::BoostBeastStreamTransport>()));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::Validation);

    // Session Assembly fails explicitly rather than dropping the configured
    // server.
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.mcp_http_servers = {rejected};
    auto models = tests::models_from_provider(std::make_shared<McpToolRoundProvider>(
            "mcp__echo__echo", support::JsonValue{support::JsonValue::object_t{}}));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == support::ErrorCode::Validation);
    CHECK(created.error().message.find("rejected") != std::string::npos);
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

TEST_CASE("streamable HTTP transport connects over plain http", "[coding_agent][mcp][issue1103][spec]") {
    // audit-1052 `transports.json`, same-named scenario: the frozen client
    // connects over `http`, the initialize POST is answered 200, and the
    // `notifications/initialized` POST is answered 202.
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace, .tls = false}};
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    CHECK(client->server_name() == "echo");

    const auto listed =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("tools/list")));
    REQUIRE(listed.has_value());

    // The connect succeeded, which requires the initialize 200; both requests
    // arrived at the plain-HTTP fixture in pi's order.
    CHECK(tests::trace_contains(trace, "recv initialize id=1"));
    CHECK(tests::trace_contains(trace, "recv notifications/initialized id=None"));
    client->close();
}

TEST_CASE("HTTPS transport rejects an untrusted certificate", "[coding_agent][mcp][issue1103][spec]") {
    // audit-1052 `transports.json`, same-named scenario: with the default
    // trust store the self-signed loopback certificate is refused and
    // certificate validation stays platform-enforced (ADR 0075). The trusted
    // half is the acceptance pair: every TLS fixture case connects only with
    // SSL_CERT_FILE pointing at the committed test CA (`trust_test_ca`).
    tests::TempWorkspace workspace;
    const auto empty_ca = workspace.path() / "empty-ca.pem";
    {
        std::ofstream create(empty_ca);
    }
    const tests::EnvVarGuard untrusted{"SSL_CERT_FILE", empty_ca.string()};
    HttpFixtureServer server;
    tests::RuntimeFixture runtime;

    auto refused = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(http_config(server), std::make_shared<support::BoostBeastStreamTransport>()));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::Network);
}

TEST_CASE("a same-origin redirect is followed and keeps the request auth header",
        "[coding_agent][mcp][issue1103][spec]") {
    // audit-1052 `transports.json`, same-named scenario: the 307 is followed,
    // the POST is re-sent to the resolved target, and `Authorization: Bearer static` is
    // preserved. This case replaces the former refusal of every redirect
    // (ADR 0054, superseded for MCP by ADR 0075).
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace,
            .tls = false,
            .env = {{"PIKE_MCP_REDIRECT_PATH", "/a/b"}, {"PIKE_MCP_REDIRECT_TARGET", "next"}}}};
    tests::RuntimeFixture runtime;

    auto config = http_config(server);
    config.url = url_origin(server.url()) + "/a/b";
    config.headers["Authorization"] = "Bearer static";
    auto client = connect_http_client(runtime, std::move(config));

    // The initialize followed the hop: the connection is live and the target
    // saw the credential.
    CHECK(tests::trace_contains(trace, "redirect /a/b -> next"));
    CHECK(tests::trace_contains(trace, "request-path /a/next"));
    CHECK(tests::trace_contains(trace, "recv initialize id=1 authorization_present=true"));

    // The credential arrived at the same-origin target, verbatim.
    auto observed = tests::run_awaitable(
            runtime, support::detail::await_async_result(client->request("debug/authorization_present")));
    REQUIRE(observed.has_value());
    CHECK(observed->at("authorization").get_string() == "Bearer static");
    client->close();
}

TEST_CASE("relative redirect references resolve against the current URL", "[coding_agent][mcp][issue1103][spec]") {
    struct RedirectCase {
        const char* from;
        const char* location;
        const char* expected;
    };
    const RedirectCase cases[] = {
            {"/a/b", "/abs", "/abs"},
            {"/a/b/c", "../up", "/a/up"},
            {"/a/b", "?q=1", "/a/b?q=1"},
    };
    for (const auto& example : cases) {
        INFO(example.location);
        tests::TempWorkspace workspace;
        const auto trace = workspace.path() / "trace.log";
        HttpFixtureServer server{{.trace_path = trace,
                .tls = false,
                .env = {{"PIKE_MCP_REDIRECT_PATH", example.from}, {"PIKE_MCP_REDIRECT_TARGET", example.location}}}};
        tests::RuntimeFixture runtime;
        auto config = http_config(server);
        config.url = url_origin(server.url()) + example.from;
        auto client = connect_http_client(runtime, std::move(config));
        CHECK(tests::trace_contains(trace, std::string{"request-path "} + example.expected));
        client->close();
    }
}

TEST_CASE("cross-origin redirect is followed without Authorization and is rejected",
        "[coding_agent][mcp][issue1103][spec]") {
    // audit-1052 `transports.json`, same-named scenario: the source 307 keeps
    // the caller's Authorization, the cross-origin hop strips it, the target
    // answers 401, and the client surfaces the rejection as the re-login
    // error (pi `McpAuthRequiredError`, "MCP server requires authentication").
    tests::TempWorkspace workspace;
    const auto target_trace = workspace.path() / "target-trace.log";
    // The frozen target answers 401 when the stripped request arrives; a
    // client that leaked the credential would instead complete initialize.
    HttpFixtureServer target{
            {.trace_path = target_trace, .tls = false, .env = {{"PIKE_MCP_REQUIRE_AUTHORIZATION", "1"}}}};
    const std::string scheme_relative = target.url().substr(std::string{"http:"}.size());
    HttpFixtureServer source{{.tls = false, .env = {{"PIKE_MCP_REDIRECT_TARGET", scheme_relative}}}};
    tests::RuntimeFixture runtime;

    auto config = http_config(source);
    config.url = url_origin(source.url()) + "/redirect-cross";
    config.headers["Authorization"] = "Bearer static";
    auto refused = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config), std::make_shared<support::BoostBeastStreamTransport>()));

    // The target's trace is the wire proof that no Authorization crossed the
    // origin: exactly the initialize POST arrived, with the header absent.
    CHECK(tests::trace_line_count(target_trace, "recv initialize") == 1);
    CHECK(tests::trace_contains(target_trace, "recv initialize id=1 authorization_present=false"));

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::OAuth);
    CHECK(refused.error().message.find("Authentication failed") != std::string::npos);
    CHECK(refused.error().detail.find("HTTP 401") != std::string::npos);
}

TEST_CASE("a redirect chain beyond the fetch ceiling fails explicitly", "[coding_agent][mcp][issue1103][protocol]") {
    // Frozen undici `httpRedirectFetch`: `request.redirectCount === 20` is a
    // network error "redirect count exceeded". A client without the ceiling
    // would hang or loop forever on /redirect-loop.
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace, .tls = false}};
    tests::RuntimeFixture runtime;

    auto config = http_config(server);
    config.url = url_origin(server.url()) + "/redirect-loop";
    auto refused = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config), std::make_shared<support::BoostBeastStreamTransport>()));

    REQUIRE_FALSE(refused.has_value());
    // Deterministic fetch-policy failure: `Process` (not the retried
    // `Network`) — pi rejects the fetch without a connect retry.
    CHECK(refused.error().code == support::ErrorCode::Process);
    CHECK(refused.error().message.find("redirect count exceeded") != std::string::npos);
    // Twenty hops are allowed, the 21st redirect answers the ceiling: the
    // fixture saw the initial request plus the twenty re-issues.
    CHECK(tests::trace_line_count(trace, "redirect /redirect-loop -> /redirect-loop") == 21);
}

TEST_CASE("a redirect to a non-HTTP(S) Location is a network error", "[coding_agent][mcp][issue1103][protocol]") {
    // Frozen undici `httpRedirectFetch` step 6: a Location whose scheme is not
    // HTTP(S) fails the fetch ("URL scheme must be a HTTP(S) scheme") rather
    // than being followed or silently surfaced as a status.
    HttpFixtureServer server{{.tls = false, .env = {{"PIKE_MCP_REDIRECT_LOCATION", "ftp://127.0.0.1/mcp"}}}};
    tests::RuntimeFixture runtime;

    auto config = http_config(server);
    config.url = url_origin(server.url()) + "/redirect-scheme";
    auto refused = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config), std::make_shared<support::BoostBeastStreamTransport>()));

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::Process);
    CHECK(refused.error().message.find("URL scheme must be a HTTP(S) scheme") != std::string::npos);
}

TEST_CASE("a redirect without a Location surfaces the status explicitly", "[coding_agent][mcp][issue1103][protocol]") {
    // Frozen undici `responseLocationURL`: a redirect status without a
    // Location is returned to the caller untouched, so the MCP layer reports
    // the status instead of following anything.
    HttpFixtureServer server{{.tls = false}};
    tests::RuntimeFixture runtime;

    auto config = http_config(server);
    config.url = url_origin(server.url()) + "/redirect-no-location";
    auto refused = tests::run_awaitable(runtime,
            cch::mcp::HttpClient::connect(std::move(config), std::make_shared<support::BoostBeastStreamTransport>()));

    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == support::ErrorCode::Process);
    CHECK(refused.error().message.find("302") != std::string::npos);
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

TEST_CASE("cancelling an in-flight MCP HTTP call surfaces the cancellation error and keeps the connection usable",
        "[coding_agent][mcp][issue872][spec]") {
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace}};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    std::stop_source cancel;
    const auto cancelled = tests::run_awaitable(runtime, cancel_in_flight_http_call(client, trace, cancel, "hang"));

    // The in-flight call failed with the cancellation error the transport
    // produced, not a timeout and not a silent hang.
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);

    // The connection stays usable: a subsequent call completes normally.
    const auto after = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request(
                    "tools/call", tools_call_params("echo", support::JsonValue::object_t{{"text", "after cancel"}}))));
    REQUIRE(after.has_value());
    CHECK(first_text_content(*after) == std::optional<std::string>{"after cancel"});
}

TEST_CASE("cancelling an in-flight MCP HTTP call tells the server with notifications/cancelled",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace}};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    std::stop_source cancel;
    const auto cancelled = tests::run_awaitable(runtime, cancel_in_flight_http_call(client, trace, cancel, "hang"));
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);

    // pi `cancelPending` sends `notifications/cancelled` for a non-`initialize`
    // request with the aborted request's id and the pi reason. The fixture
    // server traces the received notification, so this is the wire diff.
    CHECK(trace_contains(trace, "notifications/cancelled"));
    CHECK(trace_contains(trace, "\"reason\":\"Aborted\""));
    CHECK(trace_contains(trace, "\"requestId\":2"));
}

TEST_CASE("a timed-out MCP HTTP call tells the server with the pi timeout reason",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace}};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto config = http_config(server);
    config.request_timeout = 300ms;
    auto client = connect_http_client(runtime, std::move(config));

    const auto timed_out = tests::run_awaitable(runtime,
            support::detail::await_async_result(
                    client->request("tools/call", tools_call_params("hang", support::JsonValue::object_t{}))));
    REQUIRE_FALSE(timed_out.has_value());
    CHECK(timed_out.error().code == support::ErrorCode::Timeout);

    // pi `armTimeout` cancels with reason `Request timed out`; the server sees
    // that reason, not the generic abort one.
    CHECK(trace_contains(trace, "notifications/cancelled"));
    CHECK(trace_contains(trace, "\"reason\":\"Request timed out\""));
}

TEST_CASE("a failed MCP HTTP call does not emit notifications/cancelled", "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace}};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    // A server-side failure is not a cancellation or a timeout, so pi
    // `cancelPending` is never reached and no notification is sent. This is
    // the separation case for "emits on cancel": a client that notified on
    // every failure would satisfy the cancel checks while violating the rule.
    const auto failed =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("debug/http_status")));
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message.find("500") != std::string::npos);
    CHECK_FALSE(trace_contains(trace, "notifications/cancelled"));
}

TEST_CASE("a request already cancelled before it is sent does not emit notifications/cancelled",
        "[coding_agent][mcp][issue884][spec]") {
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "trace.log";
    HttpFixtureServer server{{.trace_path = trace}};
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    std::stop_source cancel;
    cancel.request_stop();
    // pi throws `McpAbortError` before the request is registered when the
    // signal is already aborted, so the server is told nothing.
    const auto cancelled = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request("tools/call",
                    tools_call_params("echo", support::JsonValue::object_t{{"text", "never"}}),
                    {.stop_token = cancel.get_token()})));
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);
    CHECK_FALSE(trace_contains(trace, "notifications/cancelled"));
}

TEST_CASE("an MCP HTTP call errors explicitly after the server dies between tools/list and tools/call",
        "[coding_agent][mcp][issue872][spec]") {
    HttpFixtureServer server;
    auto ca = trust_test_ca();
    tests::RuntimeFixture runtime;

    auto client = connect_http_client(runtime, http_config(server));
    const auto listed =
            tests::run_awaitable(runtime, support::detail::await_async_result(client->request("tools/list")));
    REQUIRE(listed.has_value());

    // `tools/call crash` exits the server without responding; that call and
    // the next one both fail explicitly rather than hanging.
    const auto crash = tests::run_awaitable(runtime,
            support::detail::await_async_result(
                    client->request("tools/call", tools_call_params("crash", support::JsonValue::object_t{}))));
    REQUIRE_FALSE(crash.has_value());

    const auto call = tests::run_awaitable(runtime,
            support::detail::await_async_result(client->request(
                    "tools/call", tools_call_params("echo", support::JsonValue::object_t{{"text", "too late"}}))));
    REQUIRE_FALSE(call.has_value());
}
