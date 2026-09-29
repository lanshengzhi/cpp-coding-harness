#include "mcp/transport/BoostBeastStreamableHttpTransport.hpp"

#include "mcp/HeaderMirror.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/SseResponseStream.hpp"
#include "mcp/transport/RetryPolicy.hpp"
#include "mcp/transport/TransportShared.hpp"
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/http.hpp>

#include <cstdint>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::transport {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;

using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;

/// One attempt's outcome, plus the record the retry policy classifies it by.
struct AttemptOutcome {
    Expected<McpResponse> result{};
    /// Whether the Upstream could have seen the request. It becomes true as
    /// soon as the transport starts writing, because a partial write may still
    /// have been acted on.
    bool request_delivered{false};
};

[[nodiscard]] Error cancelled_error() {
    return support::make_error(
            ErrorCode::Cancelled, "the MCP Streamable HTTP transport was cancelled", "the call was cancelled");
}

[[nodiscard]] Error network_error(std::string message, boost::system::error_code ec) {
    if (ec == asio::error::operation_aborted) {
        return cancelled_error();
    }
    return support::make_error(ErrorCode::Network, std::move(message), ec ? ec.message() : std::string{});
}

/// A failure during the phase the request timeout governs is a `Timeout` when
/// the timer produced it and a `Cancelled` when the caller's stop token did;
/// every other failure in that phase is a network failure.
[[nodiscard]] Error phase_error(bool timed_out, std::string message, boost::system::error_code ec) {
    if (timed_out) {
        return support::make_error(ErrorCode::Timeout, std::move(message), ec ? ec.message() : std::string{});
    }
    return network_error(std::move(message), ec);
}

template <typename Body>
[[nodiscard]] std::string_view header_value(const http::message<false, Body>& response, std::string_view field) {
    const auto found = response.base().find(field);
    return found == response.base().end() ? std::string_view{} : std::string_view{found->value()};
}

template <typename Body> [[nodiscard]] bool is_event_stream(const http::message<false, Body>& response) {
    const auto content_type = header_value(response, "Content-Type");
    return content_type.substr(0, protocol::kContentTypeEventStream.size()) == protocol::kContentTypeEventStream;
}

/// The retention bound, enforced on what this transport keeps. A response that
/// passes it is terminated rather than drained, so a flooding Upstream cannot
/// hold the connection open or stall another call.
[[nodiscard]] Error flooded_error() {
    return support::make_error(ErrorCode::ResourceLimit,
            "the Upstream MCP Server response passed the MCP Host's retention bound",
            "the response is terminated rather than drained; the partial body is released with the connection");
}

/// The failure the transport gives up with, carrying the class the retry
/// policy read it as. The class is part of the answer, not an internal detail:
/// `RequestDelivered` is what tells a caller that the exchange is lost and
/// must not be replayed, and it is the difference between a re-attempt this
/// transport makes and one it deliberately declines to.
[[nodiscard]] Error classified(Error error, McpFailureClass failure_class) {
    return support::make_error(error.code,
            std::move(error.message),
            error.detail.empty() ? std::string{describe(failure_class)}
                                 : std::move(error.detail) + " (" + std::string{describe(failure_class)} + ")");
}

/// The header set every Streamable HTTP POST carries, plus the values the
/// client stack resolved. No `Mcp-Session-Id` and no `Last-Event-ID` appear
/// here or anywhere else: the 2026-07-28 revision removed both (ADR 0064).
///
/// Every value is made header-safe on the way out, so a value that reached the
/// transport from anywhere other than the `Mcp-Param-*` mirroring still goes on
/// the wire legally. The sentinel is not re-derived here: the mirroring already
/// encoded anything the sentinel has to protect, and encoding it a second time
/// would double-encode a mirrored value.
[[nodiscard]] std::string header_wire_value(std::string_view value) {
    for (const char ch : value) {
        if (ch < ' ' || ch > '~') {
            return std::string(mcp::protocol::kHeaderValueBase64Sentinel) + headers::encode_base64(value);
        }
    }
    return std::string(value);
}

