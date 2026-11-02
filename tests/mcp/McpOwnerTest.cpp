// Issue #1065: public-caller acceptance for the cch_mcp owner. This shard links
// only cch_mcp, so it also proves the extracted package is usable without the
// application Session, extension-tool source, configuration store, or TUI.
//
// The wire behavior is unchanged: the stdio case drives the real
// `fixtures/pi-mcp/echo_server.py` child; the HTTP case drives the real TLS
// `fixtures/pi-mcp/http_server.py`; the cancellation case stops a request the
// fixture deliberately holds open. Each case runs initialize (inside connect),
// tools/list, tools/call, and close over the public headers only.

#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/JsonCompare.hpp"
#include "support/McpTestKit.hpp"
#include "support/TempWorkspace.hpp"
#include "support/UniqueFd.hpp"

#include <cch/mcp/HttpClient.hpp>
#include <cch/mcp/HttpServerConfig.hpp>
#include <cch/mcp/StdioClient.hpp>
#include <cch/mcp/StdioServerConfig.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <expected>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace cch;

namespace {

using namespace std::chrono_literals;

template <typename T, typename E>
[[nodiscard]] std::expected<T, E> run(boost::asio::io_context& io, support::AsyncResult<T, E> operation) {
    std::optional<std::expected<T, E>> outcome;
    boost::asio::co_spawn(
            io,
            [operation = std::move(operation), &outcome]() mutable -> boost::asio::awaitable<void> {
                outcome.emplace(co_await support::detail::await_async_result(std::move(operation)));
            },
            boost::asio::detached);
    // A stdio client keeps a long-lived stderr drain on the loop, so drain the
    // loop in bounded slices until this one operation completes instead of
    // waiting for the loop to run out of work.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!outcome.has_value() && std::chrono::steady_clock::now() < deadline) {
        io.restart();
        io.run_for(5ms);
    }
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
}

[[nodiscard]] std::string fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/fixtures/pi-mcp/" + std::string{name};
}

[[nodiscard]] std::string tls_fixture_path(std::string_view name) {
    return std::string{CCH_SOURCE_DIR} + "/tests/ai/providers/tls/" + std::string{name};
}

[[nodiscard]] mcp::StdioServerConfig echo_server_config() {
    mcp::StdioServerConfig config;
    config.name = "echo";
    config.command = "python3";
    config.args = {fixture_path("echo_server.py")};
    return config;
}

[[nodiscard]] std::shared_ptr<mcp::StdioClient> connect_stdio(boost::asio::io_context& io) {
    auto client = run(io, mcp::StdioClient::connect(echo_server_config()));
    REQUIRE(client.has_value());
    return *client;
}

[[nodiscard]] mcp::StdioServerConfig structured_server_config() {
    mcp::StdioServerConfig config;
    config.name = "structured";
    config.command = "python3";
    config.args = {fixture_path("structured_content_server.py")};
    return config;
}

[[nodiscard]] std::shared_ptr<mcp::StdioClient> connect_structured_stdio(boost::asio::io_context& io) {
    auto client = run(io, mcp::StdioClient::connect(structured_server_config()));
    REQUIRE(client.has_value());
    return *client;
}

[[nodiscard]] support::JsonValue call_structured(boost::asio::io_context& io, mcp::Client& client, std::string kind) {
    auto response = run(io,
            client.request("tools/call",
                    tests::tools_call_params("structured", support::JsonValue::object_t{{"kind", std::move(kind)}})));
    REQUIRE(response.has_value());
    return std::move(*response);
}

[[nodiscard]] mcp::ToolCallResult call_structured_tool(
        boost::asio::io_context& io, mcp::Client& client, std::string kind) {
    auto response = run(io, client.call_tool("structured", support::JsonValue::object_t{{"kind", std::move(kind)}}));
    REQUIRE(response.has_value());
    return std::move(*response);
}

/// Collect every page of `tools/list`, proving the owner's cursor loop rather
/// than a single-page happy path.
[[nodiscard]] std::vector<std::string> list_tool_names(boost::asio::io_context& io, mcp::Client& client) {
    std::vector<std::string> names;
    std::optional<support::JsonValue> cursor;
    while (true) {
        support::JsonValue params = support::JsonValue::object_t{};
        if (cursor) {
            const auto& value = *cursor;
            if (value.holds<std::string>()) {
                params.get_object().emplace("cursor", value.get_string());
            }
        }
        auto response = run(io, client.request("tools/list", std::move(params)));
        REQUIRE(response.has_value());
        const auto& tools = response->at("tools").get_array();
        for (const auto& tool : tools) {
            names.push_back(tool.at("name").get_string());
        }
        const auto next = response->get_object().find("nextCursor");
        if (next == response->get_object().end() || next->second.holds<std::nullptr_t>()) {
            break;
        }
        cursor = next->second;
    }
    return names;
}

