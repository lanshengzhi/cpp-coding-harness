#include <cch/mcp/UpstreamClient.hpp>

#include "mcp/Diagnostics.hpp"
#include "mcp/EraAdapter.hpp"
#include "mcp/HeaderMirror.hpp"
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"
#include "mcp/WwwAuthenticate.hpp"
#include "mcp/WireDto.hpp"
#include "support/Json.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::mcp {

struct UpstreamClient::Connection {
    std::string server_id;
    std::shared_ptr<McpTransport> transport;
    UpstreamClientOptions options{};
    /// The era selected for this connection. Null until a probe succeeds; a
    /// reconnect builds a new connection and therefore re-probes.
    std::unique_ptr<era::EraAdapter> era;
    /// Server-provided usage guidance from the probe, carried onto the catalog
    /// so the model sees it with the tools it belongs to.
    std::string instructions{};
    /// The bearer the last resolved request authenticated with, kept only so
    /// the package's own redaction can erase it from anything the Upstream
    /// hands back. It is never read for any other purpose (issue #838).
    std::optional<std::string> resolved_bearer{std::nullopt};
    std::atomic<std::uint64_t> last_request_id{0};
};

namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::JsonValue;
using support::make_error;

/// An Upstream's non-conformance, as distinct from a transport failure. A
/// protocol failure is bound to the operation it arrived on: for `tools/call`
/// it is exactly one failed tool call, never a client-stack failure.
struct ProtocolFailure {
    std::string diagnostic;
    int json_rpc_code{0};
};

/// The decoded `result` of a JSON-RPC exchange together with the **source
/// text** of that result member, the Upstream's protocol violation, or the
/// transport's own failure.
///
/// The raw text is what makes the opaque `requestState` echoable byte-for-byte
/// (issue #845): a value parsed into `JsonValue` and re-serialized is
/// normalized, and a normalized continuation token is a different token.
struct ExchangeResult {
    JsonValue value{};
    /// The `result` member's source text; empty when the message carried no
    /// recoverable `result` member text.
    std::string raw{};
};

using ExchangeOutcome = std::variant<ExchangeResult, ProtocolFailure, Error>;
using ExchangeOutcomeHandler = std::move_only_function<void(ExchangeOutcome)>;

/// Drives one operation's request chain to quiescence without nesting: a
/// completion that needs a follow-up exchange appends it here instead of
/// recursing, so a catalog walk bounded by the pagination cap keeps a flat
/// call stack.
class ExchangeQueue {
public:
    void post(std::move_only_function<void()> step) {
        pending_.push_back(std::move(step));
        if (draining_) {
            return;
        }
        draining_ = true;
        while (!pending_.empty()) {
            auto next = std::move(pending_.front());
            pending_.pop_front();
            next();
        }
        draining_ = false;
    }

private:
    std::deque<std::move_only_function<void()>> pending_;
    bool draining_{false};
};

/// Diagnostics are redacted before they are truncated (CODING_STANDARDS.md
/// §10.2), never carry an `Error::context` — which for a parse failure is the
/// whole untrusted response body — and are redacted by the connection's
/// resolved credential value as well, so an Upstream that echoes the bearer
/// back cannot put it in a diagnostic (issue #838). Both the bound and the
/// shape rules live in the one private diagnostics point the connection
/// machinery shares (issue #839).
[[nodiscard]] std::string bounded_diagnostic(std::string text, std::string_view secret) {
    return diagnostics::bounded(std::move(text), secret);
}

[[nodiscard]] std::string diagnostic_of(const Error& error, std::string_view secret) {
    return diagnostics::of(error, secret);
}

/// An error leaving this package, redacted by the connection's resolved
/// credential value and bounded before it reaches a status surface or a
/// session record (issue #838).
[[nodiscard]] Error redacted_error(const Error& error, std::string_view secret) {
    return make_error(error.code, bounded_diagnostic(error.message, secret), bounded_diagnostic(error.detail, secret));
}

[[nodiscard]] double next_request_id(const std::shared_ptr<UpstreamClient::Connection>& connection) {
    return static_cast<double>(connection->last_request_id.fetch_add(1) + 1);
}

/// The member named `key` of an object value, or `nullptr` when the value is
/// not an object or carries no such member. `cch::support::JsonValue` has no
/// `contains()`.
[[nodiscard]] const JsonValue* member(const JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<JsonValue::object_t>();
    if (object == nullptr) {
        return nullptr;
    }
    const auto found = object->find(std::string(key));
    return found == object->end() ? nullptr : &found->second;
}

/// Declare `token` under the revision's `progressToken` `_meta` key. Written
/// after the era's own reserved keys, so a `params` the caller built rides
/// along untouched and only the progress token is added here.
void attach_progress_token(JsonValue& params, const JsonValue& token) {
    auto& object = params.get_object();
    auto& meta = object["_meta"];
    if (meta.get_if<JsonValue::object_t>() == nullptr) {
        meta = JsonValue::object_t{};
    }
    meta.get_object()[std::string(protocol::kMetaProgressTokenKey)] = token;
}

/// One `notifications/progress` notification, or nothing when it is not one
/// this call can be shown: a missing or malformed `progressToken`, a token
/// naming another call, or a non-numeric `progress` all yield `std::nullopt`
/// and the notification is dropped. Dropping is silent by design — a server
/// that reports progress for a request the host no longer has must not be
/// able to write into another call's display.
[[nodiscard]] std::optional<UpstreamToolProgress> read_progress(
        const JsonValue& params, std::string_view expected_token, std::string_view secret) {
    const auto* token = member(params, protocol::kParamProgressToken);
    if (token == nullptr) {
        return std::nullopt;
    }
    // The revision allows a string or a number token; the host mints a string,
    // and a number is read as the same decimal spelling so an Upstream that
    // normalizes the token is not silently ignored.
    if (const auto* text = token->get_if<std::string>(); text != nullptr) {
        if (*text != expected_token) {
            return std::nullopt;
        }
    } else if (const auto* number = token->get_if<double>(); number == nullptr
               || !std::isfinite(*number) || std::to_string(static_cast<long long>(*number)) != expected_token) {
        return std::nullopt;
    }
    const auto* progress = member(params, protocol::kParamProgress);
    const auto* counted = progress != nullptr ? progress->get_if<double>() : nullptr;
    if (counted == nullptr || !std::isfinite(*counted)) {
        return std::nullopt;
    }
    UpstreamToolProgress read{.progress = *counted};
    if (const auto* total = member(params, protocol::kParamTotal); total != nullptr) {
        if (const auto* value = total->get_if<double>(); value != nullptr && std::isfinite(*value)) {
            read.total = *value;
        }
    }
    if (const auto* message = member(params, protocol::kParamMessage); message != nullptr) {
        if (const auto* text = message->get_if<std::string>(); text != nullptr) {
            // Upstream-supplied text on its way to a display: redacted before
            // it is bounded, exactly as a diagnostic is (issue #838).
            read.message = bounded_diagnostic(*text, secret);
        }
    }
    return read;
}

