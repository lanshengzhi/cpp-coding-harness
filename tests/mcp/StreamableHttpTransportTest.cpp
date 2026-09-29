// The MCP Host's production transport — Streamable HTTP over Beast TLS —
// driven end to end against a test-only local MCP HTTP server (issue #837).
//
// The seam is the one the scripted cases already use: this file only swaps the
// in-memory `ScriptedMcpTransport` for the Beast TLS transport the Runtime
// will use, and asserts on what the Upstream received and what the client
// stack made of the answer. Every case runs over a real socket under the
// committed test CA (ADR 0054's test-CA pattern), so framing, the required
// request headers, the base64 sentinel encoding, event-stream consumption, the
// broken-stream and flood containment, and the declared per-class retry policy
// are all covered on the wire rather than in memory.

#include <cch/mcp/UpstreamClient.hpp>
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/transport/BoostBeastStreamableHttpTransport.hpp"
#include "mcp/transport/RetryPolicy.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"
#include "support/LocalMcpHttpServer.hpp"
#include "support/ScriptedMcpTransport.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
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

using namespace cch;
using support::JsonValue;

namespace {

namespace asio = boost::asio;

/// The committed test trust anchor (issue #638), the same one the local
/// `wss://` mock uses; the regeneration commands and why the credentials are
/// committed live in `tests/ai/providers/tls/README.md`.
constexpr std::string_view kTlsFixtureDir{"/tests/ai/providers/tls/"};

/// A liveness bound, not a performance gate: the flood and the broken stream
/// must both end long before the fixture's own backstop.
constexpr auto kExchangeBound = std::chrono::seconds{10};

/// One advertised tool, annotated so the `Mcp-Param-*` mirroring has a source.
[[nodiscard]] JsonValue annotated_tool() {
    const JsonValue annotations = JsonValue::object_t{
            {"x-mcp-header", JsonValue::object_t{{"note", JsonValue::object_t{{"name", JsonValue("Note")}}}}},
    };
    return tests::tool_entry("post_message", {"note"}, annotations);
}

[[nodiscard]] std::string read_test_ca() {
    std::ifstream input(std::string{CCH_SOURCE_DIR} + std::string{kTlsFixtureDir} + "test-ca.pem", std::ios::binary);
    REQUIRE(input.good());
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
}

/// Decode the JSON-RPC request the Upstream received, so a fixture answers the
/// way a conforming one does — with the response for that request's own id.
[[nodiscard]] std::optional<mcp::jsonrpc::WireMessage> decode(const tests::RecordedHttpRequest& request) {
    auto parsed = support::read_json(request.body);
    if (!parsed) {
        return std::nullopt;
    }
    auto message = mcp::jsonrpc::decode_message(*parsed);
    if (!message) {
        return std::nullopt;
    }
    return *message;
}

[[nodiscard]] std::string framed(const tests::RecordedHttpRequest& request, const JsonValue& result) {
    const auto message = decode(request);
    if (!message) {
        return "{}";
    }
    auto text = support::write_json(mcp::jsonrpc::encode_result(message->id.value_or(0.0), result));
    return text ? *text : "{}";
}

/// The answer a conforming Modern Era Upstream gives to any of the three
/// methods the client stack invokes. A case that is not about one method's
/// reply falls back to this.
[[nodiscard]] tests::McpServerReply ordinary_reply(const tests::RecordedHttpRequest& request) {
    const auto message = decode(request);
    if (!message) {
        return {};
    }
    if (message->method == mcp::protocol::kMethodDiscover) {
        return {.body = framed(request, tests::discover_result())};
    }
    if (message->method == mcp::protocol::kMethodListTools) {
        return {.body = framed(request, tests::tool_list_result({annotated_tool()}))};
    }
    return {.body = framed(request, tests::tool_call_result(JsonValue::array_t{}))};
}

/// A case scripts one method and delegates the rest to `ordinary_reply`; a
/// handler that returns `std::nullopt` for a request means "answer it
/// ordinarily".
using McpReplyScript = std::function<std::optional<tests::McpServerReply>(const tests::RecordedHttpRequest&)>;

[[nodiscard]] McpReplyScript script(
        std::function<std::optional<tests::McpServerReply>(std::string_view method, const tests::RecordedHttpRequest&)>
                shaped) {
    return [shaped = std::move(shaped)](const tests::RecordedHttpRequest& request) {
        const auto message = decode(request);
        const auto reply = shaped(message ? std::string_view{message->method} : std::string_view{}, request);
        return reply.has_value() ? *reply : ordinary_reply(request);
    };
}

/// The reply a case that shapes nothing gets.
[[nodiscard]] McpReplyScript conforming_upstream() {
    return script([](std::string_view, const tests::RecordedHttpRequest&) { return std::nullopt; });
}

/// The `note` argument of a `tools/call` request, so a case can tell one call
/// from another.
[[nodiscard]] std::string call_note(const tests::RecordedHttpRequest& request) {
    const auto message = decode(request);
    if (!message) {
        return {};
    }
    const auto* arguments = message->params.get_if<JsonValue::object_t>();
    if (arguments == nullptr) {
        return {};
    }
    const auto found = arguments->find("arguments");
    if (found == arguments->end()) {
        return {};
    }
    const auto* values = found->second.get_if<JsonValue::object_t>();
    if (values == nullptr) {
        return {};
    }
    const auto note = values->find("note");
    if (note == values->end()) {
        return {};
    }
    const auto* text = note->second.get_if<std::string>();
    return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] std::optional<double> recorded_id(const tests::RecordedHttpRequest& request) {
    auto parsed = support::read_json(request.body);
    if (!parsed) {
        return std::nullopt;
    }
    const auto* envelope = parsed->get_if<JsonValue::object_t>();
    if (envelope == nullptr) {
        return std::nullopt;
    }
    const auto found = envelope->find("id");
    if (found == envelope->end()) {
        return std::nullopt;
    }
    const auto* id = found->second.get_if<double>();
    return id == nullptr ? std::nullopt : std::optional<double>{*id};
}

/// The protocol version a recorded request's reserved `_meta` carries.
[[nodiscard]] std::string meta_protocol_version(const tests::RecordedHttpRequest& request) {
    const auto message = decode(request);
    if (!message) {
        return {};
    }
    const auto* parameters = message->params.get_if<JsonValue::object_t>();
    if (parameters == nullptr) {
        return {};
    }
    const auto found = parameters->find("_meta");
    if (found == parameters->end()) {
        return {};
    }
    const auto* meta = found->second.get_if<JsonValue::object_t>();
    if (meta == nullptr) {
        return {};
    }
    const auto version = meta->find(std::string(mcp::protocol::kMetaProtocolVersionKey));
    if (version == meta->end()) {
        return {};
    }
    const auto* text = version->second.get_if<std::string>();
    return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] std::shared_ptr<mcp::McpTransport> trusted_transport(asio::io_context& io) {
    return std::make_shared<mcp::transport::BoostBeastStreamableHttpTransport>(io.get_executor(),
            mcp::transport::StreamableHttpTransportOptions{.trusted_ca_certificate_pem = read_test_ca()});
}

/// Drive one pending `AsyncResult` on `io` to its terminal outcome. The
/// exchange only completes once its socket work is done, so the loop runs to
/// quiescence before the outcome is read.
template <typename T>
[[nodiscard]] support::Expected<T> drive_on(asio::io_context& io, support::AsyncResult<T> operation) {
    auto outcome = std::make_shared<support::Expected<T>>(std::unexpected(support::make_error(support::ErrorCode::Busy,
            "the operation never completed",
            "the local MCP server drives every exchange over a real socket")));
    auto pending = std::make_shared<support::AsyncResult<T>>(std::move(operation));
    // The move-only operation reaches the coroutine through a shared owner,
    // because `co_spawn` copies the closure it is given.
    asio::co_spawn(
            io,
            [pending = std::move(pending), outcome]() -> asio::awaitable<void> {
                *outcome = co_await support::detail::await_async_result(std::move(*pending));
            },
            asio::detached);
    // `io_context::run` leaves the context stopped once its last handler
    // completes, so a second exchange on the same context restarts it first.
    io.restart();
    io.run();
    return std::move(*outcome);
}

/// A hand-built descriptor for the case that calls a tool without walking the
/// catalog first.
[[nodiscard]] mcp::UpstreamToolDescriptor post_message_tool() {
    return mcp::UpstreamToolDescriptor{
            .name = "post_message",
            .description = "an upstream tool",
            .parameters = JsonValue::object_t{},
            .header_parameters = {},
    };
}

} // namespace

