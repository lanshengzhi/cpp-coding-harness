#include "OpenAICodexResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ai/api/PartialJson.hpp"
#include "ai/api/ResponsesEventProcessor.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "ai/providers/StreamExecutionEngine.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

using JsonObject = support::JsonValue::object_t;
using JsonArray = support::JsonValue::array_t;

[[nodiscard]] TimestampMs current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

constexpr std::string_view kDefaultCodexBaseUrl =
    "https://chatgpt.com/backend-api";
constexpr std::string_view kJwtClaimPath = "https://api.openai.com/auth";
constexpr std::string_view kSseBeta = "responses=experimental";
constexpr std::string_view kPreviousResponseNotFound =
    "previous_response_not_found";
constexpr std::string_view kWebSocketConnectionLimitReached =
    "websocket_connection_limit_reached";
constexpr std::chrono::milliseconds kDefaultWebSocketConnectTimeout{15000};

enum class CodexFailureKind { Transport, Api, Protocol, Cancelled };

struct CodexFailure {
    CodexFailureKind kind{CodexFailureKind::Transport};
    std::string code{};
    std::string message{};
    support::Error error{};
};

[[nodiscard]] bool header_name_equal(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char left_character, char right_character) {
        const auto lower = [](char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
        };
        return lower(left_character) == lower(right_character);
    });
}

template <typename Headers> void set_header(Headers& headers, std::string name, std::string value) {
    std::erase_if(headers, [&name](const auto& header) { return header_name_equal(header.first, name); });
    headers.emplace(std::move(name), std::move(value));
}

template <typename Headers> [[nodiscard]] bool has_header(const Headers& headers, std::string_view name) {
    return std::ranges::any_of(headers,
            [name](const auto& header) { return header_name_equal(header.first, name) && !header.second.empty(); });
}

[[nodiscard]] bool header_deleted(const ProviderStreamOptions& options, std::string_view name) {
    return std::ranges::any_of(
            options.deleted_headers, [name](const auto& header) { return header_name_equal(header, name); });
}

template <typename Headers>
void erase_header(Headers& headers, std::string_view name) {
    std::erase_if(headers, [name](const auto& header) { return header_name_equal(header.first, name); });
}

[[nodiscard]] support::Error stream_error(std::string message, std::string detail = {}) {
    return support::make_error(support::ErrorCode::Stream,
            providers::bounded_provider_error_detail(std::move(message)),
            providers::bounded_provider_error_detail(std::move(detail)));
}

[[nodiscard]] std::string resolve_codex_url(std::string_view base_url) {
    std::string raw = base_url.empty()
        ? std::string{kDefaultCodexBaseUrl}
        : std::string{base_url};
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    if (raw.ends_with("/codex/responses")) {
        return raw;
    }
    if (raw.ends_with("/codex")) {
        return raw + "/responses";
    }
    return raw + "/codex/responses";
}

[[nodiscard]] std::string resolve_codex_websocket_url(std::string_view base_url) {
    std::string url = resolve_codex_url(base_url);
    if (url.starts_with("https://")) {
        url.replace(0, 8, "wss://");
    } else if (url.starts_with("http://")) {
        url.replace(0, 7, "ws://");
    }
    return url;
}

[[nodiscard]] std::optional<std::string> decode_base64(std::string_view input) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string cleaned;
    cleaned.reserve(input.size());
    for (const char character : input) {
        if (character == '-') {
            cleaned.push_back('+');
        } else if (character == '_') {
            cleaned.push_back('/');
        } else if (character == ' ' || character == '\n' ||
                   character == '\r' || character == '\t') {
            continue;
        } else {
            cleaned.push_back(character);
        }
    }
    std::string result;
    std::uint32_t accumulator = 0;
    int accumulator_bits = 0;
    for (const char character : cleaned) {
        if (character == '=') {
            break;
        }
        const auto index = kAlphabet.find(character);
        if (index == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(index);
        accumulator_bits += 6;
        if (accumulator_bits >= 8) {
            accumulator_bits -= 8;
            result.push_back(static_cast<char>(
                (accumulator >> accumulator_bits) & 0xff));
        }
    }
    return result;
}