/// The one in-flight call a progress notification may belong to. It is the
/// operation's own sink and the token that operation minted, so a notification
/// is delivered only when both match.
struct ProgressObserver {
    UpstreamProgressSink* sink{nullptr};
    std::string token{};

    void observe(const jsonrpc::WireMessage& message, std::string_view secret) const {
        if (sink == nullptr || token.empty() || message.method != protocol::kNotificationProgress) {
            return;
        }
        if (auto progress = read_progress(message.params, token, secret)) {
            (*sink)(*progress);
        }
    }
};

/// The value of one response header, matched case-insensitively: HTTP field
/// names are case-insensitive, and an Upstream that spells its
/// `WWW-Authenticate` header differently still made an authorization
/// requirement of this client.
[[nodiscard]] std::string_view header_value(const McpResponse& response, std::string_view field) {
    for (const auto& [name, value] : response.headers) {
        if (name.size() != field.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(name[index])) !=
                    std::tolower(static_cast<unsigned char>(field[index]))) {
                same = false;
                break;
            }
        }
        if (same) {
            return std::string_view{value};
        }
    }
    return {};
}

/// Read the one response that belongs to `id` out of a transport answer.
/// Notifications ahead of it — including `tools/list_changed` — are safely
/// ignored: pike does not subscribe to `subscriptions/listen`, so the
/// notification neither reconnects the connection nor refreshes the catalog.
[[nodiscard]] ExchangeOutcome read_exchange_outcome(double id,
        const std::expected<McpResponse, Error>& answer,
        std::string_view secret,
        const ProgressObserver& progress,
        const std::function<void(const AuthorizationChallenge&)>* challenge_sink) {
    if (!answer) {
        return answer.error();
    }
    const auto& response = *answer;
    if (response.status_code == 401 || response.status_code == 403) {
        // An authentication challenge is not a protocol violation: it is the
        // Upstream refusing to serve the call until the user has authorized
        // it, and the connection reads it as health evidence (ADR 0064).
        //
        // The `WWW-Authenticate` field is parsed here, once, and reduced to
        // what the authorization flow needs. A field this build cannot act on
        // — a scheme that is not OAuth, or a malformed value — changes
        // nothing the user sees: the status is `needs_auth` either way, and a
        // header this build cannot honour is not a header it reports.
        //
        // Nothing the header *carries* is put in this error. The `needs_auth`
        // publish is the one status path that has no resolved credential to
        // redact with (issue #838), so a server-derived value — an OAuth
        // `error`, let alone an `error_description` — reaching it would be
        // server text the package cannot erase before it reaches a status
        // surface (CODING_STANDARDS.md §10.7). The parsed challenge travels
        // to the authorization flow through `challenge_sink` instead, which
        // stores it and never reports it.
        const auto challenge = www_authenticate::first_bearer_challenge(header_value(response, "WWW-Authenticate"));
        if (challenge.has_value() && challenge_sink != nullptr && static_cast<bool>(*challenge_sink)) {
            (*challenge_sink)(*challenge);
        }
        return make_error(ErrorCode::Auth,
                "the Upstream MCP Server requires authentication",
                "the Upstream answered with HTTP status " + std::to_string(response.status_code));
    }
    if (response.status_code < 200 || response.status_code > 299) {
        return ProtocolFailure{
                .diagnostic = bounded_diagnostic(
                        "the Upstream MCP Server answered with HTTP status " + std::to_string(response.status_code),
                        secret),
                .json_rpc_code = 0,
        };
    }
    auto slices = jsonrpc::split_message_slices(response.body);
    if (!slices) {
        return ProtocolFailure{.diagnostic = diagnostic_of(slices.error(), secret), .json_rpc_code = 0};
    }
    for (const auto& slice : *slices) {
        auto parsed = support::read_json(slice);
        if (!parsed) {
            return ProtocolFailure{.diagnostic = diagnostic_of(parsed.error(), secret), .json_rpc_code = 0};
        }
        auto decoded = jsonrpc::decode_message(*parsed);
        if (!decoded) {
            return ProtocolFailure{.diagnostic = diagnostic_of(decoded.error(), secret), .json_rpc_code = 0};
        }
        if (decoded->is_notification()) {
            // `tools/list_changed` is safely ignored wherever it arrives, and a
            // `notifications/progress` is offered to the one call whose token
            // it echoes; anything else is not this build's to act on.
            progress.observe(*decoded, secret);
            continue;
        }
        if (decoded->id != id) {
            continue; // a stale response for an abandoned request
        }
        if (decoded->is_error) {
            return ProtocolFailure{
                    .diagnostic = bounded_diagnostic("the Upstream MCP Server returned JSON-RPC error " +
                                                             std::to_string(decoded->error_code) + ": " +
                                                             decoded->error_message,
                            secret),
                    .json_rpc_code = decoded->error_code,
            };
        }
        const auto raw_result = jsonrpc::object_member_source(slice, "result");
        return ExchangeResult{
                .value = decoded->result,
                .raw = raw_result.has_value() ? std::string(*raw_result) : std::string{},
        };
    }
    return ProtocolFailure{
            .diagnostic = bounded_diagnostic("the Upstream MCP Server sent no JSON-RPC response for request " +
                                                     std::to_string(static_cast<std::int64_t>(id)),
                    secret),
            .json_rpc_code = 0,
    };
}