TEST_CASE("the Streamable HTTP transport completes a JSON round trip against a local MCP server",
        "[mcp][transport][tls][issue837][spec]") {
    tests::LocalMcpHttpServer server(conforming_upstream());
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    auto catalog = drive_on(io, client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 1);
    CHECK(catalog->tools.front().name == "post_message");

    const auto recorded = server.requests();
    REQUIRE(recorded.size() == 2);
    CHECK(recorded[0].method == "POST");
    CHECK(recorded[0].target == "/mcp");
    CHECK(recorded[0].header("Content-Type") == "application/json");
    CHECK(recorded[1].header("Mcp-Method") == "tools/list");
    CHECK_FALSE(recorded[1].has_header("Mcp-Name"));
}

TEST_CASE("the Streamable HTTP transport assembles an event stream into the response the client stack reads",
        "[mcp][transport][sse][issue837][spec]") {
    const std::string progress = support::write_json(JsonValue::object_t{
                                                             {"jsonrpc", JsonValue("2.0")},
                                                             {"method", JsonValue("notifications/progress")},
                                                     })
                                         .value_or("{}");
    tests::LocalMcpHttpServer server(
            script([&progress](std::string_view method,
                           const tests::RecordedHttpRequest& request) -> std::optional<tests::McpServerReply> {
                if (method != mcp::protocol::kMethodCallTool) {
                    return std::nullopt;
                }
                // The notification arrives ahead of the response the call is waiting
                // for, exactly as an Upstream reports progress.
                return tests::McpServerReply{
                        .content_type = "text/event-stream",
                        .as_event_stream = true,
                        .event_payloads = {progress, framed(request, tests::tool_call_result(JsonValue::array_t{}))},
                };
            }));
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    auto outcome = drive_on(io,
            client.call_tool(mcp::UpstreamToolCall{
                    .tool = post_message_tool(),
                    .arguments = JsonValue::object_t{{"note", JsonValue("hello")}},
            }));
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->is_error);
    CHECK(outcome->diagnostic.empty());
    CHECK(server.requests().size() == 2);
}