/// pi extractAccountId: the Codex token is a ChatGPT JWT whose payload carries
/// `https://api.openai.com/auth.chatgpt_account_id`. The account id keys the
/// per-session socket cache and the `chatgpt-account-id` request header.
[[nodiscard]] support::Expected<std::string> extract_account_id(
    std::string_view token) {
    const auto failure = [] { return std::unexpected(stream_error("Failed to extract accountId from token")); };
    const auto first = token.find('.');
    if (first == std::string_view::npos) {
        return failure();
    }
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        return failure();
    }
    auto payload = decode_base64(token.substr(first + 1, second - first - 1));
    if (!payload) {
        return failure();
    }
    auto parsed = support::read_json(*payload);
    if (!parsed) {
        return failure();
    }
    const auto* payload_object = parsed->get_if<JsonObject>();
    if (!payload_object) {
        return failure();
    }
    const auto claim = payload_object->find(std::string{kJwtClaimPath});
    const auto* claim_object = claim != payload_object->end()
        ? claim->second.get_if<JsonObject>()
        : nullptr;
    if (!claim_object) {
        return failure();
    }
    const auto account = claim_object->find("chatgpt_account_id");
    const auto* account_id = account != claim_object->end() &&
            account->second.holds<std::string>()
        ? &account->second.get_string()
        : nullptr;
    if (!account_id || account_id->empty()) {
        return failure();
    }
    return *account_id;
}

/// pi buildBaseCodexHeaders plus the per-transport protocol fields. Base
/// headers are the post-merge/post-transform ModelAuth headers; deletions the
/// transform performed are honored for optional protocol fields.
[[nodiscard]] ProviderHeaders codex_headers(
    const ProviderStreamOptions& options,
    std::string_view account_id,
    bool websocket) {
    ProviderHeaders headers = options.auth.headers;
    if (options.auth.api_key && !options.auth.api_key->empty()) {
        set_header(headers, "Authorization", "Bearer " + *options.auth.api_key);
    }
    set_header(headers, "chatgpt-account-id", std::string{account_id});
    set_header(headers, "originator", "pi");
    set_header(headers, "User-Agent", "pi (cpp-harness)");
    if (websocket) {
        erase_header(headers, "accept");
        erase_header(headers, "content-type");
        erase_header(headers, "openai-beta");
        // pi's connectWebSocket deletes the OpenAI-Beta header before the
        // handshake, so the WS connection deliberately carries no beta value.
        return headers;
    }
    set_header(headers, "OpenAI-Beta", std::string{kSseBeta});
    if (!has_header(headers, "accept") && !header_deleted(options, "accept")) {
        set_header(headers, "accept", "text/event-stream");
    }
    if (!has_header(headers, "content-type") && !header_deleted(options, "content-type")) {
        set_header(headers, "content-type", "application/json");
    }
    return headers;
}

enum class WsFrameAction { Continue, Terminal };

[[nodiscard]] support::Expected<WsFrameAction> process_codex_json_event(JsonObject event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink,
        CodexFailure* failure) {
    const auto type_found = event.find("type");
    const auto* type = type_found != event.end() ? type_found->second.get_if<std::string>() : nullptr;
    const bool response_failed = type && *type == "response.failed";
    const bool error_event = type && *type == "error";
    std::string serialized_event;
    if (error_event) {
        serialized_event = support::write_json(support::JsonValue{event}).value_or("{}");
    }

    auto processed = processor.process(std::move(event), assistant, sink);
    if (!processed) {
        return std::unexpected(processed.error());
    }
    if (processed->provider_error) {
        const auto& provider_error = *processed->provider_error;
        if (failure) {
            failure->kind = CodexFailureKind::Api;
            failure->code = provider_error.code.value_or("");
            failure->message = provider_error.message.value_or("");
        }
        std::string detail = provider_error.message.value_or("");
        if (response_failed && detail.empty()) {
            detail = "Codex response failed";
        }
        if (!response_failed && detail.empty()) {
            detail = provider_error.code.value_or("");
        }
        if (!response_failed && detail.empty()) {
            detail = serialized_event;
        }
        return std::unexpected(stream_error(response_failed ? std::move(detail) : "Codex error: " + detail));
    }
    return processed->terminal ? WsFrameAction::Terminal : WsFrameAction::Continue;
}

