// The MCP Host's Upstream bearer credentials, end to end (issue #838).
//
// Every case drives the full client stack above the one transport seam, so the
// assertions are on what the Upstream received and on what the client stack
// made of the answer: the resolved token reaches the wire as the
// `Authorization` header, a credential that cannot be resolved sends nothing
// at all, and nothing the Upstream hands back — a diagnostic, a tool result —
// can carry the token into a status surface or a session record. The
// `AuthStorage`-backed store the Runtime installs is covered by
// `tests/coding_agent/McpCredentialStoreTest.cpp`.

#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamClient.hpp>
#include <cch/support/BoundedText.hpp>
#include <cch/support/Redactor.hpp>
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"
#include "mcp/transport/BoostBeastStreamableHttpTransport.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/LocalMcpHttpServer.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using support::JsonValue;

namespace {

namespace asio = boost::asio;

/// The committed test trust anchor (issue #638).
constexpr std::string_view kTlsFixtureDir{"/tests/ai/providers/tls/"};

/// A liveness bound, not a performance gate.
constexpr auto kExchangeBound = std::chrono::seconds{10};

/// The environment variable a `bearer-env:<VAR>` reference names, and the
/// token its value carries. The token is long enough to be distinctive in
/// arbitrary text, which is what the credential erasure requires.
constexpr std::string_view kTokenVar{"PI_TEST_MCP_BEARER_TOKEN"};
constexpr std::string_view kToken{"pike-mcp-upstream-bearer-0123456789abcdef"};

[[nodiscard]] std::string read_test_ca() {
    std::ifstream input(std::string{CCH_SOURCE_DIR} + std::string{kTlsFixtureDir} + "test-ca.pem", std::ios::binary);
    REQUIRE(input.good());
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::shared_ptr<mcp::McpTransport> trusted_transport(asio::io_context& io) {
    return std::make_shared<mcp::transport::BoostBeastStreamableHttpTransport>(io.get_executor(),
            mcp::transport::StreamableHttpTransportOptions{.trusted_ca_certificate_pem = read_test_ca()});
}

/// Drive one pending `AsyncResult` on `io` to its terminal outcome.
template <typename T>
[[nodiscard]] support::Expected<T> drive_on(asio::io_context& io, support::AsyncResult<T> operation) {
    auto outcome = std::make_shared<support::Expected<T>>(std::unexpected(support::make_error(support::ErrorCode::Busy,
            "the operation never completed",
            "the local MCP server drives every exchange over a real socket")));
    auto pending = std::make_shared<support::AsyncResult<T>>(std::move(operation));
    asio::co_spawn(
            io,
            [pending = std::move(pending), outcome]() -> asio::awaitable<void> {
                *outcome = co_await support::detail::await_async_result(std::move(*pending));
            },
            asio::detached);
    io.restart();
    io.run();
    return std::move(*outcome);
}

/// The `cch_mcp` half of the credential handoff under test: the store records
/// what the client stack persisted and serves what it was seeded with.
class RecordingCredentialStore final : public mcp::UpstreamCredentialStore {
public:
    std::vector<std::pair<std::string, std::string>> writes{};
    std::optional<support::Error> read_failure{std::nullopt};

    void seed(std::string server_id, std::string bearer) {
        stored_.insert_or_assign(std::move(server_id), std::move(bearer));
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> read_bearer(std::string server_id) override {
        if (read_failure.has_value()) {
            return cch::support::AsyncResult<std::optional<std::string>>(
                    std::expected<std::optional<std::string>, support::Error>{std::unexpected(*read_failure)});
        }
        const auto found = stored_.find(server_id);
        return cch::support::AsyncResult<std::optional<std::string>>(
                std::expected<std::optional<std::string>, support::Error>{found == stored_.end()
                                                                                       ? std::optional<std::string>{}
                                                                                       : std::optional<std::string>{found->second}});
    }

    [[nodiscard]] cch::support::AsyncResult<void> write_bearer(std::string server_id, std::string bearer) override {
        writes.emplace_back(server_id, bearer);
        stored_.insert_or_assign(std::move(server_id), std::move(bearer));
        return cch::support::AsyncResult<void>(std::expected<void, support::Error>{});
    }

private:
    std::map<std::string, std::string> stored_{};
};

[[nodiscard]] std::optional<mcp::jsonrpc::WireMessage> decode(const tests::RecordedHttpRequest& request) {
    auto parsed = support::read_json(request.body);
    if (!parsed) {
        return std::nullopt;
    }
    auto message = mcp::jsonrpc::decode_message(*parsed);
    return message ? std::optional<mcp::jsonrpc::WireMessage>{*message} : std::nullopt;
}

[[nodiscard]] std::string framed(const tests::RecordedHttpRequest& request, const JsonValue& result) {
    const auto message = decode(request);
    if (!message) {
        return "{}";
    }
    auto text = support::write_json(mcp::jsonrpc::encode_result(message->id.value_or(0.0), result));
    return text ? *text : "{}";
}

/// An Upstream that answers only a request carrying the expected bearer, so a
/// request without it is a 401 rather than a catalog. The chain is verified by
/// the Upstream accepting the exchange, not only by the header's presence.
[[nodiscard]] tests::LocalMcpHttpServer::Handler authenticated_upstream(std::string_view expected_bearer) {
    return [expected = std::string{"Bearer "} + std::string{expected_bearer}](
                    const tests::RecordedHttpRequest& request) -> std::optional<tests::McpServerReply> {
        if (request.header("Authorization") != expected) {
            return tests::McpServerReply{
                    .status_code = 401,
                    .body = R"({"error":"unauthorized"})",
            };
        }
        const auto message = decode(request);
        if (!message) {
            return tests::McpServerReply{};
        }
        if (message->method == mcp::protocol::kMethodDiscover) {
            return tests::McpServerReply{.body = framed(request, tests::discover_result())};
        }
        if (message->method == mcp::protocol::kMethodListTools) {
            return tests::McpServerReply{
                    .body = framed(request, tests::tool_list_result({tests::tool_entry("post_message")}))};
        }
        return tests::McpServerReply{.body = framed(request, tests::tool_call_result(JsonValue::array_t{}))};
    };
}

void script_conforming_upstream(tests::ScriptedMcpTransport& transport) {
    transport.answer(mcp::protocol::kMethodDiscover, tests::ScriptedMcpAnswer{.result = tests::discover_result()});
    transport.answer(mcp::protocol::kMethodListTools,
            tests::ScriptedMcpAnswer{.result = tests::tool_list_result({tests::tool_entry("post_message")})});
    transport.answer(mcp::protocol::kMethodCallTool,
            tests::ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::array_t{})});
}

[[nodiscard]] mcp::UpstreamToolDescriptor post_message_tool() {
    return mcp::UpstreamToolDescriptor{
            .name = "post_message",
            .description = "an upstream tool",
            .parameters = JsonValue::object_t{},
            .header_parameters = {},
    };
}

/// An authenticated client over the in-memory transport, the shape every
/// credential case that does not need a socket uses.
struct ScriptedClient {
    ScriptedClient() { script_conforming_upstream(*transport); }

    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    std::shared_ptr<RecordingCredentialStore> store{std::make_shared<RecordingCredentialStore>()};
    mcp::UpstreamClient client{"executor",
            transport,
            mcp::UpstreamClientOptions{.url = "https://upstream.example.com/mcp",
                    .bearer_env_var = std::string{kTokenVar},
                    .credentials = store}};
};

} // namespace