/// The one HTTP method the transport frames for a request that names another
/// than `POST`. The OAuth discovery documents are fetched, not posted; every
/// other method is refused rather than guessed at, because a verb this build
/// does not know is a verb whose body and response semantics it does not know
/// either.
[[nodiscard]] std::optional<http::verb> request_verb(std::string_view method) {
    if (method == "POST") {
        return http::verb::post;
    }
    if (method == "GET") {
        return http::verb::get;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<http::request<http::string_body>> frame_request(
        const McpRequest& request, const ParsedUrl& parsed) {
    const auto verb = request_verb(request.method);
    if (!verb.has_value()) {
        return std::nullopt;
    }
    http::request<http::string_body> framed{*verb, parsed.target, 11};
    framed.set(http::field::host, parsed.host);
    framed.set(http::field::user_agent, "cpp-coding-harness/0.1");
    framed.set(http::field::accept, "application/json, text/event-stream");
    if (!request.body.empty()) {
        framed.set(http::field::content_type, "application/json");
    }
    for (const auto& [name, value] : request.headers) {
        framed.set(name, header_wire_value(value));
    }
    framed.body() = request.body;
    framed.prepare_payload();
    return framed;
}

/// One attempt: connect, handshake, write, read. The whole body is retained
/// under the retention bound, either as one JSON body or as the assembled
/// payload of a `text/event-stream`.
[[nodiscard]] asio::awaitable<AttemptOutcome> attempt_exchange(
        McpRequest request, ParsedUrl parsed, const StreamableHttpTransportOptions& options) {
    AttemptOutcome outcome;
    const auto executor = transport_executor(co_await asio::this_coro::executor);
    if (request.stop_token.stop_requested()) {
        outcome.result = std::unexpected(cancelled_error());
        co_return outcome;
    }
    CancellationBridge cancellation(request.stop_token, executor);
    const auto cancellable = [&cancellation](
                                     auto completion_token) { return cancellation.bind(std::move(completion_token)); };

    // The request timeout governs setup, dispatch, the response headers, and
    // the wait for the first response byte. Past the first byte the response
    // stream belongs to the Upstream and is bounded by the retention bound and
    // the stop token instead, because an event stream may stay open while the
    // Upstream works.
    const auto timed_out = std::make_shared<bool>(false);
    TransportTimer deadline(executor, request.timeout);
    deadline.async_wait([timed_out, signal = cancellation.signal_ptr()](boost::system::error_code error) {
        if (!error) {
            *timed_out = true;
            signal->emit(asio::cancellation_type::all);
        }
    });

    ssl::context context(ssl::context::tls_client);
    if (auto ca = load_tls_client_ca(context, options.trusted_ca_certificate_pem); !ca) {
        outcome.result = std::unexpected(ca.error());
        co_return outcome;
    }
    TransportResolver resolver(executor);
    TransportTlsStream stream(executor, context);
    beast::get_lowest_layer(stream).expires_after(request.timeout);

    if (auto tls = configure_tls_client_stream(stream, parsed.host); !tls) {
        outcome.result = std::unexpected(tls.error());
        co_return outcome;
    }

    boost::system::error_code setup_error_code;
    auto results = co_await resolver.async_resolve(
            parsed.host, parsed.port, asio::redirect_error(cancellable(asio::use_awaitable), setup_error_code));
    if (setup_error_code) {
        outcome.result = std::unexpected(phase_error(*timed_out, "MCP connection setup failure", setup_error_code));
        co_return outcome;
    }
    co_await beast::get_lowest_layer(stream).async_connect(
            results, asio::redirect_error(cancellable(asio::use_awaitable), setup_error_code));
    if (setup_error_code) {
        outcome.result = std::unexpected(phase_error(*timed_out, "MCP connection setup failure", setup_error_code));
        co_return outcome;
    }
    co_await stream.async_handshake(
            ssl::stream_base::client, asio::redirect_error(cancellable(asio::use_awaitable), setup_error_code));
    if (setup_error_code) {
        outcome.result = std::unexpected(
                phase_error(*timed_out, "the Upstream MCP Server TLS handshake failed", setup_error_code));
        co_return outcome;
    }
    // The socket-level deadline is spent; the transport timer above governs
    // the rest of the governed phase.
    beast::get_lowest_layer(stream).expires_never();

    auto framed = frame_request(request, parsed);
    if (!framed.has_value()) {
        outcome.result = std::unexpected(support::make_error(support::ErrorCode::Validation,
                "the MCP Host framed an HTTP method this transport does not send",
                "method \"" + request.method + "\"; this transport sends POST and GET only"));
        co_return outcome;
    }
    outcome.request_delivered = true;
    co_await http::async_write(
            stream, *framed, asio::redirect_error(cancellable(asio::use_awaitable), setup_error_code));
    if (setup_error_code) {
        outcome.result =
                std::unexpected(phase_error(*timed_out, "the MCP request could not be dispatched", setup_error_code));
        co_return outcome;
    }

    // The response body is retained under the bound as it arrives. A chunked
    // body is diverted into the accumulator by the parser's chunk callback so
    // an event stream never has to be held twice; an identity body is bounded
    // by the parser's own body limit. The accumulator and its callback are
    // declared before the parser, because the parser keeps a reference to the
    // callback and the callback writes into the accumulator.
    std::string streamed;
    auto retain_chunk =
            [&streamed](std::uint64_t, std::string_view piece, boost::system::error_code& error) -> std::size_t {
        if (streamed.size() + piece.size() > protocol::kMaxResponseBytes) {
            error = http::error::body_limit;
            return 0;
        }
        streamed.append(piece);
        return piece.size();
    };

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(protocol::kMaxResponseBytes);
    parser.on_chunk_body(retain_chunk);
    co_await http::async_read_header(
            stream, buffer, parser, asio::redirect_error(cancellable(asio::use_awaitable), setup_error_code));
    if (setup_error_code == http::error::body_limit) {
        outcome.result = std::unexpected(flooded_error());
        co_return outcome;
    }
    if (setup_error_code) {
        outcome.result = std::unexpected(
                phase_error(*timed_out, "the Upstream MCP Server sent no response headers", setup_error_code));
        co_return outcome;
    }

    McpResponse response;
    response.status_code = static_cast<int>(parser.get().result_int());
    for (const auto& field : parser.get().base()) {
        response.headers[std::string(field.name_string())] = std::string(field.value());
    }
    // The request timeout governed setup, dispatch, and the response headers. A
    // response stream belongs to the Upstream past that point — an Upstream may
    // hold an event stream open while it works — so what bounds the rest is the
    // retention bound and the caller's stop token.
    deadline.cancel();

    boost::system::error_code read_error;
    co_await http::async_read(
            stream, buffer, parser, asio::redirect_error(cancellable(asio::use_awaitable), read_error));
    if (read_error == http::error::body_limit) {
        outcome.result = std::unexpected(flooded_error());
        co_return outcome;
    }
    if (read_error) {
        outcome.result = std::unexpected(network_error("the Upstream MCP Server response stream broke", read_error));
        co_return outcome;
    }

    const bool streaming = is_event_stream(parser.get());
    if (streaming) {
        sse::SseResponseStream event_stream{protocol::kMaxResponseBytes};
        if (auto appended = event_stream.append(streamed); !appended) {
            outcome.result = std::unexpected(appended.error());
            co_return outcome;
        }
        auto assembled = event_stream.finish();
        if (!assembled) {
            outcome.result = std::unexpected(assembled.error());
            co_return outcome;
        }
        response.body = std::move(*assembled);
    } else {
        response.body = std::move(parser.get().body());
    }

    // The response is complete, so the exchange has its answer; a TLS shutdown
    // that fails while the socket is released changes nothing about it.
    boost::system::error_code shutdown_error;
    co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, shutdown_error));
    if (request.stop_token.stop_requested()) {
        outcome.result = std::unexpected(cancelled_error());
        co_return outcome;
    }
    outcome.result = std::move(response);
    co_return outcome;
}