[[nodiscard]] support::ExpectedVoid process_codex_sse_event(const providers::SseEvent& event,
        ResponsesEventProcessor& processor,
        AssistantMessage& assistant,
        AssistantEventSink& sink) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream_error(event.data));
    }
    auto parsed = support::read_json(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(stream_error("Invalid Codex SSE JSON: " + parsed.error().detail));
    }
    auto* event_object = parsed->get_if<JsonObject>();
    if (!event_object) {
        return std::unexpected(stream_error("Malformed Codex SSE event", "event data must be a JSON object"));
    }
    auto action = process_codex_json_event(std::move(*event_object), processor, assistant, sink, nullptr);
    if (!action) {
        return std::unexpected(action.error());
    }
    return {};
}

void finalize_tool_arguments(ToolCallContent& tool) {
    tool.arguments = parse_streaming_json(tool.raw_arguments);
    tool.arguments_valid = true;
    tool.argument_error = std::nullopt;
}

[[nodiscard]] support::ExpectedVoid emit_start(AssistantEventSink& sink, AssistantMessage& assistant, bool& started) {
    if (started) {
        return {};
    }
    started = true;
    return providers::emit(sink, AssistantStartEvent{.partial = assistant});
}

/// Adapter-owned terminal completion. The processor deliberately stops before
/// this policy: the adapter owns terminal sanitization, event commitment, and
/// transport-specific failure handling.
[[nodiscard]] support::Expected<AssistantMessage> complete_failure(
        AssistantMessage assistant, support::Error failure, AssistantEventSink& sink) {
    for (auto& block : assistant.content) {
        auto* tool = std::get_if<ToolCallContent>(&block);
        if (tool && !tool->arguments) {
            finalize_tool_arguments(*tool);
        }
    }
    const auto aborted = failure.code == support::ErrorCode::Cancelled;
    assistant.stop_reason = aborted ? AssistantStopReason::Aborted : AssistantStopReason::Error;
    if (aborted) {
        assistant.error_message = "Request was aborted";
        failure = support::make_error(support::ErrorCode::Cancelled, *assistant.error_message);
    } else {
        std::string diagnostic = failure.message;
        if (!failure.detail.empty() && diagnostic.find(failure.detail) == std::string::npos) {
            if (!diagnostic.empty()) {
                diagnostic += ": ";
            }
            diagnostic += failure.detail;
        }
        assistant.error_message = providers::bounded_provider_error_detail(std::move(diagnostic));
        failure = support::make_error(support::ErrorCode::Stream, *assistant.error_message);
    }
    auto emitted = providers::emit(sink,
            AssistantErrorEvent{
                    .reason = assistant.stop_reason,
                    .error = assistant,
                    .failure = std::move(failure),
            });
    if (!emitted) {
        return std::unexpected(emitted.error());
    }
    return assistant;
}

// ── WebSocket session cache ───────────────────────────────────────────────

struct CodexContinuation {
    support::JsonValue last_request_body;
    std::string last_response_id;
    support::JsonValue::array_t last_response_items;
};

struct CodexSocketEntry {
    std::string session_id;
    std::string account_id;
    std::shared_ptr<providers::WebSocket> socket;
    bool busy{false};
    std::int64_t created_at_ms{0};
    std::int64_t released_at_ms{0};
    std::optional<CodexContinuation> continuation;
};

[[nodiscard]] std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// pi websocketSessionCache: session-id → account-id → one connection, with
/// pi's 5-minute idle close and 55-minute hard age. Expiry is checked lazily
/// at acquisition (the socket is closed when the next request notices), which
/// keeps the observable connection/reuse behavior identical without timers
/// holding the io_context open.
class CodexWebSocketCache {
public:
    explicit CodexWebSocketCache(providers::CodexWebSocketCacheConfig config)
        : config_(config) {}

    ~CodexWebSocketCache() {
        close_all();
    }

    CodexWebSocketCache(const CodexWebSocketCache&) = delete;
    CodexWebSocketCache& operator=(const CodexWebSocketCache&) = delete;