class ProbeOperation;

/// The shared half of every operation: the connection it runs on, its request
/// chain, and its terminal completion. An operation owns its own state for as
/// long as its chain is in flight, so releasing the client mid-operation is
/// safe; each in-flight continuation holds the state explicitly.
class Operation : public std::enable_shared_from_this<Operation> {
public:
    Operation(std::shared_ptr<UpstreamClient::Connection> connection, std::string_view method, std::stop_token token)
        : connection_(std::move(connection)), method_(method), stop_token_(token) {}
    virtual ~Operation() = default;

protected:
    /// Continue this operation once its connection has an era. A connection
    /// that has already probed proceeds straight to `on_era_selected`; one
    /// that has not is probed first, and a probe that failed completes this
    /// operation with the probe's error.
    void start_after_era() {
        if (connection_->era == nullptr) {
            run_probe();
            return;
        }
        on_era_selected();
    }

    [[nodiscard]] const std::shared_ptr<UpstreamClient::Connection>& connection() const noexcept { return connection_; }

    /// The caller's cancellation token, which the two-phase close stops too
    /// (ADR 0020). A Pending Elicitation wait joins it, so a close during a
    /// suspended call reaches the dialog as well as the transport.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_token_; }

    /// The elicitation port this operation may use, or `nullptr` when the
    /// caller wired none.
    [[nodiscard]] UpstreamElicitationPort* elicitation_port() const noexcept {
        return connection_->options.elicitation.get();
    }

    /// The credential the last resolved request on this connection
    /// authenticated with, for redacting anything the Upstream hands back. It
    /// is never returned to a caller.
    [[nodiscard]] std::string_view secret() const noexcept {
        const auto& bearer = connection_->resolved_bearer;
        return bearer.has_value() ? std::string_view{*bearer} : std::string_view{};
    }

    template <typename Self> [[nodiscard]] std::shared_ptr<Self> self() {
        return std::static_pointer_cast<Self>(Operation::shared_from_this());
    }

    /// One operation's terminal outcome happens exactly once. A transport that
    /// answers a request and then reports a stop — or answers the same request
    /// twice — is absorbed here instead of finishing an already-finished call
    /// a second time, so a late completion can never double-finish a call
    /// (ADR 0008, spec #833 story 30).
    [[nodiscard]] bool claim_completion() noexcept {
        if (completed_) {
            return false;
        }
        completed_ = true;
        return true;
    }

    /// Issue one framed JSON-RPC request and hand its outcome to
    /// `on_outcome`, attributed to the derived operation's own chain.
    ///
    /// `raw_params` are members written to the wire as already-serialized JSON
    /// text rather than through the value tree. They exist for the opaque
    /// `requestState` of a Multi Round-Trip retry, which must reach the server
    /// byte-for-byte (issue #845); an empty list makes the framed body
    /// byte-identical to the value-only encoding.
    ///
    /// The credential is resolved before every exchange, not once per
    /// connection: a token that is rotated in the environment or expired in the
    /// store takes effect on the next request without a reconnect
    /// (issue #838, ADR 0032's resolve-before-each-request rule). A credential
    /// that cannot be resolved fails the exchange as a connection failure and
    /// no request is written.
    void send(JsonValue params,
            std::string_view name,
            std::map<std::string, std::string> extra_headers,
            ExchangeOutcomeHandler on_outcome,
            std::vector<jsonrpc::RawJsonMember> raw_params = {}) {
        auto connection = connection_;
        resolve_auth(stop_token_,
                [self = Operation::shared_from_this(),
                        params = std::move(params),
                        name = std::string{name},
                        extra_headers = std::move(extra_headers),
                        on_outcome = std::move(on_outcome),
                        raw_params = std::move(raw_params)](Expected<UpstreamAuth> auth) mutable {
                    self->dispatch(std::move(params),
                            std::move(name),
                            std::move(extra_headers),
                            std::move(on_outcome),
                            std::move(raw_params),
                            std::move(auth));
                });
    }

    /// The operation's real work, reached once the era is selected.
    virtual void on_era_selected() = 0;

    /// A probe this operation needed failed, so the operation has no era to
    /// run on and completes with the probe's error.
    virtual void on_probe_failed(Error error) = 0;

    /// The JSON-RPC id of the exchange `dispatch` is about to write. An
    /// operation that reports progress records the id here, because the id is
    /// the `progressToken` it declares and the token the Upstream echoes.
    virtual void on_request_id(double) {}

    /// Report progress for this operation's exchange. A call made without a
    /// sink declares no progress token, and an Upstream's progress
    /// notifications for it are dropped with every other unrecognized one.
    ///
    /// The sink is *owned* by the operation rather than pointed at: it outlives
    /// the constructor that received it, and the same value serves every
    /// notification of the call.
    void report_progress_through(UpstreamProgressSink sink) {
        progress_sink_.emplace(std::move(sink));
        wants_progress_ = true;
    }