TEST_CASE("the MCP Host resolves a bearer from the environment, persists it, and sends it as the Authorization header",
        "[mcp][credentials][auth][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::string{kToken});
    auto store = std::make_shared<RecordingCredentialStore>();
    tests::LocalMcpHttpServer server(authenticated_upstream(kToken));
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("executor",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(),
                    .request_timeout = kExchangeBound,
                    .bearer_env_var = std::string{kTokenVar},
                    .credentials = store});

    // The Upstream answers only an authenticated request, so the catalog below
    // is itself the evidence that the whole chain — environment variable,
    // credential store, request header — completed.
    auto catalog = drive_on(io, client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 1);
    CHECK(catalog->tools.front().name == "post_message");

    const auto recorded = server.requests();
    REQUIRE(recorded.size() == 2);
    for (const auto& request : recorded) {
        CHECK(request.header("Authorization") == "Bearer " + std::string{kToken});
    }

    // The token reached the credential store once, and the exchange after that
    // write found the store already held it and did not write it again.
    REQUIRE(store->writes.size() == 1);
    CHECK(store->writes.front().first == "executor");
    CHECK(store->writes.front().second == kToken);
}

TEST_CASE("the MCP Host keys an Upstream credential under the mcp.<server-id> namespace",
        "[mcp][credentials][auth][issue838][spec]") {
    // The namespace is the shared credential store's key for one Server Id, and
    // #849 keys its OAuth credentials under the same one.
    CHECK(mcp::credential_key("executor") == "mcp.executor");
    CHECK(mcp::credential_key("docs_server-2") == "mcp.docs_server-2");
    // A Server Id is drawn from `[A-Za-z0-9_-]` and can never contain the
    // separator, so the namespace is unambiguous over the store's flat key map.
    CHECK(mcp::credential_key("executor-1_B").find('.', 4) == std::string::npos);
}

