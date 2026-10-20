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
                    support::JsonValue::object_t{
                            {"name", "structured"},
                            {"arguments", support::JsonValue::object_t{{"kind", std::move(kind)}}},
                    }));
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

/// One real TLS MCP server on an ephemeral loopback port, mirroring the
/// application shard's fixture so the owner shard does not link application
/// test support either.
class HttpFixtureServer final {
public:
    HttpFixtureServer() {
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
        url_ = "https://127.0.0.1:" + std::to_string(port) + "/mcp";
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
    CHECK(names == std::vector<std::string>{"echo", "fail", "sse_echo", "hang", "slow", "crash"});

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
