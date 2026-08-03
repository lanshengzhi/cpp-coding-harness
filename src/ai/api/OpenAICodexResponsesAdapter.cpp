#include "OpenAICodexResponsesAdapter.hpp"

#include "MessageConversion.hpp"
#include "ResponsesStream.hpp"
#include "Termination.hpp"
#include "UsageNormalization.hpp"
#include "ai/providers/ProviderError.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "ai/providers/SseParser.hpp"
#include "ai/providers/StreamEmit.hpp"
#include "util/ExpectedMacros.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::ai::api {
namespace {

namespace stream = responses_stream;

using JsonObject = util::JsonValue::object_t;
using JsonArray = util::JsonValue::array_t;

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
    util::Error error{};
};

template <typename Headers>
void erase_header(Headers& headers, std::string_view name) {
    std::erase_if(headers, [name](const auto& header) {
        return stream::header_name_equal(header.first, name);
    });
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
[[nodiscard]] util::Expected<std::string> extract_account_id(
    std::string_view token) {
    const auto failure = [] {
        return std::unexpected(stream::stream_error(
            "Failed to extract accountId from token"));
    };
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
    auto parsed = util::read_json<util::JsonValue>(*payload);
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
        stream::set_header(
            headers,
            "Authorization",
            "Bearer " + *options.auth.api_key);
    }
    stream::set_header(headers, "chatgpt-account-id", std::string{account_id});
    stream::set_header(headers, "originator", "pi");
    stream::set_header(headers, "User-Agent", "pi (cpp-harness)");
    if (websocket) {
        erase_header(headers, "accept");
        erase_header(headers, "content-type");
        erase_header(headers, "openai-beta");
        // pi's connectWebSocket deletes the OpenAI-Beta header before the
        // handshake, so the WS connection deliberately carries no beta value.
        return headers;
    }
    stream::set_header(headers, "OpenAI-Beta", std::string{kSseBeta});
    if (!stream::has_header(headers, "accept") &&
        !stream::header_deleted(options, "accept")) {
        stream::set_header(headers, "accept", "text/event-stream");
    }
    if (!stream::has_header(headers, "content-type") &&
        !stream::header_deleted(options, "content-type")) {
        stream::set_header(headers, "content-type", "application/json");
    }
    return headers;
}

[[nodiscard]] std::pair<std::optional<std::string>, std::optional<std::string>>
extract_event_error(const JsonObject& event) {
    const auto* nested = stream::object_member(event, "error");
    auto code = stream::string_member(event, "code");
    if (!code && nested) {
        code = stream::string_member(*nested, "code");
    }
    auto message = stream::string_member(event, "message");
    if (!message && nested) {
        message = stream::string_member(*nested, "message");
    }
    return {
        code ? std::optional<std::string>{std::string{*code}} : std::nullopt,
        message ? std::optional<std::string>{std::string{*message}} : std::nullopt,
    };
}

void apply_codex_usage(
    const Model& model,
    const JsonObject& response,
    AssistantMessage& assistant) {
    const auto* usage = stream::object_member(response, "usage");
    if (!usage) {
        return;
    }
    const auto* input_details = stream::object_member(*usage, "input_tokens_details");
    const auto* output_details = stream::object_member(*usage, "output_tokens_details");
    assistant.usage = normalize_responses_usage(
        model,
        ResponsesUsageFields{
            .input_tokens = stream::integer_member(*usage, "input_tokens").value_or(0),
            .output_tokens = stream::integer_member(*usage, "output_tokens").value_or(0),
            .cached_tokens = input_details
                ? stream::integer_member(*input_details, "cached_tokens").value_or(0)
                : 0,
            .cache_write_tokens = input_details
                ? stream::integer_member(*input_details, "cache_write_tokens").value_or(0)
                : 0,
            .reasoning_tokens = output_details
                ? stream::integer_member(*output_details, "reasoning_tokens")
                : std::nullopt,
            .total_tokens = stream::integer_member(*usage, "total_tokens").value_or(0),
        });
}