/// The whole exchange: the URL is checked once, then the attempt loop
/// applies the declared per-class retry policy.
[[nodiscard]] asio::awaitable<Expected<McpResponse>> run_exchange(
        McpRequest request, StreamableHttpTransportOptions options) {
    auto parsed = parse_mcp_url(request.url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (request.stop_token.stop_requested()) {
        co_return std::unexpected(cancelled_error());
    }
    const auto attempts = options.max_attempts == 0 ? std::size_t{1} : options.max_attempts;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        auto outcome = co_await attempt_exchange(request, *parsed, options);
        if (outcome.result) {
            co_return std::move(outcome.result);
        }
        if (request.stop_token.stop_requested()) {
            co_return std::unexpected(cancelled_error());
        }
        const auto failure_class = classify_failure(outcome.result.error().code, outcome.request_delivered);
        if (!is_retryable(failure_class) || attempt + 1 == attempts) {
            co_return std::unexpected(classified(outcome.result.error(), failure_class));
        }
    }
    co_return std::unexpected(support::make_error(
            ErrorCode::Busy, "the MCP Streamable HTTP exchange made no attempt", "the attempt budget was empty"));
}

} // namespace

BoostBeastStreamableHttpTransport::BoostBeastStreamableHttpTransport(
        boost::asio::any_io_executor executor, StreamableHttpTransportOptions options)
    : executor_(std::move(executor)), options_(std::move(options)) {}

BoostBeastStreamableHttpTransport::BoostBeastStreamableHttpTransport(StreamableHttpTransportOptions options)
    : options_(std::move(options)) {}

AsyncResult<McpResponse> BoostBeastStreamableHttpTransport::send(McpRequest request) {
    if (!executor_.has_value()) {
        // A late-bound transport takes the domain of the exchange that
        // initiates it (the `McpTransport` executor contract), and keeps it:
        // one connection's operations share one serialized executor.
        executor_ = support::detail::t_initiating_executor;
    }
    if (!executor_.has_value() || !*executor_) {
        return AsyncResult<McpResponse>(std::unexpected(make_error(ErrorCode::Busy,
                "the MCP Streamable HTTP transport was not driven from a serialized execution domain",
                "an unbound transport needs a caller that initiates its exchanges on one domain")));
    }
    return support::detail::make_async_result_on(
            *executor_, [request = std::move(request), options = options_]() -> asio::awaitable<Expected<McpResponse>> {
                co_return co_await run_exchange(std::move(request), options);
            });
}

} // namespace cch::mcp::transport

namespace cch::mcp {

std::shared_ptr<McpTransport> make_streamable_http_transport() {
    return std::make_shared<transport::BoostBeastStreamableHttpTransport>(transport::StreamableHttpTransportOptions{});
}

} // namespace cch::mcp