[[nodiscard]] support::JsonValue echo_arguments(std::string text) {
    return support::JsonValue::object_t{{"text", std::move(text)}};
}

[[nodiscard]] std::string first_text_content(const support::JsonValue& result) {
    return result.at("content").get_array().front().at("text").get_string();
}

/// One real MCP server on an ephemeral loopback port, mirroring the
/// application shard's fixture so the owner shard does not link application
/// test support either. HTTPS is the default; `tls=false` listens in plain
/// HTTP (the frozen client accepts both schemes). `server_requests` enables
/// the audit-1190 exchange after initialization and records every response in
/// `roots_trace`.
struct HttpFixtureOptions {
    bool tls{true};
    bool server_requests{false};
    std::optional<std::filesystem::path> roots_trace;
};

class HttpFixtureServer final {
public:
    explicit HttpFixtureServer(HttpFixtureOptions options = {}) {
        int raw_fds[2]{-1, -1};
        REQUIRE(::pipe(raw_fds) == 0);
        support::UniqueFd read_fd(raw_fds[0]);
        support::UniqueFd write_fd(raw_fds[1]);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            (void)read_fd.close();
            if (write_fd.get() != STDOUT_FILENO) {
                (void)::dup2(write_fd.get(), STDOUT_FILENO);
                (void)write_fd.close();
            }
            support::UniqueFd devnull(::open("/dev/null", O_WRONLY));
            if (devnull && devnull.get() != STDERR_FILENO) {
                (void)::dup2(devnull.get(), STDERR_FILENO);
                (void)devnull.close();
            }
            if (options.server_requests) {
                (void)::setenv("PIKE_MCP_SERVER_REQUESTS", "roots", 1);
            }
            if (options.roots_trace) {
                (void)::setenv("PIKE_MCP_ROOTS_TRACE", options.roots_trace->c_str(), 1);
            }
            const std::string script = fixture_path("http_server.py");
            if (!options.tls) {
                ::execlp("python3", "python3", script.c_str(), static_cast<char*>(nullptr));
                ::_exit(127);
            }
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
        (void)write_fd.close();
        std::string line;
        char character = '\0';
        while (::read(read_fd.get(), &character, 1) == 1) {
            if (character == '\n') {
                break;
            }
            line.push_back(character);
        }
        (void)read_fd.close();
        if (!line.starts_with("PORT=")) {
            terminate_child();
        }
        REQUIRE(line.starts_with("PORT="));
        const std::string_view digits{line.data() + 5, line.size() - 5};
        int port = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        if (parsed.ec != std::errc{}) {
            terminate_child();
        }
        REQUIRE(parsed.ec == std::errc{});
        url_ = std::string{options.tls ? "https://" : "http://"} + "127.0.0.1:" + std::to_string(port) + "/mcp";
    }

    HttpFixtureServer(const HttpFixtureServer&) = delete;
    HttpFixtureServer& operator=(const HttpFixtureServer&) = delete;

    ~HttpFixtureServer() { terminate_child(); }

    [[nodiscard]] const std::string& url() const noexcept { return url_; }

private:
    /// Stop and reap the fixture child. Constructor failure paths call this
    /// before a fatal assertion so a failed `PORT=` line cannot strand the
    /// python server; the destructor uses the same idempotent path.
    void terminate_child() noexcept {
        if (pid_ <= 0) {
            return;
        }
        (void)::kill(pid_, SIGTERM);
        int status = 0;
        while (::waitpid(pid_, &status, 0) == -1 && errno == EINTR) {
        }
        pid_ = -1;
    }

    pid_t pid_{-1};
    std::string url_;
};

[[nodiscard]] mcp::HttpServerConfig http_config(const HttpFixtureServer& server) {
    mcp::HttpServerConfig config;
    config.name = "echo";
    config.url = server.url();
    return config;
}

/// The connection-carried roots of the frozen capture
/// (`audit-1190/server-requests.json`): the client answers `roots/list` with
/// these, never with a root derived from the process working directory.
[[nodiscard]] std::vector<mcp::Root> configured_roots() {
    return {mcp::Root{.uri = "file:///workspace", .name = "workspace"}};
}