TEST_CASE("an Upstream with no stored credential and no environment value sends no request",
        "[mcp][credentials][auth][failure][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::nullopt);
    auto store = std::make_shared<RecordingCredentialStore>();
    tests::LocalMcpHttpServer server(authenticated_upstream(kToken));
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("executor",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(),
                    .request_timeout = kExchangeBound,
                    .bearer_env_var = std::string{kTokenVar},
                    .credentials = store});

    auto outcome = drive_on(io, client.list_tools());
    REQUIRE_FALSE(outcome.has_value());
    // The failure is the ordinary connection failure: no guessed token, no
    // empty token, and nothing written to the Upstream.
    CHECK(outcome.error().code == support::ErrorCode::Auth);
    CHECK(server.requests().empty());
    CHECK(store->writes.empty());
    // The failure names the Server Id and the environment variable, and never a
    // value.
    CHECK(outcome.error().detail.find("executor") != std::string::npos);
    CHECK(outcome.error().detail.find(kTokenVar) != std::string::npos);
}

TEST_CASE("an Upstream bearer resolves from the credential store when the environment variable is unset",
        "[mcp][credentials][auth][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::nullopt);
    ScriptedClient scripted;
    scripted.store->seed("executor", std::string{kToken});

    auto catalog = tests::drive(scripted.client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(scripted.transport->request_count() > 0);
    for (const auto& request : scripted.transport->requests()) {
        CHECK(request.headers.at("Authorization") == "Bearer " + std::string{kToken});
    }
    // A credential the host did not resolve from the environment is not written
    // back over the store that already held it.
    CHECK(scripted.store->writes.empty());
}

TEST_CASE("an unreadable Upstream credential store fails the connection rather than sending an unauthenticated request",
        "[mcp][credentials][auth][failure][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::string{kToken});
    ScriptedClient scripted;
    scripted.store->read_failure = support::make_error(
            support::ErrorCode::Auth, "failed to read auth file", "/home/user/.pike/agent/auth.json");

    auto outcome = tests::drive(scripted.client.list_tools());
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Auth);
    CHECK(scripted.transport->request_count() == 0);
}