TEST_CASE("the Streamable HTTP transport writes the required MCP headers and mirrors an annotated parameter",
        "[mcp][transport][headers][issue837][spec]") {
    tests::LocalMcpHttpServer server(conforming_upstream());
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});
    auto catalog = drive_on(io, client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.front().header_parameters.size() == 1);

    // The mirrored value is not header-safe, so it reaches the wire under the
    // base64 sentinel (SEP-2243) whatever the transport is handed.
    auto outcome = drive_on(io,
            client.call_tool(mcp::UpstreamToolCall{
                    .tool = catalog->tools.front(),
                    .arguments = JsonValue::object_t{{"note", JsonValue(std::string("a\nb"))}},
            }));
    REQUIRE(outcome.has_value());

    const auto recorded = server.requests();
    REQUIRE(recorded.size() == 3);
    const auto& call = recorded.back();
    CHECK(call.header("MCP-Protocol-Version") == mcp::protocol::kProtocolVersion);
    CHECK(call.header("Mcp-Method") == "tools/call");
    CHECK(call.header("Mcp-Name") == "post_message");
    CHECK(call.header("Mcp-Param-Note") == "base64:YQpi");
    CHECK(call.raw_head.find("MCP-Protocol-Version: 2026-07-28\r\n") != std::string::npos);
    CHECK(call.raw_head.find("Mcp-Param-Note: base64:YQpi\r\n") != std::string::npos);
    // The reserved `_meta` protocol version and the header protocol version are
    // one value, and the revision removed both `Mcp-Session-Id` and SSE
    // resumability, so neither may appear on the wire.
    CHECK(meta_protocol_version(call) == mcp::protocol::kProtocolVersion);
    CHECK_FALSE(call.has_header("Mcp-Session-Id"));
    CHECK_FALSE(call.has_header("Last-Event-ID"));
}