/// The `expected` object of one scenario in the frozen audit-1190 artifact.
/// Loaded before a test starts any process, so a missing artifact fails
/// without a live harness.
[[nodiscard]] support::JsonValue frozen_scenario_expected(std::string_view scenario_name) {
    const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1190/server-requests.json";
    auto artifact = support::read_json(tests::read_text_if_present(path));
    REQUIRE(artifact.has_value());
    const auto scenarios = artifact->get_object().find("scenarios");
    REQUIRE(scenarios != artifact->get_object().end());
    const auto* entries = scenarios->second.get_if<support::JsonValue::array_t>();
    REQUIRE(entries != nullptr);
    for (const auto& entry : *entries) {
        const auto* object = entry.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        const auto name = object->find("name");
        const auto expected = object->find("expected");
        REQUIRE(name != object->end());
        REQUIRE(expected != object->end());
        if (name->second.holds<std::string>() && name->second.get_string() == scenario_name) {
            return expected->second;
        }
    }
    FAIL("frozen audit-1190 scenario is missing: " << scenario_name);
    return support::JsonValue{};
}

/// Every response line the fixture recorded, in order.
[[nodiscard]] std::vector<support::JsonValue> read_recorded_responses(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<support::JsonValue> messages;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        if (auto parsed = support::read_json(line)) {
            messages.push_back(std::move(*parsed));
        }
    }
    return messages;
}

/// The recorded client response with this JSON-RPC id, when the fixture saw it.
[[nodiscard]] std::optional<support::JsonValue> recorded_response(
        const std::vector<support::JsonValue>& responses, int id) {
    for (const auto& response : responses) {
        const auto* object = response.get_if<support::JsonValue::object_t>();
        if (object == nullptr) {
            continue;
        }
        const auto found = object->find("id");
        if (found != object->end() && found->second.holds<double>() &&
                static_cast<int>(found->second.get_number()) == id) {
            return response;
        }
    }
    return std::nullopt;
}

/// Drive the loop until `predicate` holds or `budget` elapses; the caller
/// asserts the property afterwards, so a timeout is a test failure, not a
/// hang.
[[nodiscard]] bool pump_until(
        boost::asio::io_context& io, const std::function<bool()>& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        io.restart();
        io.run_for(5ms);
    }
    return predicate();
}

} // namespace

TEST_CASE("cch_mcp stdio client initializes, lists, calls and closes against a local fixture",
        "[mcp][owner][issue1065]") {
    boost::asio::io_context io;

    auto client = connect_stdio(io);
    CHECK(client->server_name() == "echo");
    CHECK_FALSE(client->closed());

    const auto names = list_tool_names(io, *client);
    CHECK(names == std::vector<std::string>{"echo", "fail", "crash", "hang", "slow"});

    auto call = run(io,
            client->request("tools/call",
                    support::JsonValue::object_t{
                            {"name", "echo"},
                            {"arguments", echo_arguments("owner seam")},
                    }));
    REQUIRE(call.has_value());
    CHECK(first_text_content(*call) == "owner seam");

    auto failure = run(io,
            client->request("tools/call",
                    support::JsonValue::object_t{
                            {"name", "fail"},
                            {"arguments", support::JsonValue::object_t{}},
                    }));
    REQUIRE(failure.has_value());
    CHECK(failure->at("isError").get_boolean());

    client->close();
    CHECK(client->closed());
}

TEST_CASE("cch_mcp stdio client preserves every JSON structuredContent type from tools/call",
        "[mcp][owner][issue1101][spec]") {
    boost::asio::io_context io;
    auto live = connect_structured_stdio(io);
    const std::vector<std::pair<std::string, support::JsonValue>> cases{
            {"object",
                    support::JsonValue::object_t{
                            {"nested", support::JsonValue::array_t{true, nullptr, "x"}}, {"ok", true}}},
            {"array", support::JsonValue::array_t{1, support::JsonValue::object_t{{"x", true}}, nullptr}},
            {"empty-array", support::JsonValue::array_t{}},
            {"string", "text"},
            {"empty-string", ""},
            {"false", false},
            {"zero", 0},
            {"null", nullptr},
    };
    for (const auto& [kind, expected] : cases) {
        INFO("structured kind: " << kind);
        const auto result = call_structured(io, *live, kind);
        const auto* object = result.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        CHECK_FALSE(object->contains("content"));
        const auto structured = object->find("structuredContent");
        REQUIRE(structured != object->end());
        const auto mismatch = tests::json_mismatch(expected, structured->second);
        CHECK_FALSE(mismatch.has_value());
        if (mismatch) {
            INFO(*mismatch);
        }
    }

    const auto missing = call_structured(io, *live, "missing");
    const auto* missing_object = missing.get_if<support::JsonValue::object_t>();
    REQUIRE(missing_object != nullptr);
    CHECK_FALSE(missing_object->contains("content"));
    CHECK_FALSE(missing_object->contains("structuredContent"));

    live->close();
}