    struct Acquisition {
        std::shared_ptr<providers::WebSocket> socket;
        std::shared_ptr<CodexSocketEntry> entry;
        bool reused{false};
    };

    /// Returns a reusable cached connection (marked busy) without connecting,
    /// or nullptr when no session/account entry is reusable. The caller then
    /// connects and registers the fresh socket.
    std::shared_ptr<CodexSocketEntry> try_reuse(
        std::optional<std::string_view> session_id,
        std::string_view account_id) {
        if (!session_id) {
            return nullptr;
        }
        const auto session = sessions_.find(std::string{*session_id});
        if (session == sessions_.end()) {
            return nullptr;
        }
        auto& account_entries = session->second;
        const auto found = account_entries.find(std::string{account_id});
        if (found == account_entries.end()) {
            return nullptr;
        }
        auto& entry = found->second;
        if (entry->busy) {
            return nullptr;
        }
        const auto now = now_epoch_ms();
        const bool idle_expired = entry->released_at_ms != 0 &&
            now - entry->released_at_ms >= config_.idle_close.count();
        const bool aged = now - entry->created_at_ms >= config_.max_age.count();
        if (idle_expired || aged) {
            close_and_remove(*session_id, account_id, entry);
            return nullptr;
        }
        entry->busy = true;
        return entry;
    }

    /// Registers a freshly connected socket under the session/account keys.
    /// Without a session id the socket is one-shot: never cached and closed by
    /// the caller. A busy cached entry yields an uncached one-shot socket.
    Acquisition register_or_reuse(
        std::optional<std::string_view> session_id,
        std::string_view account_id,
        std::shared_ptr<providers::WebSocket> socket) {
        if (!session_id) {
            return Acquisition{std::move(socket), nullptr, false};
        }
        auto& account_entries = sessions_[std::string{*session_id}];
        const auto found = account_entries.find(std::string{account_id});
        if (found != account_entries.end() && found->second->busy) {
            // Busy: pi opens a fresh uncached connection instead of waiting.
            return Acquisition{std::move(socket), nullptr, false};
        }
        auto entry = std::make_shared<CodexSocketEntry>(CodexSocketEntry{
            .session_id = std::string{*session_id},
            .account_id = std::string{account_id},
            .socket = socket,
            .busy = true,
            .created_at_ms = now_epoch_ms(),
            .released_at_ms = 0,
            .continuation = std::nullopt,
        });
        account_entries.emplace(std::string{account_id}, entry);
        return Acquisition{std::move(socket), std::move(entry), false};
    }

    void release(const std::shared_ptr<CodexSocketEntry>& entry, bool keep) {
        if (!entry) {
            return;
        }
        if (!keep) {
            close_and_remove(entry->session_id, entry->account_id, entry);
            return;
        }
        entry->busy = false;
        entry->released_at_ms = now_epoch_ms();
    }

    void close_all() {
        for (auto& [_, account_entries] : sessions_) {
            for (auto& [__, entry] : account_entries) {
                entry->socket->close();
            }
        }
        sessions_.clear();
    }

private:
    void close_and_remove(
        std::string_view session_id,
        std::string_view account_id,
        const std::shared_ptr<CodexSocketEntry>& entry) {
        entry->socket->close();
        const auto session = sessions_.find(std::string{session_id});
        if (session == sessions_.end()) {
            return;
        }
        auto& account_entries = session->second;
        const auto found = account_entries.find(std::string{account_id});
        if (found != account_entries.end() && found->second == entry) {
            account_entries.erase(found);
        }
        if (account_entries.empty()) {
            sessions_.erase(session);
        }
    }

    std::map<
        std::string,
        std::map<std::string, std::shared_ptr<CodexSocketEntry>, std::less<>>,
        std::less<>>
        sessions_;
    providers::CodexWebSocketCacheConfig config_;
};

// ── Continuation / delta ──────────────────────────────────────────────────

[[nodiscard]] support::JsonValue body_without_input_and_previous(
    const support::JsonValue& body) {
    auto object = *body.get_if<JsonObject>();
    object.erase("input");
    object.erase("previous_response_id");
    return support::JsonValue{std::move(object)};
}