TEST_CASE("the Streamable HTTP transport refuses an untrusted MCP server certificate",
        "[mcp][transport][tls][issue837][spec]") {
    tests::LocalMcpHttpServer server(conforming_upstream());
    REQUIRE(server.ready());

    asio::io_context io;
    // The fixture presents the committed test certificate and this client is
    // given no extra trust anchor, so the handshake must fail rather than be
    // bypassed: peer and host-name verification are never relaxed.
    mcp::UpstreamClient client("local",
            std::make_shared<mcp::transport::BoostBeastStreamableHttpTransport>(io.get_executor()),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    auto outcome = drive_on(io, client.list_tools());
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Network);
    CHECK(outcome.error().message.find("TLS handshake") != std::string::npos);
    // The refusal happens before the request is written, so the Upstream never
    // saw it.
    CHECK(server.requests().empty());
}

TEST_CASE("the Streamable HTTP transport refuses a plain-HTTP MCP endpoint before any network work",
        "[mcp][transport][tls][issue837][issue638][spec]") {
    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = "http://127.0.0.1:1/mcp", .request_timeout = kExchangeBound});

    auto outcome = drive_on(io, client.list_tools());
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Validation);
    CHECK(outcome.error().detail.find("https") != std::string::npos);
}

TEST_CASE("a broken MCP response stream loses the in-flight request and is not replayed",
        "[mcp][transport][retry][issue837][spec]") {
    // A response whose last event-stream frame never gets its blank-line
    // terminator: the stream ends mid-frame, and the 2026-07-28 revision has
    // nothing to resume it with.
    tests::LocalMcpHttpServer server(
            script([](std::string_view method,
                           const tests::RecordedHttpRequest& request) -> std::optional<tests::McpServerReply> {
                if (method != mcp::protocol::kMethodCallTool || call_note(request) != "break this stream") {
                    return std::nullopt;
                }
                return tests::McpServerReply{
                        .content_type = "text/event-stream",
                        .as_event_stream = true,
                        .event_payloads = {framed(request, tests::tool_call_result(JsonValue::array_t{}))},
                        .end_stream_mid_frame = true,
                };
            }));
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    auto broken = drive_on(io,
            client.call_tool(mcp::UpstreamToolCall{
                    .tool = post_message_tool(),
                    .arguments = JsonValue::object_t{{"note", JsonValue("break this stream")}},
            }));
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == support::ErrorCode::Network);
    CHECK(broken.error().message.find("broke") != std::string::npos);
    // The failure names the class the retry policy read it as, which is what
    // tells a caller the exchange is lost rather than merely unsuccessful.
    CHECK(broken.error().detail.find(std::string{
                  mcp::transport::describe(mcp::transport::McpFailureClass::RequestDelivered)}) != std::string::npos);
    const auto after_broken = server.requests();
    REQUIRE(after_broken.size() == 2);
    const auto abandoned_id = recorded_id(after_broken.back());
    REQUIRE(abandoned_id.has_value());

    // The abandoned exchange is not replayed behind the caller's back, and the
    // caller's own retry is a new exchange carrying a new JSON-RPC id.
    auto retried = drive_on(io,
            client.call_tool(mcp::UpstreamToolCall{
                    .tool = post_message_tool(),
                    .arguments = JsonValue::object_t{{"note", JsonValue("and again")}},
            }));
    REQUIRE(retried.has_value());
    const auto after_retry = server.requests();
    REQUIRE(after_retry.size() == 3);
    const auto retry_id = recorded_id(after_retry.back());
    REQUIRE(retry_id.has_value());
    CHECK(*retry_id > *abandoned_id);
}