TEST_CASE(
        "cch_mcp call_tool preserves actual wire object order for structuredContent", "[mcp][owner][issue1101][spec]") {
    struct OrderCase {
        std::string kind;
        std::string expected_text;
        std::string map_sorted_text;
        support::JsonValue expected_value;
    };
    const std::vector<OrderCase> cases{
            {"object",
                    "{\n  \"ok\": true,\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ]\n}",
                    "{\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ],\n  \"ok\": true\n}",
                    support::JsonValue::object_t{
                            {"nested", support::JsonValue::array_t{true, nullptr, "x"}}, {"ok", true}}},
            {"object-reverse",
                    "{\n  \"z\": \"last\",\n  \"a\": \"first\",\n  \"m\": \"middle\"\n}",
                    "{\n  \"a\": \"first\",\n  \"m\": \"middle\",\n  \"z\": \"last\"\n}",
                    support::JsonValue::object_t{
                            {"z", "last"},
                            {"a", "first"},
                            {"m", "middle"},
                    }},
            {"object-nested",
                    "{\n  \"outer\": {\n    \"z\": 1,\n    \"a\": 2\n  },\n  \"first\": true\n}",
                    "{\n  \"first\": true,\n  \"outer\": {\n    \"a\": 2,\n    \"z\": 1\n  }\n}",
                    support::JsonValue::object_t{
                            {"outer", support::JsonValue::object_t{{"z", 1}, {"a", 2}}},
                            {"first", true},
                    }},
            {"object-integer-keys",
                    "{\n  \"2\": \"two\",\n  \"10\": \"ten\",\n  \"b\": true,\n  \"a\": false\n}",
                    "{\n  \"10\": \"ten\",\n  \"2\": \"two\",\n  \"a\": false,\n  \"b\": true\n}",
                    support::JsonValue::object_t{
                            {"10", "ten"},
                            {"2", "two"},
                            {"b", true},
                            {"a", false},
                    }},
            {"object-numbers",
                    "{\n  \"integer\": 42,\n  \"negative\": -7,\n  \"float\": 1.5,\n  \"large\": 9007199254740992,\n  "
                    "\"exponent\": 1e+21\n}",
                    "{\n  \"integer\": 42,\n  \"negative\": -7,\n  \"float\": 1.5,\n  \"large\": "
                    "9.007199254740992E15,\n  \"exponent\": 1E21\n}",
                    support::JsonValue::object_t{
                            {"integer", 42},
                            {"negative", -7},
                            {"float", 1.5},
                            {"large", 9007199254740992.0},
                            {"exponent", 1e21},
                    }},
    };

    boost::asio::io_context io;
    auto stdio = connect_structured_stdio(io);
    for (const auto& order_case : cases) {
        INFO("stdio structured kind: " << order_case.kind);
        const auto result = call_structured_tool(io, *stdio, order_case.kind);
        REQUIRE(result.structured_content_json.has_value());
        CHECK(*result.structured_content_json == order_case.expected_text);
        CHECK(*result.structured_content_json != order_case.map_sorted_text);
        const auto* object = result.value.get_if<support::JsonValue::object_t>();
        REQUIRE(object != nullptr);
        CHECK_FALSE(object->contains("content"));
        const auto structured = object->find("structuredContent");
        REQUIRE(structured != object->end());
        const auto mismatch = tests::json_mismatch(order_case.expected_value, structured->second);
        CHECK_FALSE(mismatch.has_value());
        if (mismatch) {
            INFO(*mismatch);
        }
    }
    stdio->close();

    const auto trusted_ca = tests::EnvVarGuard{"SSL_CERT_FILE", tls_fixture_path("test-ca.pem")};
    HttpFixtureServer server;
    auto http = run(io, mcp::HttpClient::connect(http_config(server)));
    REQUIRE(http.has_value());
    for (const char* tool : {"structured", "sse_structured"}) {
        INFO("HTTP structured transport: " << tool);
        auto http_result = run(io, (*http)->call_tool(tool, support::JsonValue::object_t{}));
        REQUIRE(http_result.has_value());
        REQUIRE(http_result->structured_content_json.has_value());
        CHECK(*http_result->structured_content_json ==
                "{\n  \"ok\": true,\n  \"nested\": [\n    true,\n    null,\n    \"x\"\n  ]\n}");
    }
    (*http)->close();
}