    /// Write one JSON-RPC notification that expects no answer, and discard its
    /// outcome. The notification carries no caller stop token, because the one
    /// caller that motivates it has already stopped: `notifications/cancelled`
    /// is how a stopped call tells the Upstream to stop the work the closed
    /// response stream abandoned, and a transport that refused to write it for
    /// that reason would send nothing at all (ADR 0020; spec #833 story 30).
    /// The write is bounded by the per-call deadline and is best-effort by
    /// construction — a notification has no response to be late for — so it can
    /// never hold up the call that is already settling.
    void notify(std::string method, JsonValue params) {
        auto connection = connection_;
        // The notification resolves its credential under *no* caller token.
        // `resolve_upstream_auth` refuses a token that is already stopped, and
        // the one caller this exists for has always stopped: refusing here
        // would mean the notification is exactly the one thing a cancelled
        // call cannot send.
        resolve_auth(std::stop_token{},
                [connection, method = std::move(method), params = std::move(params)](
                        Expected<UpstreamAuth> auth) mutable noexcept {
            if (!auth) {
                return; // a credential that did not resolve stops the write; the call settles either way
            }
            McpRequest request;
            request.url = connection->options.url;
            request.headers = connection->era ? connection->era->request_headers(method, {})
                                              : era::modern_request_headers(method, {});
            auto body = support::write_json(jsonrpc::encode_notification(method, std::move(params)));
            if (!body) {
                return;
            }
            request.body = std::move(*body);
            request.timeout = connection->options.request_timeout;
            apply_upstream_auth(request, *auth);
            connection->resolved_bearer = auth->bearer;
            connection->transport->send(std::move(request))
                    .start([](std::expected<McpResponse, Error>) noexcept {});
        });
    }

private:
    /// The progress token this operation declared, empty when it declared none.
    [[nodiscard]] const std::string& progress_token() const noexcept { return progress_token_; }
    /// Resolve this connection's live credential and post `on_ready` on the
    /// operation's own queue, so a store that completes inline cannot nest
    /// exchanges and one that completes later cannot re-enter the operation.
    /// `token` is the cancellation the resolution runs under, which is the
    /// operation's own token everywhere except the `notifications/cancelled`
    /// write — see `notify`.
    void resolve_auth(std::stop_token token, std::move_only_function<void(Expected<UpstreamAuth>)> on_ready) {
        const auto& connection = connection_;
        // A declared environment reference is authoritative (issue #838); an
        // OAuth issuer is what `/mcp auth` established instead (issue #849).
        // Exactly one of them resolves, so a request is never authenticated
        // twice from two different credentials.
        auto resolution =
                connection->options.bearer_env_var.has_value() ? resolve_upstream_auth(connection->server_id,
                                                                         connection->options.bearer_env_var,
                                                                         connection->options.credentials,
                                                                         token)
                : connection->options.oauth_issuer.has_value()
                        ? resolve_oauth_bearer(connection->server_id,
                                  *connection->options.oauth_issuer,
                                  connection->options.credentials,
                                  token)
                        : resolve_upstream_auth(
                                  connection->server_id, std::nullopt, connection->options.credentials, token);
        resolution.start([self = Operation::shared_from_this(), on_ready = std::move(on_ready)](
                                 std::expected<UpstreamAuth, Error> auth) mutable noexcept {
            self->queue_.post(
                    [on_ready = std::move(on_ready), auth = std::move(auth)]() mutable { on_ready(std::move(auth)); });
        });
    }

    /// Frame and write the exchange `send` was asked for, with the resolved
    /// credential applied to the request headers. A credential that did not
    /// resolve fails the exchange through the connection-failure path without
    /// writing anything.
    void dispatch(JsonValue params,
            std::string name,
            std::map<std::string, std::string> extra_headers,
            ExchangeOutcomeHandler on_outcome,
            std::vector<jsonrpc::RawJsonMember> raw_params,
            Expected<UpstreamAuth> auth) {
        auto connection = connection_;
        if (!auth) {
            auto error = auth.error();
            queue_.post([on_outcome = std::move(on_outcome), error = std::move(error)]() mutable {
                on_outcome(std::move(error));
            });
            return;
        }
        connection->resolved_bearer = auth->bearer;

        const auto id = next_request_id(connection);
        on_request_id(id);
        // The era probe is the one exchange that runs before the connection has
        // selected an adapter, so it frames itself with the Modern Era request
        // shape directly. Everything after it frames through the selected
        // adapter, which a deferred Legacy Era adapter would replace.
        auto headers = connection->era ? connection->era->request_headers(method_, name)
                                       : era::modern_request_headers(method_, name);
        if (connection->era) {
            connection->era->attach_request_meta(params);
        } else {
            era::attach_modern_request_meta(params);
        }
        if (wants_progress_) {
            // The id is declared as the call's progress token once the era's own
            // reserved `_meta` keys are in place, so the token on the wire and
            // the token every `notifications/progress` is matched against are
            // the same value by construction rather than by agreement.
            progress_token_ = protocol::progress_token(id).get_string();
            attach_progress_token(params, JsonValue(progress_token_));
        }
        for (auto& [key, value] : extra_headers) {
            headers.insert_or_assign(std::move(key), std::move(value));
        }
        auto body = jsonrpc::encode_request_body(id, method_, params, raw_params);
        if (!body) {
            auto error = body.error();
            queue_.post([on_outcome = std::move(on_outcome), error = std::move(error), secret = secret()]() mutable {
                on_outcome(ProtocolFailure{.diagnostic = diagnostic_of(error, secret), .json_rpc_code = 0});
            });
            return;
        }

        McpRequest request;
        request.url = connection->options.url;
        request.headers = std::move(headers);
        request.body = std::move(*body);
        request.timeout = connection->options.request_timeout;
        request.stop_token = stop_token_;
        apply_upstream_auth(request, *auth);

        auto pending = connection->transport->send(std::move(request));
        pending.start([self = Operation::shared_from_this(), connection, id, on_outcome = std::move(on_outcome)](
                              std::expected<McpResponse, Error> answer) mutable noexcept {
            const auto secret = self->secret();
            auto outcome = read_exchange_outcome(id,
                    answer,
                    secret,
                    ProgressObserver{.sink = self->progress_sink_ ? &*self->progress_sink_ : nullptr,
                            .token = self->progress_token_},
                    connection->options.auth_challenge_sink.has_value() ? &*connection->options.auth_challenge_sink
                                                                        : nullptr);
            self->queue_.post([on_outcome = std::move(on_outcome), outcome = std::move(outcome)]() mutable {
                on_outcome(std::move(outcome));
            });
        });
    }

    /// Probe `server/discover` and continue. The probe owns its own
    /// completion: the operation's terminal completion belongs to the
    /// operation, so it is never handed to the probe. Defined below
    /// `ProbeOperation`.
    void run_probe();

