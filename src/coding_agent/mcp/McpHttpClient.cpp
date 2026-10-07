// MCP streamable-http transport client (spec #865, ticket #873; server-to-client
// GET stream, spec #882 ticket #884). The protocol shape mirrors pi v1.0.4
// `packages/mcp/src/transports/streamable-http.ts`: one JSON-RPC POST per
// request with `accept: application/json, text/event-stream`, an
// `application/json` or `text/event-stream` response, and an `mcp-session-id`
// captured from the server and echoed on later requests, plus the long-lived
// server-to-client GET stream (pi `runGetStream`) opened after
// `notifications/initialized`. The HTTPS round trip itself reuses the existing
// outbound client transport (`ai::providers::StreamTransport`, ADR 0054); this
// file owns only the MCP protocol and opens no second HTTP stack.
//
// The transport is private to cch_coding_agent and is reached only through
// `McpExtensionToolSource`; nothing here is an Owner Interface.

#include "coding_agent/mcp/McpHttpClient.hpp"

#include "coding_agent/mcp/McpOAuthProvider.hpp"
#include "coding_agent/mcp/McpProtocol.hpp"

#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include "ai/providers/SseParser.hpp"
#include "support/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {
namespace {

using detail::transport_error;

/// The error-body excerpt carried in a non-2xx diagnostic, matching pi
/// `ERROR_MESSAGE_BODY_CHARS` (500).
constexpr std::size_t kErrorBodyChars = 500;

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
    if (status == 401) {
        // A rejected credential is an explicit re-login error (never a silent
        // unauthenticated retry), reusing the shared `/login` guidance the
        // Models auth path produces.
        return support::make_error(support::ErrorCode::OAuth,
                format_oauth_reauthenticate_message(mcp_oauth_provider_id(server)),
                "MCP server '" + server + "' rejected the request (HTTP 401)");
    }
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

/// One dispatched SSE event of the server-to-client GET stream: pi
/// `consumeSseStream`'s `{event, data}` shape.
struct GetStreamEvent {
    std::string event{"message"};
    std::string data;
};

/// Incremental SSE line reader for the server-to-client GET stream (pi
/// `consumeSseStream`): `event`/`data` fields build the event, `id` and
/// `retry` are recorded per field (including priming events with no data, which
/// are not dispatched), `:` comment lines are skipped, and a blank line
/// dispatches. The provider `SseParser` carries neither `id` nor `retry`, so
/// the GET stream reads its own lines.
class GetStreamSseReader {
public:
    [[nodiscard]] support::Expected<std::vector<GetStreamEvent>> append(std::string_view bytes) {
        pending_.append(bytes.data(), bytes.size());
        std::vector<GetStreamEvent> events;
        for (;;) {
            const auto newline = pending_.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            consume_line(std::move(line), events);
        }
        return events;
    }

    /// Flush a trailing line without a newline and dispatch a pending event, as
    /// pi does when the stream closes.
    [[nodiscard]] std::vector<GetStreamEvent> finish() {
        std::vector<GetStreamEvent> events;
        if (!pending_.empty()) {
            std::string line = std::move(pending_);
            pending_.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            consume_line(std::move(line), events);
        }
        if (auto event = dispatch()) {
            events.push_back(std::move(*event));
        }
        return events;
    }

    [[nodiscard]] const std::optional<std::string>& last_event_id() const noexcept { return last_event_id_; }
    [[nodiscard]] const std::optional<int>& retry_ms() const noexcept { return retry_ms_; }

private:
    void consume_line(std::string line, std::vector<GetStreamEvent>& events) {
        if (line.empty()) {
            if (auto event = dispatch()) {
                events.push_back(std::move(*event));
            }
            return;
        }
        if (line.front() == ':') {
            return;
        }
        const auto colon = line.find(':');
        const std::string_view field =
                colon == std::string::npos ? std::string_view{line} : std::string_view{line}.substr(0, colon);
        std::string_view value =
                colon == std::string::npos ? std::string_view{} : std::string_view{line}.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        if (field == "event") {
            event_name_ = std::string{value};
        } else if (field == "data") {
            data_lines_.emplace_back(value);
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) {
                last_event_id_ = std::string{value};
            }
        } else if (field == "retry") {
            if (!value.empty() &&
                    std::ranges::all_of(value, [](unsigned char character) { return std::isdigit(character) != 0; })) {
                retry_ms_ = std::stoi(std::string{value});
            }
        }
    }

