// MCP streamable-http transport client (spec #865, ticket #873). The protocol
// shape mirrors pi v1.0.4 `packages/mcp/src/transports/streamable-http.ts` for
// the request path: one JSON-RPC POST per request with
// `accept: application/json, text/event-stream`, an `application/json` or
// `text/event-stream` response, and an `mcp-session-id` captured from the
// server and echoed on later requests. The HTTPS round trip itself reuses the
// existing outbound client transport (`ai::providers::StreamTransport`, ADR
// 0054); this file owns only the MCP protocol and opens no second HTTP stack.
//
// The transport is private to cch_coding_agent and is reached only through
// `McpExtensionToolSource`; nothing here is an Owner Interface.

#include "coding_agent/mcp/McpHttpClient.hpp"

#include "coding_agent/mcp/McpProtocol.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include "ai/providers/SseParser.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

/// pi `client.ts` request timeout default (30 s); a request that gets no
/// response by then fails with a timeout instead of hanging the queue.
constexpr std::chrono::milliseconds kRequestTimeout{30000};
/// The error-body excerpt carried in a non-2xx diagnostic, matching pi
/// `ERROR_MESSAGE_BODY_CHARS` (500).
constexpr std::size_t kErrorBodyChars = 500;

[[nodiscard]] support::Error transport_error(std::string server, std::string message, std::string cause = {}) {
    std::string summary = "MCP server '" + std::move(server) + "' " + std::move(message);
    std::string detail = summary;
    if (!cause.empty()) {
        detail += ": " + std::move(cause);
    }
    return support::make_error(support::ErrorCode::Process, std::move(summary), std::move(detail));
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string result{text};
    std::ranges::transform(
            result, result.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return result;
}

/// Case-insensitive header lookup; Beast normalizes response field names to
/// lowercase, but the lookup does not rely on it.
[[nodiscard]] std::string header_value(const std::map<std::string, std::string>& headers, std::string_view name) {
    const std::string wanted = lowercase(name);
    for (const auto& [key, value] : headers) {
        if (lowercase(key) == wanted) {
            return value;
        }
    }
    return {};
}

/// The media type of a response, lowercased and stripped of its parameters.
[[nodiscard]] std::string normalized_content_type(const std::map<std::string, std::string>& headers) {
    std::string type = header_value(headers, "content-type");
    const auto semicolon = type.find(';');
    if (semicolon != std::string::npos) {
        type.erase(semicolon);
    }
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.pop_back();
    }
    return lowercase(type);
}

[[nodiscard]] bool has_non_whitespace(std::string_view text) {
    return std::ranges::any_of(text, [](unsigned char character) { return std::isspace(character) == 0; });
}

/// Non-2xx (and redirect) status diagnostics. A redirect is never followed:
/// streamable-http is TLS-only (ADR 0054), so following `Location` could
/// downgrade the connection to plaintext.
[[nodiscard]] support::Error http_status_error(
        const std::string& server, const ai::providers::StreamResponse& response) {
    const int status = response.head.status_code;
    if (status >= 300 && status < 400) {
        std::string detail = "MCP streamable-http is TLS-only (ADR 0054) and never follows a redirect";
        if (const std::string location = header_value(response.head.headers, "location"); !location.empty()) {
            detail += "; Location: " + location;
        }
        return support::make_error(support::ErrorCode::Process,
                "MCP server '" + server + "' refused an HTTP redirect (" + std::to_string(status) + ")",
                std::move(detail));
    }
    std::string body = response.body;
    if (body.size() > kErrorBodyChars) {
        body.resize(kErrorBodyChars);
        body += "...";
    }
    return support::make_error(support::ErrorCode::Process,
            "MCP server '" + server + "' returned HTTP status " + std::to_string(status),
            std::move(body));
}

/// One response body that is `application/json` (a single message or a batch)
/// must contain the JSON-RPC reply for `id`.
[[nodiscard]] support::Expected<support::JsonValue> response_from_json_body(
        const std::string& server, int id, const std::string& body) {
    auto parsed = support::read_json(body);
    if (!parsed) {
        return std::unexpected(transport_error(server, "sent a response body that is not JSON", body));
    }
    const auto match_one =
            [id](const support::JsonValue& message) -> support::Expected<std::optional<support::JsonValue>> {
        return detail::response_for_id(message, id);
    };
    if (const auto* batch = parsed->get_if<support::JsonValue::array_t>()) {
        for (const auto& message : *batch) {
            auto matched = match_one(message);
            if (!matched) {
                return std::unexpected(std::move(matched.error()));
            }
            if (matched->has_value()) {
                return std::move(**matched);
            }
        }
    } else {
        auto matched = match_one(*parsed);
        if (!matched) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched->has_value()) {
            return std::move(**matched);
        }
    }
    return std::unexpected(
            transport_error(server, "did not return a JSON-RPC response for the request", "no matching id"));
}