    ExchangeQueue queue_;
    std::shared_ptr<UpstreamClient::Connection> connection_;
    std::string method_;
    std::stop_token stop_token_;
    /// Whether this operation declares a progress token, and the token it
    /// declared. Empty for every operation that reports no progress, which is
    /// every operation but a `tools/call` that was given a sink.
    bool wants_progress_{false};
    std::string progress_token_{};
    /// The caller's progress sink, or none. It is a member rather than a
    /// borrowed one so that the same sink serves every notification of the call
    /// and so a notification arriving after the call settled finds the
    /// operation's own state rather than a dangling reference.
    std::optional<UpstreamProgressSink> progress_sink_{};
    bool completed_{false};
};

/// The era probe: `server/discover` selects this connection's adapter. It
/// completes through a plain callback so the same operation serves both the
/// explicit `probe_era` call and the probe another operation needed.
class ProbeOperation final : public Operation {
public:
    ProbeOperation(std::shared_ptr<UpstreamClient::Connection> connection, std::stop_token token)
        : Operation(std::move(connection), protocol::kMethodDiscover, token) {}

    void begin(std::move_only_function<void(std::expected<UpstreamServerInfo, Error>)> on_done) {
        on_done_ = std::move(on_done);
        on_era_selected();
    }

    void start(AsyncCompletion<UpstreamServerInfo, Error> completion) {
        begin([completion = std::move(completion)](std::expected<UpstreamServerInfo, Error> outcome) mutable noexcept {
            completion(std::move(outcome));
        });
    }

protected:
    void on_era_selected() override {
        send(JsonValue::object_t{}, {}, {}, [self = self<ProbeOperation>()](ExchangeOutcome outcome) mutable {
            self->absorb(std::move(outcome));
        });
    }

    void on_probe_failed(Error error) override { deliver(std::unexpected(std::move(error))); }

private:
    void absorb(ExchangeOutcome outcome) {
        if (const auto* error = std::get_if<Error>(&outcome); error != nullptr) {
            return deliver(std::unexpected(*error));
        }
        if (const auto* failure = std::get_if<ProtocolFailure>(&outcome); failure != nullptr) {
            return deliver(std::unexpected(make_error(
                    ErrorCode::Validation, "the Upstream MCP Server era probe failed", failure->diagnostic)));
        }
        const auto& result = std::get<ExchangeResult>(outcome).value;
        auto adapter = era::select_era_adapter(result);
        if (!adapter) {
            return deliver(std::unexpected(redacted_error(adapter.error(), secret())));
        }
        auto info = (*adapter)->read_probe_result(result);
        if (!info) {
            return deliver(std::unexpected(redacted_error(info.error(), secret())));
        }
        connection()->era = std::move(*adapter);
        connection()->instructions = info->instructions;
        deliver(std::move(*info));
    }

    void deliver(Expected<UpstreamServerInfo> outcome) {
        if (!claim_completion()) {
            return;
        }
        auto on_done = std::move(on_done_);
        on_done(std::move(outcome));
    }

    std::move_only_function<void(std::expected<UpstreamServerInfo, Error>)> on_done_{};
};

void Operation::run_probe() {
    auto self = Operation::shared_from_this();
    auto probe = std::make_shared<ProbeOperation>(connection_, stop_token_);
    probe->begin([self = std::move(self)](std::expected<UpstreamServerInfo, Error> probed) mutable noexcept {
        if (!probed) {
            self->on_probe_failed(probed.error());
            return;
        }
        self->on_era_selected();
    });
}

/// The catalog walk: `tools/list` to the end of the Upstream's catalog under
/// the per-Upstream tool cap and the pagination-cursor cap.
class ListToolsOperation final : public Operation {
public:
    ListToolsOperation(std::shared_ptr<UpstreamClient::Connection> connection, std::stop_token token)
        : Operation(std::move(connection), protocol::kMethodListTools, token) {}

    void start(AsyncCompletion<UpstreamCatalog, Error> completion) {
        completion_ = std::move(completion);
        start_after_era();
    }

protected:
    void on_era_selected() override { request_page(); }

    void on_probe_failed(Error error) override { complete_with(std::unexpected(std::move(error))); }

private:
    void request_page() {
        JsonValue params = JsonValue::object_t{};
        if (cursor_.has_value()) {
            params.get_object().emplace("cursor", JsonValue(*cursor_));
        }
        send(std::move(params), {}, {}, [self = self<ListToolsOperation>()](ExchangeOutcome outcome) mutable {
            self->absorb_page(std::move(outcome));
        });
    }

    void absorb_page(ExchangeOutcome outcome) {
        if (const auto* error = std::get_if<Error>(&outcome); error != nullptr) {
            return complete_with(std::unexpected(*error));
        }
        if (const auto* failure = std::get_if<ProtocolFailure>(&outcome); failure != nullptr) {
            return complete_with(std::unexpected(make_error(
                    ErrorCode::Validation, "the Upstream MCP Server tools/list request failed", failure->diagnostic)));
        }
        auto page = dto::read_tool_list_page(std::get<ExchangeResult>(outcome).value);
        if (!page) {
            return complete_with(std::unexpected(redacted_error(page.error(), secret())));
        }
        if (catalog_.tools.size() + page->tools.size() > protocol::kMaxToolsPerUpstream) {
            return complete_with(std::unexpected(make_error(ErrorCode::ResourceLimit,
                    "the Upstream MCP Server advertises more tools than the MCP Host admits",
                    "the " + std::to_string(protocol::kMaxToolsPerUpstream) + "-tool per-server cap is exceeded")));
        }
        for (auto& tool : page->tools) {
            if (!tool_names_.insert(tool.name).second) {
                return complete_with(std::unexpected(make_error(ErrorCode::Validation,
                        "the Upstream MCP Server advertised a duplicate tool name",
                        "the walk stops at the duplicate; the untrusted name is not echoed")));
            }
            catalog_.tools.push_back(std::move(tool));
        }
        pages_ += 1;
        if (!page->next_cursor.has_value()) {
            catalog_.instructions = connection()->instructions;
            // The freshness hint and the reuse scope are read from the page
            // that completes the walk: a hint on a page that still carried a
            // `nextCursor` describes an incomplete catalog (issue #848).
            catalog_.freshness = page->freshness;
            catalog_.cache_scope = page->cache_scope.value_or(std::string{kDefaultCatalogCacheScope});
            return complete_with(std::move(catalog_));
        }
        if (pages_ >= protocol::kMaxListPagesPerUpstream) {
            return complete_with(std::unexpected(make_error(ErrorCode::ResourceLimit,
                    "the Upstream MCP Server paginates past the MCP Host's catalog bound",
                    "the " + std::to_string(protocol::kMaxListPagesPerUpstream) + "-page cursor cap is exceeded")));
        }
        if (!cursor_known_.insert(*page->next_cursor).second) {
            return complete_with(std::unexpected(make_error(ErrorCode::Validation,
                    "the Upstream MCP Server returned a pagination cursor it already returned",
                    "a repeated cursor is a catalog loop and is not followed")));
        }
        cursor_ = *page->next_cursor;
        self<ListToolsOperation>()->request_page();
    }