[[nodiscard]] bool request_bodies_match_except_input(
    const support::JsonValue& left,
    const support::JsonValue& right) {
    const auto left_bytes = support::write_json(body_without_input_and_previous(left));
    const auto right_bytes = support::write_json(body_without_input_and_previous(right));
    return left_bytes && right_bytes && *left_bytes == *right_bytes;
}

[[nodiscard]] std::optional<JsonArray> cached_input_delta(
    const support::JsonValue& body,
    const CodexContinuation& continuation) {
    if (!request_bodies_match_except_input(body, continuation.last_request_body)) {
        return std::nullopt;
    }
    const auto* body_object = body.get_if<JsonObject>();
    if (!body_object) {
        return std::nullopt;
    }
    JsonArray current;
    if (const auto found = body_object->find("input");
        found != body_object->end()) {
        const auto* items = found->second.get_if<JsonArray>();
        if (!items) {
            return std::nullopt;
        }
        current = *items;
    }
    JsonArray baseline;
    if (const auto found = continuation.last_request_body.get_if<JsonObject>();
        found != nullptr) {
        if (const auto input = found->find("input");
            input != found->end()) {
            const auto* items = input->second.get_if<JsonArray>();
            if (items) {
                baseline = *items;
            }
        }
    }
    baseline.insert(
        baseline.end(),
        continuation.last_response_items.begin(),
        continuation.last_response_items.end());
    if (current.size() < baseline.size()) {
        return std::nullopt;
    }
    const auto baseline_bytes = support::write_json(support::JsonValue{baseline});
    const auto prefix_bytes = support::write_json(support::JsonValue{
        JsonArray{current.begin(), current.begin() + baseline.size()}});
    if (!baseline_bytes || !prefix_bytes || *baseline_bytes != *prefix_bytes) {
        return std::nullopt;
    }
    return JsonArray{current.begin() + baseline.size(), current.end()};
}

[[nodiscard]] support::Expected<std::string> ws_frame_json(
    const support::JsonValue& body) {
    const auto* body_object = body.get_if<JsonObject>();
    if (!body_object) {
        return std::unexpected(stream_error("Codex request body is not an object"));
    }
    JsonObject frame;
    frame.emplace("type", "response.create");
    for (const auto& [key, value] : *body_object) {
        frame.emplace(key, value);
    }
    return support::write_json(support::JsonValue{std::move(frame)});
}

// ── Diagnostics ───────────────────────────────────────────────────────────

void append_transport_diagnostic(
    AssistantMessage& assistant,
    const support::Error& error,
    std::string_view configured_transport,
    bool websocket_started,
    std::size_t request_bytes) {
    if (!assistant.diagnostics) {
        assistant.diagnostics.emplace();
    }
    assistant.diagnostics->push_back(DiagnosticEntry{
            .type = "provider_transport_failure",
            .timestamp = current_timestamp_ms(),
            .error =
                    DiagnosticErrorInfo{
                            .name = "Error",
                            .message = error.message,
                            .stack = std::nullopt,
                            .code = std::nullopt,
                    },
            .details =
                    support::JsonValue::object_t{
                            {"configuredTransport", std::string{configured_transport}},
                            {"fallbackTransport",
                                    websocket_started ? support::JsonValue{nullptr}
                                                      : support::JsonValue{std::string{"sse"}}},
                            {"eventsEmitted", websocket_started},
                            {"phase", websocket_started ? "after_message_stream_start" : "before_message_stream_start"},
                            {"requestBytes", static_cast<double>(request_bytes)},
                    },
    });
}

// ── WebSocket attempt ─────────────────────────────────────────────────────

struct WsAttemptOutcome {
    bool completed{false};
    support::Error error{};
    CodexFailureKind failure_kind{CodexFailureKind::Transport};
    std::string api_code{};
};