TEST_CASE("an Upstream that declares no bearer credential is sent no Authorization header",
        "[mcp][credentials][auth][issue838][spec]") {
    auto transport = std::make_shared<tests::ScriptedMcpTransport>();
    script_conforming_upstream(*transport);
    mcp::UpstreamClient client("open",
            transport,
            mcp::UpstreamClientOptions{.url = "https://upstream.example.com/mcp"});

    auto catalog = tests::drive(client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(transport->request_count() > 0);
    for (const auto& request : transport->requests()) {
        CHECK_FALSE(request.headers.contains("Authorization"));
    }
}

TEST_CASE("a rotated bearer environment value is applied to the next request without a reconnect",
        "[mcp][credentials][auth][issue838][spec]") {
    // Credentials resolve per request, so a token rotated in the environment
    // takes effect on the next exchange of the same connection.
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::string{kToken});
    ScriptedClient scripted;

    REQUIRE(tests::drive(scripted.client.probe_era()).has_value());
    token_environment.set("pike-mcp-upstream-bearer-rotated");
    REQUIRE(tests::drive(scripted.client.probe_era()).has_value());

    REQUIRE(scripted.transport->request_count() == 2);
    CHECK(scripted.transport->requests()[0].headers.at("Authorization") == "Bearer " + std::string{kToken});
    CHECK(scripted.transport->requests()[1].headers.at("Authorization") == "Bearer pike-mcp-upstream-bearer-rotated");
    REQUIRE(scripted.store->writes.size() == 2);
}

TEST_CASE("an Authorization header the request already carries wins over the resolved bearer",
        "[mcp][credentials][auth][issue838][spec]") {
    // HTTP field names are case-insensitive, so a header spelled in any case
    // occupies the slot the bearer would fill.
    for (const std::string& occupied :
            {std::string{"Authorization"}, std::string{"authorization"}, std::string{"AUTHORIZATION"}}) {
        mcp::McpRequest request;
        request.headers.emplace(occupied, "Bearer configured");
        mcp::apply_upstream_auth(request, mcp::UpstreamAuth{.bearer = std::string{kToken}});
        CHECK(request.headers.size() == 1);
        CHECK(request.headers.at(occupied) == "Bearer configured");
    }

    mcp::McpRequest unoccupied;
    unoccupied.headers.emplace("Mcp-Method", "tools/call");
    mcp::apply_upstream_auth(unoccupied, mcp::UpstreamAuth{.bearer = std::string{kToken}});
    CHECK(unoccupied.headers.at("Authorization") == "Bearer " + std::string{kToken});

    // A server that declares no credential adds no header at all.
    mcp::McpRequest anonymous;
    mcp::apply_upstream_auth(anonymous, mcp::UpstreamAuth{});
    CHECK(anonymous.headers.empty());
}