    void complete_with(Expected<UpstreamCatalog> outcome) {
        if (!claim_completion()) {
            return;
        }
        auto completion = std::move(completion_);
        completion(std::move(outcome));
    }

    AsyncCompletion<UpstreamCatalog, Error> completion_{};
    UpstreamCatalog catalog_{};
    std::set<std::string> tool_names_{};
    std::set<std::string> cursor_known_{};
    std::optional<std::string> cursor_{};
    std::size_t pages_{0};
};

/// One `tools/call`, including the `Mcp-Param-*` mirroring of the tool's
/// validated `x-mcp-header` annotation, the `progressToken` the call declares
/// so an Upstream's progress is attributable to it, the
/// `notifications/cancelled` a cancelled call sends so the Upstream stops the
/// work behind the closed response stream, and the Multi Round-Trip loop
/// (issue #845, spec #833 stories 26-29).
///
/// The loop is the whole of the MRTR contract and it is **mode-agnostic**:
/// a suspended result is asked of the port, the answers are collected, and the
/// original `name` and `arguments` go back out under a **new** JSON-RPC id
/// with the opaque `requestState` echoed verbatim. Adding form mode therefore
/// changes the port and the rendering, never this loop.
class CallToolOperation final : public Operation {
public:
    CallToolOperation(std::shared_ptr<UpstreamClient::Connection> connection,
            UpstreamToolCall call,
            std::stop_token token,
            UpstreamProgressSink progress_sink)
        : Operation(std::move(connection), protocol::kMethodCallTool, token), call_(std::move(call)) {
        if (progress_sink) {
            report_progress_through(std::move(progress_sink));
        }
    }

    void start(AsyncCompletion<UpstreamToolCallResult, Error> completion) {
        completion_ = std::move(completion);
        start_after_era();
    }

protected:
    void on_era_selected() override { issue_call({}); }

    void on_probe_failed(Error error) override { complete_with(std::unexpected(std::move(error))); }

    void on_request_id(double id) override { request_id_ = id; }

private:
    /// One answer the user gave, ready to be echoed on the retried request.
    struct PendingAnswer {
        std::string request_id{};
        ElicitationAction action{ElicitationAction::Accept};
        JsonValue content{};
    };

    /// Everything a retry contributes that the original call did not.
    struct RetryCarry {
        /// The Upstream's opaque continuation token, exactly as it arrived.
        std::string request_state{};
        std::vector<PendingAnswer> answers{};
    };

    /// One question of a suspended result, held across the wait so the answer
    /// arrives with the question it belongs to. The loop must not recurse
    /// through a wait: a chain that suspends per question would grow a stack
    /// with the number of questions a server asked.
    struct AskState {
        dto::ToolCallInputRequest suspended{};
        RetryCarry carry{};
        std::size_t index{0};
    };

    /// The wait's own cancellation, which the call's stop token drives and the
    /// loop stops when it stops waiting.
    struct WaitControl {
        struct Forward {
            std::shared_ptr<WaitControl> control;
            void operator()() const noexcept { control->source.request_stop(); }
        };

        std::stop_source source{};
        std::optional<std::stop_callback<Forward>> join{};
    };

    /// Issue the call, carrying `carry` on the wire when this is a retry.
    void issue_call(const RetryCarry& carry) {
        auto mirrored = headers::mirror_parameter_headers(call_.tool, call_.arguments);
        if (!mirrored) {
            return complete_with(std::unexpected(mirrored.error()));
        }
        // The original `name` and `arguments` are re-sent unchanged on every
        // round: a retry that changed them would be a different call, and the
        // Upstream correlates the exchange by the token, not by the arguments.
        JsonValue params = JsonValue::object_t{
                {"name", JsonValue(call_.tool.name)},
                {"arguments", call_.arguments},
        };
        std::vector<jsonrpc::RawJsonMember> raw_params;
        if (!carry.answers.empty()) {
            JsonValue::array_t responses;
            responses.reserve(carry.answers.size());
            for (const auto& answer : carry.answers) {
                JsonValue::object_t entry{
                        {"action", JsonValue(std::string(to_string(answer.action)))},
                };
                if (!answer.request_id.empty()) {
                    entry.emplace(protocol::kInputResponseId, JsonValue(answer.request_id));
                }
                if (!answer.content.holds<JsonValue::null_t>()) {
                    entry.emplace(protocol::kInputResponseContent, answer.content);
                }
                responses.emplace_back(std::move(entry));
            }
            params.get_object().emplace(protocol::kCallInputResponses, JsonValue(std::move(responses)));
            // The continuation token is written as the text the Upstream sent,
            // so a server that compares it byte-for-byte sees the same token.
            raw_params.push_back(jsonrpc::RawJsonMember{
                    .key = std::string{protocol::kCallRequestState}, .json_text = carry.request_state});
        }
        send(
                std::move(params),
                call_.tool.name,
                std::move(*mirrored),
                [self = self<CallToolOperation>()](
                        ExchangeOutcome outcome) mutable { self->absorb(std::move(outcome)); },
                std::move(raw_params));
    }