/// One `text/event-stream` body: each `message` event's `data` is a JSON-RPC
/// message; the first reply for `id` wins, mirroring pi's response-stream
/// consumption (`consumeResponseStream`).
[[nodiscard]] support::Expected<support::JsonValue> response_from_sse_body(
        const std::string& server, int id, const std::string& body) {
    ai::providers::SseParser parser;
    std::vector<ai::providers::SseEvent> events;
    if (auto appended = parser.append(body); !appended) {
        return std::unexpected(transport_error(server, "sent an unparsable event stream", appended.error().detail));
    } else {
        events = std::move(*appended);
    }
    if (auto final = parser.finish(); !final) {
        return std::unexpected(transport_error(server, "sent an unparsable event stream", final.error().detail));
    } else if (*final) {
        events.push_back(std::move(**final));
    }

    for (const auto& event : events) {
        // Events without data prime resumption; non-`message` events are not
        // JSON-RPC responses (pi `consumeSse`).
        if (event.event != "message" || !has_non_whitespace(event.data)) {
            continue;
        }
        auto message = support::read_json(event.data);
        if (!message) {
            continue;
        }
        auto matched = detail::response_for_id(*message, id);
        if (!matched) {
            return std::unexpected(std::move(matched.error()));
        }
        if (matched->has_value()) {
            return std::move(**matched);
        }
    }
    return std::unexpected(transport_error(server, "event stream ended without a JSON-RPC response", "no matching id"));
}

} // namespace

McpHttpClient::McpHttpClient(boost::asio::any_io_executor executor,
        McpHttpServerConfig config,
        std::shared_ptr<ai::providers::StreamTransport> transport)
    : executor_(std::move(executor)), config_(std::move(config)), transport_(std::move(transport)) {}

boost::asio::awaitable<support::Expected<std::shared_ptr<McpHttpClient>>> McpHttpClient::connect(
        McpHttpServerConfig config, std::shared_ptr<ai::providers::StreamTransport> transport) {
    if (auto valid = validate_mcp_http_server_config(config); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (transport == nullptr) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "MCP server '" + config.name + "' has no HTTP transport"));
    }
    auto executor = co_await boost::asio::this_coro::executor;
    const std::string server_name = config.name;
    auto client = std::shared_ptr<McpHttpClient>(
            new McpHttpClient(std::move(executor), std::move(config), std::move(transport)));

    support::JsonValue params{support::JsonValue::object_t{
            {"protocolVersion", std::string{kMcpProtocolVersion}},
            {"capabilities", support::JsonValue::object_t{}},
            {"clientInfo",
                    support::JsonValue::object_t{
                            {"name", std::string{kMcpClientName}},
                            {"version", std::string{kMcpClientVersion}},
                    }},
    }};
    auto initialized = co_await support::detail::await_async_result(client->request("initialize", std::move(params)));
    if (!initialized) {
        co_return std::unexpected(std::move(initialized.error()));
    }
    if (auto valid = detail::validate_initialize_result(server_name, *initialized); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (const auto* object = initialized->get_if<support::JsonValue::object_t>()) {
        if (const auto version = object->find("protocolVersion");
                version != object->end() && version->second.holds<std::string>()) {
            client->protocol_version_ = version->second.get_string();
        }
    }
    // pi opens the server-to-client GET stream only after this notification;
    // the stream carries server-initiated notifications, which this slice does
    // not consume, so only the notification is sent.
    client->notify("notifications/initialized");
    co_return client;
}

support::AsyncResult<support::JsonValue> McpHttpClient::request(
        std::string method, std::optional<support::JsonValue> params, std::stop_token stop_token) {
    return support::AsyncResult<support::JsonValue>{support::AsyncProducer<support::JsonValue, support::Error>{
            [self = shared_from_this(), method = std::move(method), params = std::move(params), stop_token](
                    support::AsyncCompletion<support::JsonValue, support::Error> completion) mutable noexcept {
                if (stop_token.stop_requested()) {
                    completion(std::unexpected(detail::cancelled_error(self->config_.name)));
                    return;
                }
                const int id = self->next_id_++;
                self->enqueue_frame(
                        detail::build_request_body(id, method, params), id, std::move(completion), stop_token);
            }}};
}