boost::asio::awaitable<support::Expected<WsAttemptOutcome>> run_ws_attempt(
    const std::shared_ptr<providers::WebSocketTransport>& ws_transport,
    const Model& model,
    const ProviderStreamOptions& options,
    const support::JsonValue& full_body,
    const providers::WebSocketConnectRequest& ws_request,
    std::optional<std::string_view> cache_session_id,
    std::string_view account_id,
    CodexWebSocketCache& cache,
    AssistantMessage& assistant,
    bool& started,
    bool& websocket_started,
    AssistantEventSink& sink) {
    websocket_started = false;

    const auto finish_failed = [](support::Error error,
                                  CodexFailureKind kind,
                                  std::string api_code = {}) {
        return WsAttemptOutcome{
            .completed = false,
            .error = std::move(error),
            .failure_kind = kind,
            .api_code = std::move(api_code),
        };
    };

    auto entry = cache.try_reuse(cache_session_id, account_id);
    std::shared_ptr<providers::WebSocket> socket;
    if (!entry) {
        auto connected = co_await ws_transport->async_connect(ws_request);
        if (!connected) {
            const auto kind = connected.error().code == support::ErrorCode::Cancelled
                ? CodexFailureKind::Cancelled
                : CodexFailureKind::Transport;
            co_return finish_failed(connected.error(), kind);
        }
        auto acquisition = cache.register_or_reuse(
            cache_session_id, account_id, std::move(*connected));
        entry = acquisition.entry;
        socket = std::move(acquisition.socket);
    } else {
        socket = entry->socket;
    }
    const auto release_socket = [&](bool keep) {
        if (entry) {
            cache.release(entry, keep);
        } else {
            socket->close();
        }
    };

    support::JsonValue request_body = full_body;
    if (entry && entry->continuation) {
        auto delta = cached_input_delta(full_body, *entry->continuation);
        if (!delta || entry->continuation->last_response_id.empty()) {
            entry->continuation.reset();
        } else {
            auto object = *full_body.get_if<JsonObject>();
            object.insert_or_assign("input", support::JsonValue{std::move(*delta)});
            object.insert_or_assign(
                "previous_response_id", entry->continuation->last_response_id);
            request_body = support::JsonValue{std::move(object)};
        }
    }
    auto frame = ws_frame_json(request_body);
    if (!frame) {
        release_socket(false);
        co_return finish_failed(frame.error(), CodexFailureKind::Transport);
    }

    auto sent = co_await socket->async_send(*frame);
    if (!sent) {
        const auto kind = sent.error().code == support::ErrorCode::Cancelled
            ? CodexFailureKind::Cancelled
            : CodexFailureKind::Transport;
        release_socket(false);
        co_return finish_failed(sent.error(), kind);
    }

    ResponsesEventProcessor processor{ResponsesDialect::Codex, ResponsesDelivery::WebSocket, model};
    CodexFailure failure;
    for (;;) {
        if (options.stop_token.stop_requested()) {
            release_socket(false);
            co_return finish_failed(
                support::make_error(
                    support::ErrorCode::Cancelled, "Request was aborted"),
                CodexFailureKind::Cancelled);
        }
        auto received = co_await socket->async_receive();
        if (!received) {
            const auto kind = received.error().code == support::ErrorCode::Cancelled
                ? CodexFailureKind::Cancelled
                : CodexFailureKind::Transport;
            release_socket(false);
            co_return finish_failed(received.error(), kind);
        }
        if (!*received) {
            release_socket(false);
            co_return finish_failed(
                    stream_error("WebSocket stream closed before response.completed"), CodexFailureKind::Transport);
        }
        auto parsed = support::read_json(**received);
        if (!parsed) {
            failure = CodexFailure{
                    .kind = CodexFailureKind::Protocol,
                    .code = {},
                    .message = {},
                    .error = stream_error("Invalid Codex WebSocket JSON: " + parsed.error().detail),
            };
            release_socket(false);
            co_return finish_failed(failure.error, CodexFailureKind::Protocol);
        }
        auto* event = parsed->get_if<JsonObject>();
        if (!event) {
            // pi skips frames whose type is not a string.
            continue;
        }
        const auto type_found = event->find("type");
        const auto* frame_type = type_found != event->end() ? type_found->second.get_if<std::string>() : nullptr;
        const bool api_error_event = frame_type &&
            (*frame_type == "error" || *frame_type == "response.failed");
        if (!api_error_event && !websocket_started) {
            websocket_started = true;
            if (!started) {
                if (auto emitted = emit_start(sink, assistant, started); !emitted) {
                    release_socket(false);
                    co_return std::unexpected(emitted.error());
                }
            }
        }
        auto action = process_codex_json_event(std::move(*event), processor, assistant, sink, &failure);
        if (!action) {
            const auto kind = failure.kind;
            release_socket(false);
            co_return finish_failed(action.error(), kind, failure.code);
        }
        if (*action == WsFrameAction::Terminal) {
            break;
        }
    }

    if (options.stop_token.stop_requested()) {
        release_socket(false);
        co_return finish_failed(
            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
            CodexFailureKind::Cancelled);
    }
    if (assistant.stop_reason == AssistantStopReason::Error) {
        // pi's assertSuccessfulOutput throws here; the outer loop treats it as
        // a started transport failure (diagnostic + no SSE fallback).
        release_socket(false);
        co_return finish_failed(
                stream_error(assistant.error_message.value_or("Codex request failed")), CodexFailureKind::Transport);
    }
    if (entry && assistant.response_id) {
        auto items = build_responses_continuation_items(model, assistant);
        if (items) {
            entry->continuation = CodexContinuation{
                .last_request_body = full_body,
                .last_response_id = *assistant.response_id,
                .last_response_items = std::move(*items),
            };
        }
    }
    release_socket(true);
    co_return WsAttemptOutcome{.completed = true};
}

} // namespace