    void absorb(ExchangeOutcome outcome) {
        if (const auto* error = std::get_if<Error>(&outcome); error != nullptr) {
            // A cancelled call is the caller's decision, and the Upstream is
            // told about it: the response stream is already gone, so the one
            // signal left that reaches the work behind it is written here,
            // before this call settles (ADR 0020; spec #833 story 30).
            if (error->code == ErrorCode::Cancelled) {
                notify_cancelled();
            }
            return complete_with(std::unexpected(*error)); // transport failure, cancellation included
        }
        if (const auto* failure = std::get_if<ProtocolFailure>(&outcome); failure != nullptr) {
            request_id_.reset();
            return complete_with(failed(diagnostic_for(*failure)));
        }
        auto result = std::get<ExchangeResult>(outcome);
        // The exchange this id named has now answered, so nothing is
        // outstanding: a stop that lands during a later Multi Round-Trip wait,
        // before the next round is framed, must not make `notify_cancelled`
        // name a request the Upstream already completed. The next round stamps
        // a fresh id in `on_request_id`, which is the one a cancellation names.
        request_id_.reset();
        if (dto::is_input_required(result.value)) {
            return suspend(std::move(result));
        }
        auto completed = dto::read_tool_call_result(std::move(result.value));
        if (!completed) {
            return complete_with(failed(diagnostic_of(completed.error(), secret())));
        }
        // The Upstream's own content is handed on with the connection's
        // credential value erased from it, so a tool result that echoed the
        // bearer back cannot carry it into the model's context or a transcript
        // (issue #838).
        complete_with(UpstreamToolCallResult{
                .is_error = completed->is_error,
                .content = redaction::redacted_value(std::move(completed->content), secret()),
                .diagnostic = {},
        });
    }

    /// Tell the Upstream to stop the work this call asked for. Written only for
    /// a cancellation that happened after the request reached the wire — a call
    /// cancelled before it was framed has nothing on the Upstream to stop. The
    /// reason is the one bounded, redacted diagnostic every other explanation
    /// in this package carries, so a server that logs it logs no more than a
    /// session record would.
    void notify_cancelled() {
        if (!request_id_.has_value()) {
            return;
        }
        notify(std::string(protocol::kMethodCancelled),
                JsonValue::object_t{
                        {std::string(protocol::kParamRequestId), JsonValue(*request_id_)},
                        {std::string(protocol::kParamReason),
                                JsonValue(bounded_diagnostic("the MCP Host cancelled the call", secret()))},
                });
    }

    /// An `input_required` result suspended the call. The defensive matrix
    /// decides first whether this build can answer at all, so a non-conformant
    /// server fails one call rather than a session (ADR 0008, story 32).
    void suspend(ExchangeResult result) {
        auto* port = elicitation_port();
        if (port == nullptr || !port->prompter) {
            return complete_with(failed(diagnostics::bounded(
                    "the Upstream MCP Server suspended the call for client input with an \"input_required\" result, "
                    "but this session has no Pending Elicitation surface to ask",
                    secret())));
        }
        if (rounds_ >= protocol::kMaxElicitationRounds) {
            return complete_with(failed(diagnostics::bounded(
                    "the Upstream MCP Server suspended the call for client input past the MCP Host's Multi "
                    "Round-Trip bound",
                    secret())));
        }
        auto suspended = dto::read_input_required_result(result.value, result.raw);
        if (!suspended) {
            return complete_with(failed(diagnostic_of(suspended.error(), secret())));
        }
        for (const auto& request : suspended->requests) {
            if (request.type == dto::InputRequestType::Undeclared) {
                // An undeclared input-request type is not a mode this build
                // guesses at: one failed call, and the session is untouched.
                return complete_with(failed(diagnostics::bounded(
                        "the Upstream MCP Server asked for an input request of a type this build does not declare",
                        secret())));
            }
        }
        ++rounds_;
        RetryCarry carry;
        carry.request_state = std::move(suspended->request_state);
        ask(std::move(*suspended), std::move(carry), 0);
    }