void McpHttpClient::notify(std::string method, std::optional<support::JsonValue> params) {
    enqueue_frame(detail::build_notification_body(method, params), 0, std::nullopt);
}

void McpHttpClient::enqueue_frame(std::string frame,
        int id,
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion,
        std::stop_token stop_token) {
    auto item = std::make_unique<QueuedFrame>();
    item->frame = std::move(frame);
    item->id = id;
    item->completion = std::move(completion);
    item->is_notification = !item->completion.has_value();
    item->stop_token = stop_token;
    queue_.push_back(std::move(item));
    if (!pumping_) {
        pumping_ = true;
        // Capture the shared owner in the coroutine frame at creation, so the
        // client outlives the pump even if every external reference drops.
        boost::asio::co_spawn(
                executor_,
                [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->pump(); },
                boost::asio::detached);
    }
}

boost::asio::awaitable<void> McpHttpClient::pump() {
    while (!queue_.empty()) {
        auto item = std::move(queue_.front());
        queue_.pop_front();
        if (item->is_notification) {
            // A notification has no response to route; a failed POST is
            // dropped without failing the queue, matching pi's notification
            // send (best-effort, no request waits on it).
            (void)co_await send_notification(item->frame);
            continue;
        }
        complete_frame(*item, co_await send_request(item->frame, item->id, item->stop_token));
    }
    pumping_ = false;
}

void McpHttpClient::complete_frame(QueuedFrame& frame, support::Expected<support::JsonValue> outcome) {
    if (frame.completion.has_value()) {
        (*frame.completion)(std::move(outcome));
    }
}

boost::asio::awaitable<support::Expected<ai::providers::StreamResponse>> McpHttpClient::post(
        std::string_view body, std::stop_token stop_token) {
    ai::providers::StreamRequest request;
    request.method = "POST";
    request.url = config_.url;
    request.headers = request_headers();
    request.body = std::string{body};
    request.timeout = kRequestTimeout;
    // The reused transport resolves the token into its own Cancelled error, so
    // an aborted Agent Turn cancels the HTTPS request instead of waiting out
    // the deadline.
    request.stop_token = stop_token;
    // An empty body handler buffers the whole response body, which is enough
    // for the request path: a JSON reply, or the complete SSE response stream.
    co_return co_await transport_->async_stream(request, {});
}

boost::asio::awaitable<support::Expected<support::JsonValue>> McpHttpClient::send_request(
        std::string_view frame, int id, std::stop_token stop_token) {
    auto response = co_await post(frame, stop_token);
    if (!response) {
        co_return std::unexpected(std::move(response.error()));
    }
    capture_session(*response);
    co_return interpret_response(id, *response);
}

boost::asio::awaitable<std::optional<support::Error>> McpHttpClient::send_notification(std::string_view frame) {
    auto response = co_await post(frame, {});
    if (!response) {
        co_return std::move(response.error());
    }
    capture_session(*response);
    const int status = response->head.status_code;
    if (status < 200 || status >= 300) {
        co_return http_status_error(config_.name, *response);
    }
    co_return std::nullopt;
}

std::map<std::string, std::string> McpHttpClient::request_headers() const {
    std::map<std::string, std::string> headers = config_.headers;
    headers["content-type"] = "application/json";
    headers["accept"] = "application/json, text/event-stream";
    if (!session_id_.empty()) {
        headers["mcp-session-id"] = session_id_;
    }
    if (!protocol_version_.empty()) {
        headers["mcp-protocol-version"] = protocol_version_;
    }
    return headers;
}

void McpHttpClient::capture_session(const ai::providers::StreamResponse& response) {
    if (std::string session = header_value(response.head.headers, "mcp-session-id"); !session.empty()) {
        session_id_ = std::move(session);
    }
}

support::Expected<support::JsonValue> McpHttpClient::interpret_response(
        int id, const ai::providers::StreamResponse& response) const {
    const int status = response.head.status_code;
    if (status < 200 || status >= 300) {
        return std::unexpected(http_status_error(config_.name, response));
    }
    if (status == 202 || status == 204) {
        return std::unexpected(transport_error(
                config_.name, "accepted a request without a response", "HTTP " + std::to_string(status)));
    }
    const std::string type = normalized_content_type(response.head.headers);
    if (type == "application/json") {
        return response_from_json_body(config_.name, id, response.body);
    }
    if (type == "text/event-stream") {
        return response_from_sse_body(config_.name, id, response.body);
    }
    return std::unexpected(transport_error(
            config_.name, "sent an unsupported response content type", type.empty() ? "missing content-type" : type));
}

} // namespace cch::coding_agent::mcp