struct OpenAICodexResponsesAdapter::Impl {
    explicit Impl(providers::CodexWebSocketCacheConfig config)
        : cache(std::move(config)) {}

    CodexWebSocketCache cache;
    std::set<std::string, std::less<>> sse_fallback_sessions;
};

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(
    std::shared_ptr<providers::StreamTransport> http_transport,
    std::shared_ptr<providers::WebSocketTransport> ws_transport,
    providers::CodexWebSocketCacheConfig cache_config)
    : http_transport_(std::move(http_transport)),
      ws_transport_(std::move(ws_transport)),
      impl_(std::make_unique<Impl>(cache_config)) {}

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter& OpenAICodexResponsesAdapter::operator=(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter::~OpenAICodexResponsesAdapter() = default;

boost::asio::awaitable<support::Expected<AssistantMessage>> OpenAICodexResponsesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!http_transport_ || !ws_transport_) {
        co_return std::unexpected(stream_error("Codex Responses adapter requires HTTP and WebSocket transports"));
    }
    if (model.api != "openai-codex-responses") {
        co_return std::unexpected(stream_error("Codex Responses adapter received the wrong Model API"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (!options.auth.api_key || options.auth.api_key->empty()) {
        co_return std::unexpected(stream_error("No API key for provider: " + model.provider));
    }

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.stop_reason = AssistantStopReason::Pending;
    assistant.timestamp = current_timestamp_ms();

    std::optional<support::Error> sink_failure;
    AssistantEventSink guarded_sink =
        [&sink, &sink_failure](const AssistantStreamEvent& event) -> support::ExpectedVoid {
            auto emitted = providers::emit(sink, event);
            if (!emitted) {
                sink_failure = emitted.error();
            }
            return emitted;
        };

    CCH_TRY(account_id, extract_account_id(*options.auth.api_key));
    CCH_TRY(payload, build_adapter_payload(
        AdapterKind::OpenAICodexResponses, model, context, options));
    CCH_TRY(body_json, support::write_json(payload));
    const auto codex_url = resolve_codex_url(model.base_url);
    const auto cache_session_id = options.session_id;

    const auto ws_headers = codex_headers(options, account_id, true);
    const auto sse_headers = codex_headers(options, account_id, false);
    providers::WebSocketConnectRequest ws_request;
    ws_request.url = resolve_codex_websocket_url(model.base_url);
    ws_request.headers = ws_headers;
    ws_request.connect_timeout = kDefaultWebSocketConnectTimeout;
    if (options.timeout_ms) {
        ws_request.idle_timeout = std::chrono::milliseconds{*options.timeout_ms};
    }
    ws_request.stop_token = options.stop_token;

    providers::StreamRequest sse_request;
    sse_request.url = codex_url;
    sse_request.timeout = std::chrono::milliseconds{
        options.timeout_ms.value_or(30000)};
    sse_request.stop_token = options.stop_token;
    sse_request.headers.insert(sse_headers.begin(), sse_headers.end());
    sse_request.body = body_json;

    CodexWebSocketCache& cache = impl_->cache;
    auto& sse_fallback_sessions = impl_->sse_fallback_sessions;

    bool started = false;
    const bool ws_disabled = cache_session_id.has_value() &&
        sse_fallback_sessions.contains(std::string{*cache_session_id});

    if (!ws_disabled) {
        bool retried_previous_response = false;
        bool retried_connection_limit = false;
        while (true) {
            bool websocket_started = false;
            auto outcome = co_await run_ws_attempt(
                ws_transport_,
                model,
                options,
                payload,
                ws_request,
                cache_session_id,
                account_id,
                cache,
                assistant,
                started,
                websocket_started,
                guarded_sink);
            if (!outcome) {
                co_return std::unexpected(outcome.error());
            }
            if (outcome->completed) {
                if (options.stop_token.stop_requested()) {
                    co_return complete_failure(assistant,
                            support::make_error(support::ErrorCode::Cancelled, "Request was aborted"),
                            guarded_sink);
                }
                if (assistant.stop_reason == AssistantStopReason::Error) {
                    co_return complete_failure(assistant,
                            stream_error(assistant.error_message.value_or("Codex request failed")),
                            guarded_sink);
                }
                CCH_TRY_VOID(providers::emit(
                    guarded_sink,
                    AssistantDoneEvent{
                        .reason = assistant.stop_reason,
                        .message = assistant,
                    }));
                co_return assistant;
            }

            const bool aborted =
                options.stop_token.stop_requested() ||
                outcome->failure_kind == CodexFailureKind::Cancelled;
            const bool connection_limit_before_start =
                !websocket_started &&
                outcome->failure_kind == CodexFailureKind::Api &&
                outcome->api_code == kWebSocketConnectionLimitReached;
            const bool previous_response_not_found =
                outcome->failure_kind == CodexFailureKind::Api &&
                outcome->api_code == kPreviousResponseNotFound;
            if (!aborted && previous_response_not_found &&
                !retried_previous_response) {
                retried_previous_response = true;
                continue;
            }
            if (!aborted && connection_limit_before_start &&
                !retried_connection_limit) {
                retried_connection_limit = true;
                continue;
            }
            if (aborted ||
                ((outcome->failure_kind == CodexFailureKind::Api ||
                  outcome->failure_kind == CodexFailureKind::Protocol) &&
                 !connection_limit_before_start)) {
                co_return complete_failure(assistant, outcome->error, guarded_sink);
            }
            append_transport_diagnostic(
                assistant,
                outcome->error,
                "auto",
                websocket_started,
                body_json.size());
            if (cache_session_id) {
                sse_fallback_sessions.insert(std::string{*cache_session_id});
            }
            if (websocket_started) {
                co_return complete_failure(assistant, outcome->error, guarded_sink);
            }
            break;
        }
    }

    // SSE fallback path: pi's plain-JSON branch (zstd compression omitted).
    struct AttemptState {
        std::unique_ptr<ResponsesEventProcessor> processor;
    };

    auto attempt_state = std::make_shared<AttemptState>();

    auto attempt_hook = [attempt_state, &model]() -> support::Expected<providers::SseEventHook> {
        attempt_state->processor =
                std::make_unique<ResponsesEventProcessor>(ResponsesDialect::Codex, ResponsesDelivery::Sse, model);
        return [attempt_state](const providers::SseEvent& event,
                       AssistantMessage& assistant,
                       AssistantEventSink& sink) -> support::ExpectedVoid {
            return process_codex_sse_event(event, *attempt_state->processor, assistant, sink);
        };
    };

    auto finalize_hook = [attempt_state](AssistantMessage& assistant) -> support::ExpectedVoid {
        return attempt_state->processor->finish(assistant);
    };

    co_return co_await providers::execute_sse_stream(providers::SseStreamExecutionOptions{
        .protocol_name = "Codex",
        .request = sse_request,
        .transport = *http_transport_,
        .options = options,
        .initial_assistant = std::move(assistant),
        .sink = std::move(sink),
        .attempt_hook = std::move(attempt_hook),
        .finalize_hook = std::move(finalize_hook),
    });
}

} // namespace cch::ai::api