TEST_CASE("cch_mcp stdio client resolves a caller-cancelled held request as Cancelled", "[mcp][owner][issue1065]") {
    boost::asio::io_context io;
    auto client = connect_stdio(io);

    std::stop_source stop;
    std::jthread stopper([&stop](std::stop_token) {
        std::this_thread::sleep_for(150ms);
        stop.request_stop();
    });

    auto cancelled = run(io,
            client->request("tools/call",
                    support::JsonValue::object_t{
                            {"name", "hang"},
                            {"arguments", support::JsonValue::object_t{}},
                    },
                    mcp::RequestOptions{.stop_token = stop.get_token()}));
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error().code == support::ErrorCode::Cancelled);

    client->close();
}

TEST_CASE("cch_mcp stdio client reports a crashing fixture as a transport failure", "[mcp][owner][issue1065]") {
    boost::asio::io_context io;
    auto client = connect_stdio(io);

    auto crashed = run(io,
            client->request("tools/call",
                    support::JsonValue::object_t{
                            {"name", "crash"},
                            {"arguments", support::JsonValue::object_t{}},
                    }));
    REQUIRE_FALSE(crashed.has_value());
    CHECK(crashed.error().code != support::ErrorCode::Cancelled);

    client->close();
}

TEST_CASE("cch_mcp streamable HTTP client initializes, lists, calls and closes against a local fixture",
        "[mcp][owner][issue1065]") {
    boost::asio::io_context io;
    const auto trusted_ca = tests::EnvVarGuard{"SSL_CERT_FILE", tls_fixture_path("test-ca.pem")};
    HttpFixtureServer server;

    auto config = http_config(server);
    auto client = run(io, mcp::HttpClient::connect(std::move(config)));
    REQUIRE(client.has_value());

    const auto names = list_tool_names(io, **client);
    CHECK(names == std::vector<std::string>{
                           "echo", "fail", "sse_echo", "hang", "slow", "crash", "structured", "sse_structured"});

    auto call = run(io,
            (*client)->request("tools/call",
                    support::JsonValue::object_t{
                            {"name", "echo"},
                            {"arguments", echo_arguments("http owner seam")},
                    }));
    REQUIRE(call.has_value());
    CHECK(first_text_content(*call) == "http owner seam");

    (*client)->close();
}

TEST_CASE("cch_mcp stdio client answers a fixture server's ping and roots from the connection",
        "[mcp][owner][frozen][issue1190]") {
    const auto expected = frozen_scenario_expected("a stdio server ping is answered with an empty result and "
                                                   "roots/list returns the connection's configured roots");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "roots.jsonl";
    auto config = echo_server_config();
    config.roots = configured_roots();
    config.env.emplace("PIKE_MCP_SERVER_REQUESTS", "roots");
    config.env.emplace("PIKE_MCP_ROOTS_TRACE", trace.string());

    boost::asio::io_context io;
    auto client = run(io, mcp::StdioClient::connect(std::move(config)));
    REQUIRE(client.has_value());
    const bool recorded = pump_until(io, [&] { return read_recorded_responses(trace).size() >= 3; }, 10s);
    (*client)->close();
    const auto responses = read_recorded_responses(trace);

    // Release-then-assert (CODING_STANDARDS.md §11.10): the client is closed
    // before any discriminating assertion can abort this frame.
    REQUIRE(recorded);
    REQUIRE(responses.size() == 3);
    const auto ping = recorded_response(responses, 101);
    const auto roots = recorded_response(responses, 102);
    REQUIRE(ping.has_value());
    REQUIRE(roots.has_value());
    const auto ping_mismatch = tests::json_mismatch(expected.get_object().at("ping"), *ping);
    const auto roots_mismatch = tests::json_mismatch(expected.get_object().at("rootsList"), *roots);
    CHECK_FALSE(ping_mismatch.has_value());
    CHECK_FALSE(roots_mismatch.has_value());
    if (ping_mismatch) {
        INFO(*ping_mismatch);
    }
    if (roots_mismatch) {
        INFO(*roots_mismatch);
    }
}