TEST_CASE("the Streamable HTTP transport re-attempts a request the Upstream never received",
        "[mcp][transport][retry][issue837][spec]") {
    tests::LocalMcpHttpServer server(conforming_upstream(),
            tests::LocalMcpHttpServerOptions{
                    .abort_first_handshake = true,
            });
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    auto catalog = drive_on(io, client.list_tools());
    REQUIRE(catalog.has_value());
    REQUIRE(catalog->tools.size() == 1);
    // The first connection was refused during the handshake, before the request
    // was written, so the re-attempt is the only exchange the Upstream saw.
    CHECK(server.requests().size() == 2);
}

TEST_CASE("an MCP output flood is bounded and leaves the next call and the connection release unaffected",
        "[mcp][transport][limits][issue837][spec]") {
    // Comfortably past the retention bound, so the flood is unambiguous.
    constexpr std::size_t kFloodBytes{64u * 1024u * 1024u};
    tests::LocalMcpHttpServer server(
            script([](std::string_view method,
                           const tests::RecordedHttpRequest& request) -> std::optional<tests::McpServerReply> {
                if (method != mcp::protocol::kMethodCallTool || call_note(request) != "flood") {
                    return std::nullopt;
                }
                return tests::McpServerReply{
                        .content_type = "text/event-stream",
                        .as_event_stream = true,
                        .event_payloads = {std::string(1024, 'a')},
                        .flood_bytes = kFloodBytes,
                };
            }));
    REQUIRE(server.ready());

    asio::io_context io;
    const auto started = std::chrono::steady_clock::now();
    {
        mcp::UpstreamClient client("local",
                trusted_transport(io),
                mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});
        auto flooded = drive_on(io,
                client.call_tool(mcp::UpstreamToolCall{
                        .tool = post_message_tool(),
                        .arguments = JsonValue::object_t{{"note", JsonValue("flood")}},
                }));
        REQUIRE_FALSE(flooded.has_value());
        CHECK(flooded.error().code == support::ErrorCode::ResourceLimit);

        // The flooding Upstream owns nothing between two calls, so the next
        // ordinary call on the same connection still succeeds.
        auto afterwards = drive_on(io,
                client.call_tool(mcp::UpstreamToolCall{
                        .tool = post_message_tool(),
                        .arguments = JsonValue::object_t{{"note", JsonValue("hello again")}},
                }));
        REQUIRE(afterwards.has_value());
        CHECK_FALSE(afterwards->is_error);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // The flood was terminated rather than drained, and releasing the client
    // left no stalled work behind for Close to wait on.
    io.restart();
    CHECK(io.run() == 0);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    (void)elapsed;
#else
    CHECK(elapsed < kExchangeBound);
#endif
}