    [[nodiscard]] std::optional<GetStreamEvent> dispatch() {
        if (data_lines_.empty()) {
            event_name_.clear();
            return std::nullopt;
        }
        GetStreamEvent event;
        event.event = event_name_.empty() ? "message" : event_name_;
        for (std::size_t index = 0; index < data_lines_.size(); ++index) {
            if (index != 0) {
                event.data.push_back('\n');
            }
            event.data += data_lines_[index];
        }
        event_name_.clear();
        data_lines_.clear();
        return event;
    }

    std::string pending_;
    std::string event_name_;
    std::vector<std::string> data_lines_;
    std::optional<std::string> last_event_id_;
    std::optional<int> retry_ms_;
};

} // namespace

McpHttpClient::McpHttpClient(ConstructionKey,
        boost::asio::any_io_executor executor,
        McpHttpServerConfig config,
        std::shared_ptr<ai::providers::StreamTransport> transport,
        std::shared_ptr<McpRequestAuthSource> request_auth,
        McpHttpGetStreamOptions get_stream_options)
    : executor_(std::move(executor)), config_(std::move(config)), transport_(std::move(transport)),
      request_auth_(std::move(request_auth)), get_stream_options_(std::move(get_stream_options)) {
    // pi `client.ts` installs `ping` by default; every other server request is
    // answered `-32601` until a handler is registered.
    request_handlers_["ping"] =
            [](const support::JsonValue&,
                    std::stop_token) -> boost::asio::awaitable<support::Expected<support::JsonValue>> {
        co_return support::JsonValue{support::JsonValue::object_t{}};
    };
}

boost::asio::awaitable<support::Expected<std::shared_ptr<McpHttpClient>>> McpHttpClient::connect(
        McpHttpServerConfig config,
        std::shared_ptr<ai::providers::StreamTransport> transport,
        std::shared_ptr<McpRequestAuthSource> request_auth,
        McpHttpGetStreamOptions get_stream_options) {
    if (auto valid = validate_mcp_http_server_config(config); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    if (transport == nullptr) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "MCP server '" + config.name + "' has no HTTP transport"));
    }
    auto executor = co_await boost::asio::this_coro::executor;
    const std::string server_name = config.name;
    auto client = std::make_shared<McpHttpClient>(ConstructionKey{},
            std::move(executor),
            std::move(config),
            std::move(transport),
            std::move(request_auth),
            std::move(get_stream_options));

    auto initialized =
            co_await support::detail::await_async_result(client->request("initialize", detail::initialize_params()));
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
    client->server_instructions_ = detail::initialize_instructions(*initialized);
    // pi sends `notifications/initialized` and only then opens the
    // server-to-client GET stream (`send` awaits the POST, then
    // `startGetStream`). The stream carries server-initiated requests and
    // notifications; a server without it answers 405 and the stream is absent.
    if (auto error = co_await client->send_notification(
                detail::build_notification_body("notifications/initialized", std::nullopt));
            error) {
        co_return std::unexpected(std::move(*error));
    }
    client->start_get_stream();
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
                self->enqueue_frame(detail::build_request_body(id, method, params),
                        id,
                        std::move(completion),
                        stop_token,
                        /* cancellable */ method != "initialize");
            }}};
}

void McpHttpClient::notify(std::string method, std::optional<support::JsonValue> params) {
    enqueue_frame(detail::build_notification_body(method, params), 0, std::nullopt);
}

void McpHttpClient::enqueue_frame(std::string frame,
        int id,
        std::optional<support::AsyncCompletion<support::JsonValue, support::Error>> completion,
        std::stop_token stop_token,
        bool cancellable) {
    auto item = std::make_unique<QueuedFrame>();
    item->frame = std::move(frame);
    item->id = id;
    item->completion = std::move(completion);
    item->is_notification = !item->completion.has_value();
    item->stop_token = stop_token;
    item->cancellable = cancellable;
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
        complete_frame(*item, co_await send_request(item->frame, item->id, item->stop_token, item->cancellable));
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
    CCH_TRY(headers, co_await request_headers_for_call());
    ai::providers::StreamRequest request;
    request.method = "POST";
    request.url = config_.url;
    request.headers = std::move(headers);
    request.body = std::string{body};
    request.timeout = config_.request_timeout;
    // The reused transport resolves the token into its own Cancelled error, so
    // an aborted Agent Turn cancels the HTTPS request instead of waiting out
    // the deadline.
    request.stop_token = stop_token;
    // An empty body handler buffers the whole response body, which is enough
    // for the request path: a JSON reply, or the complete SSE response stream.
    co_return co_await transport_->async_stream(request, {});
}