[[nodiscard]] util::ExpectedVoid finalize_codex_response(
    const Model& model,
    const JsonObject& event,
    AssistantMessage& assistant,
    bool& saw_terminal) {
    const auto* response = stream::object_member(event, "response");
    if (!response) {
        // pi mapCodexEvents yields a terminal with an undefined response and
        // mapStopReason(undefined) → stop.
        saw_terminal = true;
        return {};
    }
    saw_terminal = true;
    if (const auto id = stream::string_member(*response, "id"); id && !id->empty()) {
        assistant.response_id = std::string{*id};
    }
    if (const auto model_id = stream::string_member(*response, "model");
        model_id && *model_id != model.id) {
        assistant.response_model = std::string{*model_id};
    }
    apply_codex_usage(model, *response, assistant);
    // pi mapCodexEvents.normalizeCodexStatus feeds processResponsesStream's
    // mapStopReason: completed → stop, incomplete → length, failed/cancelled →
    // error, and queued/in_progress/missing/unknown all normalize to stop.
    const auto status = stream::string_member(*response, "status");
    std::string_view normalized = "done";
    if (status &&
        (*status == "completed" || *status == "incomplete" ||
         *status == "failed" || *status == "cancelled")) {
        normalized = *status;
    }
    auto termination = map_responses_termination(
        normalized,
        std::ranges::any_of(assistant.content, [](const AssistantContent& block) {
            return std::holds_alternative<ToolCallContent>(block);
        }));
    if (!termination) {
        return std::unexpected(termination.error());
    }
    assistant.stop_reason = termination->reason;
    assistant.error_message = termination->error_message;
    return {};
}

enum class WsFrameAction { Continue, Terminal };