    /// Ask the port for the answers to the suspended result's requests, one at
    /// a time and in order. A request is asked only after the previous one is
    /// answered, so a server that asks several questions costs the user a
    /// sequence of decisions rather than an unreadable pile.
    ///
    /// The wait is **bounded**: the port's timer races the prompter, and the
    /// first of the two to complete ends the wait. A wait that reaches its
    /// bound produces no re-send, so a user who walks away costs one failed
    /// tool call and never a suspended call that never returns (story 29).
    void ask(dto::ToolCallInputRequest suspended, RetryCarry carry, std::size_t index) {
        const auto& request = suspended.requests[index];
        auto presented = ElicitationRequest{
                .server_id = connection()->server_id,
                .tool_name = call_.tool.name,
                .mode = request.type == dto::InputRequestType::Form ? ElicitationMode::Form : ElicitationMode::Url,
                .request_id = request.id,
                .message = request.message,
                .url = request.url,
                .form_schema = request.form_schema,
        };
        auto* port = elicitation_port();
        auto operation = self<CallToolOperation>();
        // The wait's own stop source is what the port is asked under, and it
        // is stopped whenever the loop stops waiting. That is how an
        // abandoned question is withdrawn: the port learns the call is over
        // and stops showing a question nobody can answer any more, which is
        // the difference between a bounded wait and a stale dialog.
        auto wait = std::make_shared<WaitControl>();
        wait->join.emplace(stop_token(), WaitControl::Forward{wait});
        auto asked = port->prompter(std::move(presented), wait->source.get_token());
        // Both completions are guarded by one flag, so a timer that fires
        // after the prompter already answered — or an answer that arrives after
        // the bound — is discarded rather than completing the call twice.
        auto settled = std::make_shared<std::atomic<bool>>(false);
        auto held = std::make_shared<AskState>(AskState{
                .suspended = std::move(suspended),
                .carry = std::move(carry),
                .index = index,
        });
        asked.start([operation, settled, held](std::expected<ElicitationAnswer, Error> answer) mutable noexcept {
            if (settled->exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            operation->answered(std::move(answer), std::move(*held));
        });
        if (!port->delay) {
            return; // the wait is bounded only by the caller's stop token
        }
        const auto bound = std::min(
                port->timeout <= std::chrono::milliseconds{0} ? protocol::kDefaultElicitationTimeout : port->timeout,
                protocol::kMaxElicitationTimeout);
        auto elapsed_wait = port->delay(bound, stop_token());
        elapsed_wait.start([operation, settled, bound, wait](std::expected<void, Error> elapsed) mutable noexcept {
            if (settled->exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            wait->source.request_stop();
            if (!elapsed) {
                // A cancelled timer is the session stopping the wait, which
                // ends the call the same way an unanswered bound does.
                operation->abandon_wait(elapsed.error());
                return;
            }
            operation->abandon_wait(make_error(ErrorCode::Timeout,
                    "the Pending Elicitation was not answered within the MCP Host's bound",
                    "the wait for the user's answer reached the " + std::to_string(bound.count()) +
                            "ms elicitation bound; the suspended call was not re-sent"));
        });
    }

    /// The wait ended without an answer, so the suspended call is over and
    /// nothing is re-sent in the user's name.
    void abandon_wait(Error error) { complete_with(failed(diagnostic_of(error, secret()))); }

    void answered(std::expected<ElicitationAnswer, Error> answer, AskState state) {
        if (!answer) {
            // A wait that failed — cancelled, or a port that could not ask —
            // ends the call. There is no re-send: the user never answered, so
            // anything sent in their name would be a fabrication.
            return complete_with(failed(diagnostic_of(answer.error(), secret())));
        }
        state.carry.answers.push_back(PendingAnswer{
                .request_id = state.suspended.requests[state.index].id,
                .action = answer->action,
                .content = answer->form_content,
        });
        const auto next = state.index + 1;
        if (next < state.suspended.requests.size()) {
            state.index = next;
            return ask(std::move(state.suspended), std::move(state.carry), next);
        }
        issue_call(std::move(state.carry));
    }

    /// The defensive matrix, named where each case is mapped (ADR 0064,
    /// ADR 0008). Every case fails exactly one tool call, and a later ordinary
    /// call on the same connection still succeeds.
    [[nodiscard]] std::string diagnostic_for(const ProtocolFailure& failure) const {
        if (failure.json_rpc_code == protocol::kErrorMissingRequiredClientCapability) {
            return bounded_diagnostic(
                    "the Upstream MCP Server requires a client capability this build does not advertise "
                    "(JSON-RPC -32021 MissingRequiredClientCapability)",
                    secret());
        }
        return failure.diagnostic;
    }

    [[nodiscard]] static Expected<UpstreamToolCallResult> failed(std::string diagnostic) {
        return UpstreamToolCallResult{.is_error = true, .content = JsonValue{}, .diagnostic = std::move(diagnostic)};
    }

    void complete_with(Expected<UpstreamToolCallResult> outcome) {
        if (!claim_completion()) {
            return; // a late answer to a call that already settled
        }
        auto completion = std::move(completion_);
        completion(std::move(outcome));
    }

    AsyncCompletion<UpstreamToolCallResult, Error> completion_{};
    UpstreamToolCall call_{};
    /// The JSON-RPC id of this call's **current** exchange, which is also the
    /// request id a `notifications/cancelled` names. Absent when the call
    /// never reached the wire. It is re-stamped on every MRTR round, because
    /// each retried `tools/call` goes out under a fresh JSON-RPC id (spec
    /// #833 story 28) and the notification must name the id that is actually
    /// outstanding.
    std::optional<double> request_id_{};
    /// How many `input_required` results this call has already been suspended
    /// by. Bounded by `kMaxElicitationRounds`, so a server that keeps asking
    /// costs a bounded amount of the user's attention.
    std::size_t rounds_{0};
};

/// Start one operation, owning the connection it runs on for as long as the
/// operation is in flight.
template <typename T, typename Start>
[[nodiscard]] support::AsyncResult<T> start_operation(
        std::shared_ptr<UpstreamClient::Connection> connection, std::stop_token stop_token, Start start) {
    using Result = support::AsyncResult<T>;
    return Result(
            typename Result::producer_type([connection = std::move(connection), stop_token, start = std::move(start)](
                                                   AsyncCompletion<T, Error> completion) mutable noexcept {
                start(std::move(connection), stop_token, std::move(completion));
            }));
}

} // namespace

UpstreamClient::UpstreamClient(
        std::string server_id, std::shared_ptr<McpTransport> transport, UpstreamClientOptions options)
    : server_id_(std::move(server_id)) {
    connection_ = std::make_shared<Connection>();
    connection_->server_id = server_id_;
    connection_->transport = std::move(transport);
    connection_->options = std::move(options);
}

std::string_view UpstreamClient::era_name() const noexcept {
    return connection_->era == nullptr ? std::string_view{} : connection_->era->name();
}

AsyncResult<UpstreamServerInfo> UpstreamClient::probe_era(std::stop_token stop_token) {
    return start_operation<UpstreamServerInfo>(connection_,
            stop_token,
            [](auto connection,
                    std::stop_token token,
                    AsyncCompletion<UpstreamServerInfo, Error> completion) mutable noexcept {
                auto operation = std::make_shared<ProbeOperation>(std::move(connection), token);
                operation->start(std::move(completion));
            });
}

AsyncResult<UpstreamCatalog> UpstreamClient::list_tools(std::stop_token stop_token) {
    return start_operation<UpstreamCatalog>(connection_,
            stop_token,
            [](auto connection,
                    std::stop_token token,
                    AsyncCompletion<UpstreamCatalog, Error> completion) mutable noexcept {
                auto operation = std::make_shared<ListToolsOperation>(std::move(connection), token);
                operation->start(std::move(completion));
            });
}

AsyncResult<UpstreamToolCallResult> UpstreamClient::call_tool(
        UpstreamToolCall call, std::stop_token stop_token, UpstreamProgressSink progress_sink) {
    return start_operation<UpstreamToolCallResult>(connection_,
            stop_token,
            [call = std::move(call), progress_sink = std::move(progress_sink)](
                    auto connection,
                    std::stop_token token,
                    AsyncCompletion<UpstreamToolCallResult, Error> completion) mutable noexcept {
                auto operation = std::make_shared<CallToolOperation>(
                        std::move(connection), std::move(call), token, std::move(progress_sink));
                operation->start(std::move(completion));
            });
}

} // namespace cch::mcp