TEST_CASE("a bearer echoed back in an Upstream error message never reaches the diagnostic",
        "[mcp][credentials][redaction][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::string{kToken});
    ScriptedClient scripted;
    scripted.transport->answer(mcp::protocol::kMethodCallTool,
            tests::ScriptedMcpAnswer{
                    .error_code = -32000,
                    .error_message = std::string{"rejected: Authorization: Bearer "} + std::string{kToken},
            });

    auto outcome = tests::drive(scripted.client.call_tool(mcp::UpstreamToolCall{
            .tool = post_message_tool(),
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->is_error);
    CHECK(outcome->diagnostic.find(kToken) == std::string::npos);
    CHECK(outcome->diagnostic.find(support::kRedactionMarker) != std::string::npos);
    // The failure is still the Upstream's own failure, reported as one failed
    // tool call rather than as a client-stack failure.
    CHECK(outcome->diagnostic.find("-32000") != std::string::npos);
}

TEST_CASE("a bearer echoed back in a tool result never reaches the model's context",
        "[mcp][credentials][redaction][issue838][spec]") {
    tests::EnvVarGuard token_environment(std::string{kTokenVar}, std::string{kToken});
    ScriptedClient scripted;
    // The Upstream echoes the credential with no key and no recognizable
    // prefix, which is the form the shape-based rules cannot match.
    scripted.transport->answer(mcp::protocol::kMethodCallTool,
            tests::ScriptedMcpAnswer{.result = tests::tool_call_result(JsonValue::array_t{
                    JsonValue::object_t{
                            {"type", JsonValue("text")},
                            {"text", JsonValue(std::string{"your token is "} + std::string{kToken})},
                    },
            })});

    auto outcome = tests::drive(scripted.client.call_tool(mcp::UpstreamToolCall{
            .tool = post_message_tool(),
            .arguments = JsonValue::object_t{},
    }));
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->is_error);
    const auto* const content = outcome->content.get_if<JsonValue::array_t>();
    REQUIRE(content != nullptr);
    REQUIRE(content->size() == 1);
    const auto* const block = (*content)[0].get_if<JsonValue::object_t>();
    REQUIRE(block != nullptr);
    const auto* const text = block->at("text").get_if<std::string>();
    REQUIRE(text != nullptr);
    CHECK(text->find(kToken) == std::string::npos);
    CHECK(text->find(support::kRedactionMarker) != std::string::npos);
    // The rest of the Upstream's own content is untouched.
    CHECK(text->find("your token is") != std::string::npos);
}

TEST_CASE("an oversized MCP diagnostic is redacted before it is truncated",
        "[mcp][credentials][redaction][limits][issue838][spec]") {
    const std::string token{kToken};

    // A credential assignment carried well past the diagnostic bound: the
    // bound is applied to the redacted text, so what is retained is the
    // complete marker rather than a cut one (CODING_STANDARDS.md section
    // 10.2).
    std::string assigned(1000, 'a');
    assigned += " token=";
    assigned += token;
    assigned.append(4000, 'b');
    REQUIRE(assigned.size() > mcp::protocol::kMaxDiagnosticBytes);

    const auto bounded = mcp::redaction::redacted_text(assigned, token);
    CHECK(bounded.size() <= mcp::protocol::kMaxDiagnosticBytes);
    CHECK(bounded.find(token) == std::string::npos);
    CHECK(bounded.find(support::kRedactionMarker) != std::string::npos);
    CHECK(bounded.find(support::kRedactionMarker) + support::kRedactionMarker.size() <= bounded.size());

    // A bare token with no key and no recognizable prefix is what the shared
    // shape-based rules cannot match, so the erasure is what removes it — and
    // it runs on the whole text before the bound, which is the ordering the
    // ticket requires.
    std::string echoed(400, 'c');
    echoed += token;
    echoed.append(4000, 'd');
    REQUIRE(support::bounded_redacted_text(echoed, mcp::protocol::kMaxDiagnosticBytes).find(token) !=
            std::string::npos);
    const auto erased = mcp::redaction::redacted_text(echoed, token);
    CHECK(erased.size() <= mcp::protocol::kMaxDiagnosticBytes);
    CHECK(erased.find(token) == std::string::npos);
    CHECK(erased.find(support::kRedactionMarker) != std::string::npos);
    CHECK(erased.find(support::kRedactionMarker) + support::kRedactionMarker.size() <= erased.size());
}

TEST_CASE("the MCP Host redacts a credential from every text a tool result carries",
        "[mcp][credentials][redaction][issue838][spec]") {
    const std::string token{kToken};
    const JsonValue value = JsonValue::object_t{
            {"authorization", JsonValue(std::string{"Bearer "} + token)},
            {"nested", JsonValue::array_t{JsonValue(token), JsonValue(42.0), JsonValue(true), JsonValue()}},
            {"count", JsonValue(7.0)},
    };
    const auto redacted = mcp::redaction::redacted_value(value, token);
    const auto text = support::write_json(redacted);
    REQUIRE(text.has_value());
    CHECK(text->find(token) == std::string::npos);
    CHECK(text->find(support::kRedactionMarker) != std::string::npos);
    // Structure and the non-text members survive unchanged.
    const auto* const object = redacted.get_if<JsonValue::object_t>();
    REQUIRE(object != nullptr);
    REQUIRE(object->at("nested").get_if<JsonValue::array_t>() != nullptr);
    const auto& nested = *object->at("nested").get_if<JsonValue::array_t>();
    CHECK(nested[1].get_if<double>() != nullptr);
    CHECK(nested[2].get_if<bool>() != nullptr);
    CHECK(nested[3].holds<JsonValue::null_t>());
    CHECK(object->at("count").get_if<double>() != nullptr);
}