[[nodiscard]] util::Expected<WsFrameAction> process_codex_json_event(
    const JsonObject& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal,
    CodexFailure* failure) {
    const auto type = stream::string_member(event, "type");
    if (!type) {
        return WsFrameAction::Continue;
    }
    if (*type == "response.created") {
        if (const auto* response = stream::object_member(event, "response")) {
            if (const auto id = stream::string_member(*response, "id");
                id && !id->empty()) {
                assistant.response_id = std::string{*id};
            }
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.output_item.added") {
        const auto index = stream::output_index(event);
        const auto* item = stream::object_member(event, "item");
        if (index && item) {
            if (auto created = stream::create_slot(
                    *index, *item, slots, assistant, sink);
                !created) {
                return std::unexpected(created.error());
            }
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.reasoning_summary_text.delta" ||
        *type == "response.reasoning_text.delta" ||
        *type == "response.output_text.delta" ||
        *type == "response.refusal.delta" ||
        *type == "response.function_call_arguments.delta") {
        if (auto processed = stream::append_delta(
                event, *type, slots, assistant, sink);
            !processed) {
            return std::unexpected(processed.error());
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.reasoning_summary_part.done") {
        if (auto processed = stream::append_reasoning_separator(
                event, slots, assistant, sink);
            !processed) {
            return std::unexpected(processed.error());
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.function_call_arguments.done") {
        if (auto processed = stream::finish_argument_stream(
                event, slots, assistant, sink);
            !processed) {
            return std::unexpected(processed.error());
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.output_item.done") {
        if (auto processed = stream::finish_output_item(
                event, slots, assistant, sink);
            !processed) {
            return std::unexpected(processed.error());
        }
        return WsFrameAction::Continue;
    }
    if (*type == "response.completed" || *type == "response.done" ||
        *type == "response.incomplete") {
        if (auto finalized = finalize_codex_response(
                model, event, assistant, saw_terminal);
            !finalized) {
            return std::unexpected(finalized.error());
        }
        return WsFrameAction::Terminal;
    }
    if (*type == "response.failed") {
        const auto [code, message] = extract_event_error(event);
        if (failure) {
            failure->kind = CodexFailureKind::Api;
            failure->code = code.value_or("");
            failure->message = message.value_or("");
        }
        return std::unexpected(stream::stream_error(
            message.value_or("Codex response failed")));
    }
    if (*type == "error") {
        const auto [code, message] = extract_event_error(event);
        std::string detail = message.value_or("");
        if (detail.empty()) {
            detail = code.value_or("");
        }
        if (detail.empty()) {
            detail = util::write_json(util::JsonValue{event}).value_or("{}");
        }
        if (failure) {
            failure->kind = CodexFailureKind::Api;
            failure->code = code.value_or("");
            failure->message = message.value_or("");
        }
        return std::unexpected(stream::stream_error("Codex error: " + detail));
    }
    return WsFrameAction::Continue;
}

[[nodiscard]] util::ExpectedVoid process_codex_sse_event(
    const providers::SseEvent& event,
    const Model& model,
    AssistantMessage& assistant,
    std::map<std::size_t, stream::OutputSlot>& slots,
    AssistantEventSink& sink,
    bool& saw_terminal,
    CodexFailure* failure) {
    if (event.done || event.data.empty()) {
        return {};
    }
    if (event.event == "error") {
        return std::unexpected(stream::stream_error(event.data));
    }
    auto parsed = util::read_json<util::JsonValue>(event.data);
    if (!parsed) {
        if (event.event != "message" && !event.event.starts_with("response.")) {
            return {};
        }
        return std::unexpected(stream::stream_error(
            "Invalid Codex SSE JSON: " + parsed.error().detail));
    }
    const auto* event_object = parsed->get_if<JsonObject>();
    if (!event_object) {
        return std::unexpected(stream::stream_error(
            "Malformed Codex SSE event",
            "event data must be a JSON object"));
    }
    auto action = process_codex_json_event(
        *event_object, model, assistant, slots, sink, saw_terminal, failure);
    if (!action) {
        return std::unexpected(action.error());
    }
    return {};
}

[[nodiscard]] util::Error normalize_codex_transport_error(const util::Error& error) {
    if (error.code == util::ErrorCode::Cancelled) {
        return util::make_error(util::ErrorCode::Cancelled, "Request was aborted");
    }
    if (error.code == util::ErrorCode::Unknown) {
        return error;
    }
    return stream::stream_error(
        error.message.empty() ? "Codex request failed" : error.message,
        error.detail);
}

// ── WebSocket session cache ───────────────────────────────────────────────

struct CodexContinuation {
    util::JsonValue last_request_body;
    std::string last_response_id;
    util::JsonValue::array_t last_response_items;
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
    explicit CodexWebSocketCache(CodexWebSocketCacheConfig config)
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
    CodexWebSocketCacheConfig config_;
};

// ── Continuation / delta ──────────────────────────────────────────────────

[[nodiscard]] util::JsonValue body_without_input_and_previous(
    const util::JsonValue& body) {
    auto object = *body.get_if<JsonObject>();
    object.erase("input");
    object.erase("previous_response_id");
    return util::JsonValue{std::move(object)};
}

[[nodiscard]] bool request_bodies_match_except_input(
    const util::JsonValue& left,
    const util::JsonValue& right) {
    const auto left_bytes = util::write_json(body_without_input_and_previous(left));
    const auto right_bytes = util::write_json(body_without_input_and_previous(right));
    return left_bytes && right_bytes && *left_bytes == *right_bytes;
}

[[nodiscard]] std::optional<JsonArray> cached_input_delta(
    const util::JsonValue& body,
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
    const auto baseline_bytes = util::write_json(util::JsonValue{baseline});
    const auto prefix_bytes = util::write_json(util::JsonValue{
        JsonArray{current.begin(), current.begin() + baseline.size()}});
    if (!baseline_bytes || !prefix_bytes || *baseline_bytes != *prefix_bytes) {
        return std::nullopt;
    }
    return JsonArray{current.begin() + baseline.size(), current.end()};
}

[[nodiscard]] util::Expected<std::string> ws_frame_json(
    const util::JsonValue& body) {
    const auto* body_object = body.get_if<JsonObject>();
    if (!body_object) {
        return std::unexpected(stream::stream_error(
            "Codex request body is not an object"));
    }
    JsonObject frame;
    frame.emplace("type", "response.create");
    for (const auto& [key, value] : *body_object) {
        frame.emplace(key, value);
    }
    return util::write_json(util::JsonValue{std::move(frame)});
}

// ── Diagnostics ───────────────────────────────────────────────────────────

void append_transport_diagnostic(
    AssistantMessage& assistant,
    const util::Error& error,
    std::string_view configured_transport,
    bool websocket_started,
    std::size_t request_bytes) {
    if (!assistant.diagnostics) {
        assistant.diagnostics.emplace();
    }
    assistant.diagnostics->push_back(DiagnosticEntry{
        .type = "provider_transport_failure",
        .timestamp = stream::current_timestamp_ms(),
        .error = DiagnosticErrorInfo{
            .name = "Error",
            .message = error.message,
            .stack = std::nullopt,
            .code = std::nullopt,
        },
        .details = util::JsonValue::object_t{
            {"configuredTransport", std::string{configured_transport}},
            {"fallbackTransport",
             websocket_started
                 ? util::JsonValue{nullptr}
                 : util::JsonValue{std::string{"sse"}}},
            {"eventsEmitted", websocket_started},
            {"phase",
             websocket_started
                 ? "after_message_stream_start"
                 : "before_message_stream_start"},
            {"requestBytes", static_cast<double>(request_bytes)},
        },
    });
}

// ── WebSocket attempt ─────────────────────────────────────────────────────

struct WsAttemptOutcome {
    bool completed{false};
    util::Error error{};
    CodexFailureKind failure_kind{CodexFailureKind::Transport};
    std::string api_code{};
};

boost::asio::awaitable<util::Expected<WsAttemptOutcome>> run_ws_attempt(
    const std::shared_ptr<providers::WebSocketTransport>& ws_transport,
    const Model& model,
    const ProviderStreamOptions& options,
    const util::JsonValue& full_body,
    const providers::WebSocketConnectRequest& ws_request,
    std::optional<std::string_view> cache_session_id,
    std::string_view account_id,
    CodexWebSocketCache& cache,
    AssistantMessage& assistant,
    bool& started,
    bool& websocket_started,
    AssistantEventSink& sink) {
    websocket_started = false;

    const auto finish_failed = [](util::Error error,
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
            const auto kind = connected.error().code == util::ErrorCode::Cancelled
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

    util::JsonValue request_body = full_body;
    if (entry && entry->continuation) {
        auto delta = cached_input_delta(full_body, *entry->continuation);
        if (!delta || entry->continuation->last_response_id.empty()) {
            entry->continuation.reset();
        } else {
            auto object = *full_body.get_if<JsonObject>();
            object.insert_or_assign("input", util::JsonValue{std::move(*delta)});
            object.insert_or_assign(
                "previous_response_id", entry->continuation->last_response_id);
            request_body = util::JsonValue{std::move(object)};
        }
    }
    auto frame = ws_frame_json(request_body);
    if (!frame) {
        release_socket(false);
        co_return finish_failed(frame.error(), CodexFailureKind::Transport);
    }

    auto sent = co_await socket->async_send(*frame);
    if (!sent) {
        const auto kind = sent.error().code == util::ErrorCode::Cancelled
            ? CodexFailureKind::Cancelled
            : CodexFailureKind::Transport;
        release_socket(false);
        co_return finish_failed(sent.error(), kind);
    }

    std::map<std::size_t, stream::OutputSlot> slots;
    bool saw_terminal = false;
    CodexFailure failure;
    for (;;) {
        if (options.stop_token.stop_requested()) {
            release_socket(false);
            co_return finish_failed(
                util::make_error(
                    util::ErrorCode::Cancelled, "Request was aborted"),
                CodexFailureKind::Cancelled);
        }
        auto received = co_await socket->async_receive();
        if (!received) {
            const auto kind = received.error().code == util::ErrorCode::Cancelled
                ? CodexFailureKind::Cancelled
                : CodexFailureKind::Transport;
            release_socket(false);
            co_return finish_failed(received.error(), kind);
        }
        if (!*received) {
            release_socket(false);
            co_return finish_failed(
                stream::stream_error(
                    "WebSocket stream closed before response.completed"),
                CodexFailureKind::Transport);
        }
        auto parsed = util::read_json<util::JsonValue>(**received);
        if (!parsed) {
            failure = CodexFailure{
                .kind = CodexFailureKind::Protocol,
                .code = {},
                .message = {},
                .error = stream::stream_error(
                    "Invalid Codex WebSocket JSON: " + parsed.error().detail),
            };
            release_socket(false);
            co_return finish_failed(failure.error, CodexFailureKind::Protocol);
        }
        const auto* event = parsed->get_if<JsonObject>();
        if (!event) {
            // pi skips frames whose type is not a string.
            continue;
        }
        const auto frame_type = stream::string_member(*event, "type");
        const bool api_error_event = frame_type &&
            (*frame_type == "error" || *frame_type == "response.failed");
        if (!api_error_event && !websocket_started) {
            websocket_started = true;
            if (!started) {
                if (auto emitted = stream::emit_start(sink, assistant, started);
                    !emitted) {
                    release_socket(false);
                    co_return std::unexpected(emitted.error());
                }
            }
        }
        auto action = process_codex_json_event(
            *event, model, assistant, slots, sink, saw_terminal, &failure);
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
            util::make_error(util::ErrorCode::Cancelled, "Request was aborted"),
            CodexFailureKind::Cancelled);
    }
    if (assistant.stop_reason == AssistantStopReason::Error) {
        // pi's assertSuccessfulOutput throws here; the outer loop treats it as
        // a started transport failure (diagnostic + no SSE fallback).
        release_socket(false);
        co_return finish_failed(
            stream::stream_error(assistant.error_message.value_or(
                "Codex request failed")),
            CodexFailureKind::Transport);
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
    explicit Impl(CodexWebSocketCacheConfig config)
        : cache(std::move(config)) {}

    CodexWebSocketCache cache;
    std::set<std::string, std::less<>> sse_fallback_sessions;
};

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(
    std::shared_ptr<providers::StreamTransport> http_transport,
    std::shared_ptr<providers::WebSocketTransport> ws_transport,
    CodexWebSocketCacheConfig cache_config)
    : http_transport_(std::move(http_transport)),
      ws_transport_(std::move(ws_transport)),
      impl_(std::make_unique<Impl>(cache_config)) {}

OpenAICodexResponsesAdapter::OpenAICodexResponsesAdapter(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter& OpenAICodexResponsesAdapter::operator=(OpenAICodexResponsesAdapter&&) noexcept = default;
OpenAICodexResponsesAdapter::~OpenAICodexResponsesAdapter() = default;

boost::asio::awaitable<util::Expected<AssistantMessage>> OpenAICodexResponsesAdapter::stream(
    const Model& model,
    const AiContext& context,
    ProviderStreamOptions options,
    AssistantEventSink sink) {
    if (!http_transport_ || !ws_transport_) {
        co_return std::unexpected(stream::stream_error(
            "Codex Responses adapter requires HTTP and WebSocket transports"));
    }
    if (model.api != "openai-codex-responses") {
        co_return std::unexpected(stream::stream_error(
            "Codex Responses adapter received the wrong Model API"));
    }
    if (options.stop_token.stop_requested()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Cancelled,
            "Request was aborted"));
    }
    if (!options.auth.api_key || options.auth.api_key->empty()) {
        co_return std::unexpected(stream::stream_error(
            "No API key for provider: " + model.provider));
    }

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.timestamp = stream::current_timestamp_ms();

    std::optional<util::Error> sink_failure;
    AssistantEventSink guarded_sink =
        [&sink, &sink_failure](const AssistantStreamEvent& event) -> util::ExpectedVoid {
            auto emitted = providers::emit(sink, event);
            if (!emitted) {
                sink_failure = emitted.error();
            }
            return emitted;
        };

    CCH_TRY(account_id, extract_account_id(*options.auth.api_key));
    CCH_TRY(payload, build_adapter_payload(
        AdapterKind::OpenAICodexResponses, model, context, options));
    CCH_TRY(body_json, util::write_json(payload));
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
                    co_return stream::complete_failure(
                        assistant,
                        util::make_error(
                            util::ErrorCode::Cancelled,
                            "Request was aborted"),
                        guarded_sink);
                }
                if (assistant.stop_reason == AssistantStopReason::Error) {
                    co_return stream::complete_failure(
                        assistant,
                        stream::stream_error(assistant.error_message.value_or(
                            "Codex request failed")),
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
                co_return stream::complete_failure(
                    assistant, outcome->error, guarded_sink);
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
                co_return stream::complete_failure(
                    assistant, outcome->error, guarded_sink);
            }
            break;
        }
    }

    // SSE fallback path: pi's plain-JSON branch (zstd compression omitted).
    for (std::uint32_t attempt = 0;; ++attempt) {
        providers::SseParser parser;
        std::map<std::size_t, stream::OutputSlot> slots;
        bool saw_terminal = false;
        bool saw_body = false;
        std::optional<util::Error> handler_failure;

        auto handle_chunk = [&](std::string_view bytes) -> util::ExpectedVoid {
            saw_body = true;
            if (auto emitted = stream::emit_start(guarded_sink, assistant, started);
                !emitted) {
                handler_failure = emitted.error();
                return std::unexpected(emitted.error());
            }
            auto events = parser.append(bytes);
            if (!events) {
                handler_failure = events.error();
                return std::unexpected(events.error());
            }
            for (const auto& event : *events) {
                auto processed = process_codex_sse_event(
                    event,
                    model,
                    assistant,
                    slots,
                    guarded_sink,
                    saw_terminal,
                    nullptr);
                if (!processed) {
                    handler_failure = processed.error();
                    return std::unexpected(processed.error());
                }
            }
            return {};
        };

        auto response = co_await http_transport_->async_stream(sse_request, handle_chunk);
        if (!response) {
            if (sink_failure) {
                co_return std::unexpected(*sink_failure);
            }
            if (handler_failure) {
                co_return stream::complete_failure(
                    assistant, *handler_failure, guarded_sink);
            }
            if (response.error().code == util::ErrorCode::Cancelled ||
                options.stop_token.stop_requested()) {
                co_return stream::complete_failure(
                    assistant,
                    util::make_error(
                        util::ErrorCode::Cancelled,
                        "Request was aborted"),
                    guarded_sink);
            }
            const auto failure = stream::transport_failure(response.error());
            if (!saw_body) {
                CCH_TRY(retry, co_await stream::retry_provider_failure(
                    failure,
                    attempt,
                    options.max_retries,
                    options.max_retry_delay_ms,
                    options.stop_token));
                if (retry) {
                    continue;
                }
            }
            co_return stream::complete_failure(
                assistant,
                normalize_codex_transport_error(response.error()),
                guarded_sink);
        }

        if (response->head.status_code < 200 || response->head.status_code >= 300) {
            const auto failure = stream::response_failure(*response);
            CCH_TRY(retry, co_await stream::retry_provider_failure(
                failure,
                attempt,
                options.max_retries,
                options.max_retry_delay_ms,
                options.stop_token));
            if (retry) {
                continue;
            }
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(
                    "Codex request failed with HTTP " +
                        std::to_string(response->head.status_code),
                    response->body),
                guarded_sink);
        }

        if (!started) {
            if (auto emitted = stream::emit_start(guarded_sink, assistant, started);
                !emitted) {
                co_return std::unexpected(emitted.error());
            }
        }
        auto final_event = parser.finish();
        if (!final_event) {
            co_return stream::complete_failure(
                assistant, final_event.error(), guarded_sink);
        }
        if (*final_event) {
            if (auto processed = process_codex_sse_event(
                    **final_event,
                    model,
                    assistant,
                    slots,
                    guarded_sink,
                    saw_terminal,
                    nullptr);
                !processed) {
                if (sink_failure) {
                    co_return std::unexpected(*sink_failure);
                }
                co_return stream::complete_failure(
                    assistant, processed.error(), guarded_sink);
            }
        }
        if (options.stop_token.stop_requested()) {
            co_return stream::complete_failure(
                assistant,
                util::make_error(
                    util::ErrorCode::Cancelled,
                    "Request was aborted"),
                guarded_sink);
        }
        if (!saw_terminal) {
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(
                    "Codex stream ended without a stop reason"),
                guarded_sink);
        }
        if (assistant.stop_reason == AssistantStopReason::Error) {
            co_return stream::complete_failure(
                assistant,
                stream::stream_error(assistant.error_message.value_or(
                    "Codex request failed")),
                guarded_sink);
        }
        CCH_TRY_VOID(providers::emit(guarded_sink, AssistantDoneEvent{
            .reason = assistant.stop_reason,
            .message = assistant,
        }));
        co_return assistant;
    }
}

} // namespace cch::ai::api