TEST_CASE("cch_mcp streamable HTTP client answers a fixture server's ping and roots from the connection",
        "[mcp][owner][frozen][issue1190]") {
    const auto expected = frozen_scenario_expected("a streamable HTTP server ping is answered with an empty result and "
                                                   "roots/list returns the connection's configured roots");
    tests::TempWorkspace workspace;
    const auto trace = workspace.path() / "roots.jsonl";
    HttpFixtureServer server(HttpFixtureOptions{.tls = false, .server_requests = true, .roots_trace = trace});
    auto config = http_config(server);
    config.roots = configured_roots();

    boost::asio::io_context io;
    auto client = run(io, mcp::HttpClient::connect(std::move(config)));
    REQUIRE(client.has_value());
    const bool recorded = pump_until(io, [&] { return read_recorded_responses(trace).size() >= 3; }, 10s);
    (*client)->close();
    const auto responses = read_recorded_responses(trace);

    // Release-then-assert (CODING_STANDARDS.md §11.10): the client is closed
    // before any discriminating assertion can abort this frame.
    REQUIRE(recorded);
    REQUIRE(responses.size() == 3);
    const auto ping = recorded_response(responses, 101);
    const auto roots = recorded_response(responses, 102);
    REQUIRE(ping.has_value());
    REQUIRE(roots.has_value());
    const auto ping_mismatch = tests::json_mismatch(expected.get_object().at("ping"), *ping);
    const auto roots_mismatch = tests::json_mismatch(expected.get_object().at("rootsList"), *roots);
    CHECK_FALSE(ping_mismatch.has_value());
    CHECK_FALSE(roots_mismatch.has_value());
    if (ping_mismatch) {
        INFO(*ping_mismatch);
    }
    if (roots_mismatch) {
        INFO(*roots_mismatch);
    }
}

TEST_CASE("cch_mcp clients answer a fixture server's unhandled request -32601 on both transports",
        "[mcp][owner][frozen][issue1190]") {
    const auto expected = frozen_scenario_expected(
            "a server request with no matching handler is answered -32601 before the connection closes");
    tests::TempWorkspace workspace;
    boost::asio::io_context io;

    const auto stdio_trace = workspace.path() / "stdio-roots.jsonl";
    auto stdio_config = echo_server_config();
    stdio_config.roots = configured_roots();
    stdio_config.env.emplace("PIKE_MCP_SERVER_REQUESTS", "roots");
    stdio_config.env.emplace("PIKE_MCP_ROOTS_TRACE", stdio_trace.string());
    auto stdio = run(io, mcp::StdioClient::connect(std::move(stdio_config)));
    REQUIRE(stdio.has_value());
    const bool stdio_recorded = pump_until(io, [&] { return read_recorded_responses(stdio_trace).size() >= 3; }, 10s);
    (*stdio)->close();
    const auto stdio_responses = read_recorded_responses(stdio_trace);

    const auto http_trace = workspace.path() / "http-roots.jsonl";
    HttpFixtureServer server(HttpFixtureOptions{.tls = false, .server_requests = true, .roots_trace = http_trace});
    auto http_config_value = http_config(server);
    http_config_value.roots = configured_roots();
    auto http = run(io, mcp::HttpClient::connect(std::move(http_config_value)));
    REQUIRE(http.has_value());
    const bool http_recorded = pump_until(io, [&] { return read_recorded_responses(http_trace).size() >= 3; }, 10s);
    (*http)->close();
    const auto http_responses = read_recorded_responses(http_trace);

    // Release-then-assert (CODING_STANDARDS.md §11.10): both clients are
    // closed before any discriminating assertion can abort this frame.
    REQUIRE(stdio_recorded);
    REQUIRE(http_recorded);
    const auto stdio_error = recorded_response(stdio_responses, 103);
    const auto http_error = recorded_response(http_responses, 103);
    REQUIRE(stdio_error.has_value());
    REQUIRE(http_error.has_value());
    const auto stdio_mismatch = tests::json_mismatch(expected.get_object().at("stdio"), *stdio_error);
    const auto http_mismatch = tests::json_mismatch(expected.get_object().at("streamableHttp"), *http_error);
    CHECK_FALSE(stdio_mismatch.has_value());
    CHECK_FALSE(http_mismatch.has_value());
    if (stdio_mismatch) {
        INFO(*stdio_mismatch);
    }
    if (http_mismatch) {
        INFO(*http_mismatch);
    }
}