boost::asio::awaitable<support::Expected<support::JsonValue>> McpHttpClient::send_request(
        std::string_view frame, int id, std::stop_token stop_token, bool cancellable) {
    auto response = co_await post(frame, stop_token);
    if (!response) {
        // pi `cancelPending`: a cancellable request that is aborted or times
        // out tells the server through `notifications/cancelled` (with the pi
        // reason) before the call fails. `initialize` is never cancellable.
        const support::ErrorCode code = response.error().code;
        if (cancellable && (code == support::ErrorCode::Cancelled || code == support::ErrorCode::Timeout)) {
            co_await notify_cancelled(id, code == support::ErrorCode::Timeout ? "Request timed out" : "Aborted");
        }
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

boost::asio::awaitable<void> McpHttpClient::notify_cancelled(int id, std::string reason) {
    support::JsonValue params{support::JsonValue::object_t{
            {"requestId", static_cast<double>(id)},
            {"reason", std::move(reason)},
    }};
    // Best-effort: the notification is not tied to the caller's stop token (pi
    // `cancelPending` ignores the send's outcome), so a cancellation still
    // reaches the server after the in-flight request was aborted.
    (void)co_await send_notification(detail::build_notification_body("notifications/cancelled", params));
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

boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> McpHttpClient::resolve_headers(
        std::map<std::string, std::string> headers) {
    if (request_auth_ == nullptr) {
        co_return headers;
    }
    // Resolve the credential for this call only: a rotated or newly signed-in
    // token is observed without restarting the connection, and a failure is the
    // request's explicit error (never an unauthenticated send).
    auto resolved = co_await support::detail::await_async_result(request_auth_->current_headers());
    if (!resolved) {
        co_return std::unexpected(std::move(resolved.error()));
    }
    for (auto& [name, value] : *resolved) {
        headers.insert_or_assign(std::move(name), std::move(value));
    }
    co_return headers;
}

boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>>
McpHttpClient::request_headers_for_call() {
    co_return co_await resolve_headers(request_headers());
}

void McpHttpClient::set_notification_listener(NotificationListener listener) {
    notification_listener_ = std::move(listener);
}

void McpHttpClient::set_error_listener(ErrorListener listener) { error_listener_ = std::move(listener); }

void McpHttpClient::set_request_handler(std::string method, ServerRequestHandler handler) {
    request_handlers_.insert_or_assign(std::move(method), std::move(handler));
}

void McpHttpClient::post_message(support::JsonValue message) {
    auto serialized = support::write_json(message);
    if (!serialized) {
        return;
    }
    // A JSON-RPC response is a raw frame with no completion; the pump's
    // notification path POSTs it and ignores the reply body (pi `transport.send`
    // of a response message).
    enqueue_frame(std::move(*serialized), 0, std::nullopt);
}

boost::asio::awaitable<void> McpHttpClient::serve_server_request(
        support::JsonValue id, std::string method, support::JsonValue params) {
    const auto handler = request_handlers_.find(method);
    if (handler == request_handlers_.end()) {
        support::JsonValue response{support::JsonValue::object_t{
                {"jsonrpc", "2.0"},
                {"id", std::move(id)},
                {"error",
                        support::JsonValue::object_t{
                                {"code", static_cast<double>(-32601)},
                                {"message", "Method not found: " + method},
                        }},
        }};
        post_message(std::move(response));
        co_return;
    }
    const ServerRequestHandler serve = handler->second;
    const std::string key = detail::json_or_empty(id);
    std::stop_source source;
    incoming_handlers_.insert_or_assign(key, source);
    support::Expected<support::JsonValue> outcome = co_await serve(params, source.get_token());
    incoming_handlers_.erase(key);
    if (outcome) {
        post_message(support::JsonValue{support::JsonValue::object_t{
                {"jsonrpc", "2.0"},
                {"id", std::move(id)},
                {"result", std::move(*outcome)},
        }});
        co_return;
    }
    const std::string message = source.get_token().stop_requested() ? "MCP request aborted" : outcome.error().message;
    post_message(support::JsonValue{support::JsonValue::object_t{
            {"jsonrpc", "2.0"},
            {"id", std::move(id)},
            {"error",
                    support::JsonValue::object_t{
                            {"code", static_cast<double>(-32603)},
                            {"message", std::move(message)},
                    }},
    }});
}

void McpHttpClient::handle_stream_message(const support::JsonValue& message) {
    const auto* object = message.get_if<support::JsonValue::object_t>();
    if (object == nullptr) {
        return;
    }
    const auto method = object->find("method");
    if (method == object->end() || !method->second.holds<std::string>()) {
        // A JSON-RPC response on the server-to-client stream answers no request
        // this client made; pi reports it and continues.
        return;
    }
    const std::string name = method->second.get_string();
    support::JsonValue params{support::JsonValue::object_t{}};
    if (const auto found = object->find("params"); found != object->end()) {
        params = found->second;
    }
    const auto id = object->find("id");
    if (id != object->end() && (id->second.holds<double>() || id->second.holds<std::string>())) {
        // A server-to-client request (pi `handleRequest`).
        boost::asio::co_spawn(
                executor_,
                [self = shared_from_this(), id = id->second, name, params]() mutable -> boost::asio::awaitable<void> {
                    co_await self->serve_server_request(std::move(id), std::move(name), std::move(params));
                },
                boost::asio::detached);
        return;
    }
    // A server-to-client notification (pi `handleNotification`).
    if (name == "notifications/cancelled") {
        if (const auto* params_object = params.get_if<support::JsonValue::object_t>()) {
            if (const auto request_id = params_object->find("requestId"); request_id != params_object->end()) {
                const auto handler = incoming_handlers_.find(detail::json_or_empty(request_id->second));
                if (handler != incoming_handlers_.end()) {
                    handler->second.request_stop();
                }
            }
        }
    }
    if (notification_listener_) {
        notification_listener_(name, params);
    }
}

boost::asio::awaitable<support::Expected<std::map<std::string, std::string>>> McpHttpClient::get_stream_headers(
        const std::optional<std::string>& last_event_id) {
    std::map<std::string, std::string> headers = config_.headers;
    if (!session_id_.empty()) {
        headers["mcp-session-id"] = session_id_;
    }
    if (!protocol_version_.empty()) {
        headers["mcp-protocol-version"] = protocol_version_;
    }
    // pi `openSseStream`: only `accept` and, when resuming, `last-event-id`.
    headers["accept"] = "text/event-stream";
    if (last_event_id.has_value()) {
        headers["last-event-id"] = *last_event_id;
    }
    co_return co_await resolve_headers(std::move(headers));
}

boost::asio::awaitable<std::pair<McpHttpClient::GetStreamAttempt, support::Error>> McpHttpClient::consume_get_stream(
        GetStreamCursor& cursor) {
    auto headers = co_await get_stream_headers(cursor.last_event_id);
    if (!headers) {
        co_return std::pair{GetStreamAttempt::FatalFailure, std::move(headers.error())};
    }
    ai::providers::StreamRequest request;
    request.method = "GET";
    request.url = config_.url;
    request.headers = std::move(*headers);
    // pi bounds connection setup and response headers; body lifetime is
    // governed by `close()`'s cancellation.
    request.timeout = config_.request_timeout;
    request.stop_token = stream_stop_.get_token();

    GetStreamSseReader reader;
    auto on_chunk = [&reader, &cursor, this](std::string_view bytes) -> support::ExpectedVoid {
        auto events = reader.append(bytes);
        if (!events) {
            return std::unexpected(std::move(events.error()));
        }
        if (reader.last_event_id().has_value()) {
            cursor.last_event_id = reader.last_event_id();
        }
        if (reader.retry_ms().has_value()) {
            cursor.retry_ms = reader.retry_ms();
        }
        for (const auto& event : *events) {
            cursor.received = true;
            // Events without data prime resumption; other event types are not
            // JSON-RPC (pi `consumeSse`).
            if (event.event != "message" || !has_non_whitespace(event.data)) {
                continue;
            }
            if (auto message = support::read_json(event.data)) {
                handle_stream_message(*message);
            }
        }
        return {};
    };

    auto response = co_await transport_->async_stream(request, on_chunk);
    if (!response) {
        co_return std::pair{
                stream_stop_.stop_requested() ? GetStreamAttempt::FatalFailure : GetStreamAttempt::RetryableFailure,
                std::move(response.error())};
    }
    for (const auto& event : reader.finish()) {
        cursor.received = true;
        if (event.event != "message" || !has_non_whitespace(event.data)) {
            continue;
        }
        if (auto message = support::read_json(event.data)) {
            handle_stream_message(*message);
        }
    }
    const int status = response->head.status_code;
    // pi `openSseStream`: 405 means the server offers no GET stream, which is
    // the feature being absent, not a failure.
    if (status == 405) {
        co_return std::pair{GetStreamAttempt::Absent, support::Error{}};
    }
    if (status < 200 || status >= 300) {
        const bool transient = status == 408 || status == 429 || status >= 500;
        co_return std::pair{transient ? GetStreamAttempt::RetryableFailure : GetStreamAttempt::FatalFailure,
                http_status_error(config_.name, *response)};
    }
    co_return std::pair{GetStreamAttempt::Ended, support::Error{}};
}

std::chrono::milliseconds McpHttpClient::reconnect_delay(int attempt, const std::optional<int>& server_delay_ms) const {
    if (server_delay_ms.has_value()) {
        return std::chrono::milliseconds{*server_delay_ms};
    }
    const auto initial = get_stream_options_.initial_delay;
    // Bound the shift so a large `max_retries` cannot overflow the backoff.
    const auto backoff = attempt >= 30 ? get_stream_options_.max_delay : initial * (1 << attempt);
    return backoff < get_stream_options_.max_delay ? backoff : get_stream_options_.max_delay;
}

boost::asio::awaitable<bool> McpHttpClient::reconnect_wait(std::chrono::milliseconds delay) {
    if (stream_stop_.stop_requested()) {
        co_return false;
    }
    auto executor = co_await boost::asio::this_coro::executor;
    auto timer = std::make_shared<boost::asio::steady_timer>(executor);
    timer->expires_after(delay);
    std::stop_callback wake(stream_stop_.get_token(), [timer] { (void)timer->cancel(); });
    boost::system::error_code error;
    co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    co_return !stream_stop_.stop_requested();
}

boost::asio::awaitable<void> McpHttpClient::run_get_stream() {
    GetStreamCursor cursor;
    for (int attempt = 0; !closed_;) {
        const auto opened_at = std::chrono::steady_clock::now();
        auto [outcome, error] = co_await consume_get_stream(cursor);
        if (outcome == GetStreamAttempt::Absent) {
            // The server does not offer a GET stream; the feature is absent.
            co_return;
        }
        if (outcome == GetStreamAttempt::FatalFailure) {
            if (closed_) {
                co_return;
            }
            if (error_listener_) {
                error_listener_(error);
            }
            co_return;
        }
        if (outcome == GetStreamAttempt::Ended) {
            // A stream that delivered an event or stayed up past `maxDelay`
            // counts as healthy, even if it was idle (pi `runGetStream`).
            if (cursor.received || std::chrono::steady_clock::now() - opened_at > get_stream_options_.max_delay) {
                attempt = 0;
            }
        }
        cursor.received = false;
        if (attempt >= get_stream_options_.max_retries) {
            if (error_listener_) {
                error_listener_(support::make_error(support::ErrorCode::Process,
                        "MCP server-to-client stream dropped and could not be reopened",
                        "MCP server '" + config_.name + "' server-to-client stream dropped after " +
                                std::to_string(get_stream_options_.max_retries) + " retries"));
            }
            co_return;
        }
        if (!co_await reconnect_wait(reconnect_delay(attempt, cursor.retry_ms))) {
            co_return;
        }
        ++attempt;
    }
}

void McpHttpClient::start_get_stream() {
    if (!get_stream_options_.enabled || closed_) {
        return;
    }
    boost::asio::co_spawn(
            executor_,
            [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->run_get_stream(); },
            boost::asio::detached);
}

boost::asio::awaitable<void> McpHttpClient::delete_session() {
    std::map<std::string, std::string> headers = config_.headers;
    if (!session_id_.empty()) {
        headers["mcp-session-id"] = session_id_;
    }
    if (!protocol_version_.empty()) {
        headers["mcp-protocol-version"] = protocol_version_;
    }
    auto resolved = co_await resolve_headers(std::move(headers));
    if (!resolved) {
        // Resolving auth headers failed; the session expires on the server.
        co_return;
    }
    ai::providers::StreamRequest request;
    request.method = "DELETE";
    request.url = config_.url;
    request.headers = std::move(*resolved);
    // pi `close`: a 1 s bound on the session DELETE; the outcome is ignored.
    request.timeout = std::chrono::milliseconds{1000};
    (void)co_await transport_->async_stream(request, {});
}

void McpHttpClient::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    stream_stop_.request_stop();
    if (!session_id_.empty()) {
        boost::asio::co_spawn(
                executor_,
                [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->delete_session(); },
                boost::asio::detached);
    }
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