TEST_CASE("the Streamable HTTP transport stops an in-flight exchange when the call is cancelled",
        "[mcp][transport][cancellation][issue837][spec]") {
    // A flooding reply is also a request that never finishes, which is what a
    // cancellation needs to interrupt; the fixture keeps the connection open
    // writing, so the exchange is in flight for as long as the case lets it be.
    tests::LocalMcpHttpServer server(
            script([](std::string_view method,
                           const tests::RecordedHttpRequest& request) -> std::optional<tests::McpServerReply> {
                if (method != mcp::protocol::kMethodCallTool || call_note(request) != "cancel me") {
                    return std::nullopt;
                }
                return tests::McpServerReply{
                        .content_type = "text/event-stream",
                        .as_event_stream = true,
                        .event_payloads = {std::string(1024, 'a')},
                        .flood_bytes = 64u * 1024u * 1024u,
                };
            }));
    REQUIRE(server.ready());

    asio::io_context io;
    mcp::UpstreamClient client("local",
            trusted_transport(io),
            mcp::UpstreamClientOptions{.url = server.url(), .request_timeout = kExchangeBound});

    // The era is probed before the case starts, so the cancellation below
    // lands on the `tools/call` exchange the case is about rather than on
    // whichever of the two the local server happened to be slower on.
    auto catalog = drive_on(io, client.list_tools());
    REQUIRE(catalog.has_value());
    const auto probes = server.requests().size();

    std::stop_source cancelled;
    auto pending = client.call_tool(
            mcp::UpstreamToolCall{
                    .tool = post_message_tool(),
                    .arguments = JsonValue::object_t{{"note", JsonValue("cancel me")}},
            },
            cancelled.get_token());
    // The cancellation is requested on the connection's own execution domain,
    // which is where the exchange runs, so the request is bound before it can
    // be stopped.
    asio::steady_timer cancel_at(io);
    cancel_at.expires_after(std::chrono::milliseconds{100});
    asio::co_spawn(
            io,
            [&cancel_at, &cancelled]() -> asio::awaitable<void> {
                boost::system::error_code error;
                co_await cancel_at.async_wait(asio::redirect_error(asio::use_awaitable, error));
                cancelled.request_stop();
            },
            asio::detached);

    auto outcome = drive_on(io, std::move(pending));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Cancelled);
    // Cancellation is the caller's decision, so the call is never re-attempted:
    // the Upstream saw exactly one `tools/call` on top of the era probe and the
    // catalog walk the case had already made.
    //
    // The stop is not silent, either. Closing the response stream releases the
    // socket and says nothing to the work behind it, so the client stack also
    // writes one `notifications/cancelled` naming the request it abandoned
    // (issue #844) — the 2026-07-28 revision has no resumability, so that
    // notification is the only signal that reaches the server-side operation.
    // Counting the two methods separately says precisely what the Upstream saw:
    // one call, never retried, and one request to stop it.
    std::size_t calls = 0;
    std::size_t cancellations = 0;
    for (const auto& request : server.requests()) {
        const auto method = request.header("Mcp-Method");
        calls += method == "tools/call" ? 1 : 0;
        cancellations += method == "notifications/cancelled" ? 1 : 0;
    }
    CHECK(calls == 1);
    CHECK(cancellations == 1);
    CHECK(server.requests().size() == probes + 2);
}

TEST_CASE("the MCP retry policy re-attempts only a request that was never delivered",
        "[mcp][transport][retry][issue837][spec]") {
    using mcp::transport::McpFailureClass;

    CHECK(mcp::transport::is_retryable(McpFailureClass::RequestNotDelivered));
    CHECK_FALSE(mcp::transport::is_retryable(McpFailureClass::RequestDelivered));
    CHECK_FALSE(mcp::transport::is_retryable(McpFailureClass::ResponseFlooded));
    CHECK_FALSE(mcp::transport::is_retryable(McpFailureClass::Cancelled));
    CHECK_FALSE(mcp::transport::is_retryable(McpFailureClass::Rejected));

    // The record the transport keeps is conservative: once it starts writing, a
    // network failure or a timeout is a delivered request, because a partial
    // write may still have been acted on.
    CHECK(mcp::transport::classify_failure(support::ErrorCode::Network, false) == McpFailureClass::RequestNotDelivered);
    CHECK(mcp::transport::classify_failure(support::ErrorCode::Network, true) == McpFailureClass::RequestDelivered);
    CHECK(mcp::transport::classify_failure(support::ErrorCode::Timeout, true) == McpFailureClass::RequestDelivered);
    CHECK(mcp::transport::classify_failure(support::ErrorCode::ResourceLimit, true) ==
            McpFailureClass::ResponseFlooded);
    CHECK(mcp::transport::classify_failure(support::ErrorCode::Cancelled, true) == McpFailureClass::Cancelled);
    CHECK(mcp::transport::classify_failure(support::ErrorCode::Validation, false) == McpFailureClass::Rejected);

    // Each class has its own declared sentence, so a failure can report the
    // class the policy read it as rather than only a code.
    CHECK(mcp::transport::describe(McpFailureClass::RequestNotDelivered) !=
            mcp::transport::describe(McpFailureClass::RequestDelivered));
    CHECK(mcp::transport::describe(McpFailureClass::Cancelled) !=
            mcp::transport::describe(McpFailureClass::ResponseFlooded));
}
