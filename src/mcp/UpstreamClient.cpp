#include <cch/mcp/UpstreamClient.hpp>

#include "mcp/EraAdapter.hpp"
#include "mcp/HeaderMirror.hpp"
#include "mcp/JsonRpc.hpp"
#include "mcp/Protocol.hpp"
#include "mcp/Redaction.hpp"
#include "mcp/WireDto.hpp"
#include "support/Json.hpp"

#include <atomic>
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

/// The decoded `result` of a JSON-RPC exchange, the Upstream's protocol
/// violation, or the transport's own failure.
using ExchangeOutcome = std::variant<JsonValue, ProtocolFailure, Error>;
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
/// back cannot put it in a diagnostic (issue #838).
[[nodiscard]] std::string bounded_diagnostic(std::string text, std::string_view secret) {
    return redaction::redacted_text(std::move(text), secret);
}

[[nodiscard]] std::string diagnostic_of(const Error& error, std::string_view secret) {
    return bounded_diagnostic(error.detail.empty() ? error.message : error.message + ": " + error.detail, secret);
}

/// An error leaving this package, redacted by the connection's resolved
/// credential value and bounded before it reaches a status surface or a
/// session record (issue #838).
[[nodiscard]] Error redacted_error(const Error& error, std::string_view secret) {
    return make_error(error.code,
            bounded_diagnostic(error.message, secret),
            bounded_diagnostic(error.detail, secret));
}

[[nodiscard]] double next_request_id(const std::shared_ptr<UpstreamClient::Connection>& connection) {
    return static_cast<double>(connection->last_request_id.fetch_add(1) + 1);
}

/// Read the one response that belongs to `id` out of a transport answer.
/// Notifications ahead of it — including `tools/list_changed` — are safely
/// ignored: pike does not subscribe to `subscriptions/listen`, so the
/// notification neither reconnects the connection nor refreshes the catalog.
[[nodiscard]] ExchangeOutcome read_exchange_outcome(
        double id, const std::expected<McpResponse, Error>& answer, std::string_view secret) {
    if (!answer) {
        return answer.error();
    }
    const auto& response = *answer;
    if (response.status_code < 200 || response.status_code > 299) {
        return ProtocolFailure{
                .diagnostic = bounded_diagnostic(
                        "the Upstream MCP Server answered with HTTP status " + std::to_string(response.status_code),
                        secret),
                .json_rpc_code = 0,
        };
    }
    auto messages = jsonrpc::split_messages(response.body);
    if (!messages) {
        return ProtocolFailure{.diagnostic = diagnostic_of(messages.error(), secret), .json_rpc_code = 0};
    }
    for (const auto& raw : *messages) {
        auto decoded = jsonrpc::decode_message(raw);
        if (!decoded) {
            return ProtocolFailure{.diagnostic = diagnostic_of(decoded.error(), secret), .json_rpc_code = 0};
        }
        if (decoded->is_notification()) {
            continue;
        }
        if (decoded->id != id) {
            continue; // a stale response for an abandoned request
        }
        if (decoded->is_error) {
            return ProtocolFailure{
                    .diagnostic = bounded_diagnostic(
                            "the Upstream MCP Server returned JSON-RPC error " +
                                    std::to_string(decoded->error_code) + ": " + decoded->error_message,
                            secret),
                    .json_rpc_code = decoded->error_code,
            };
        }
        return decoded->result;
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

    /// Issue one framed JSON-RPC request and hand its outcome to
    /// `on_outcome`, attributed to the derived operation's own chain.
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
            ExchangeOutcomeHandler on_outcome) {
        auto connection = connection_;
        resolve_auth([self = Operation::shared_from_this(),
                             params = std::move(params),
                             name = std::string{name},
                             extra_headers = std::move(extra_headers),
                             on_outcome = std::move(on_outcome)](Expected<UpstreamAuth> auth) mutable {
            self->queue_.post([self = std::move(self),
                                     params = std::move(params),
                                     name = std::move(name),
                                     extra_headers = std::move(extra_headers),
                                     on_outcome = std::move(on_outcome),
                                     auth = std::move(auth)]() mutable {
                self->dispatch(std::move(params),
                        std::move(name),
                        std::move(extra_headers),
                        std::move(on_outcome),
                        std::move(auth));
            });
        });
    }

    /// The operation's real work, reached once the era is selected.
    virtual void on_era_selected() = 0;

    /// A probe this operation needed failed, so the operation has no era to
    /// run on and completes with the probe's error.
    virtual void on_probe_failed(Error error) = 0;

private:
    /// Resolve this connection's live credential and hand it to `on_ready`,
    /// posted through the operation's own queue so a store that completes
    /// inline cannot nest exchanges.
    void resolve_auth(std::move_only_function<void(Expected<UpstreamAuth>)> on_ready) {
        const auto& connection = connection_;
        auto resolution = resolve_upstream_auth(connection->server_id,
                connection->options.bearer_env_var,
                connection->options.credentials,
                stop_token_);
        resolution.start(
                [on_ready = std::move(on_ready)](std::expected<UpstreamAuth, Error> auth) mutable noexcept {
                    on_ready(std::move(auth));
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
        for (auto& [key, value] : extra_headers) {
            headers.insert_or_assign(std::move(key), std::move(value));
        }
        auto body = support::write_json(jsonrpc::encode_request(id, method_, std::move(params)));
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
        pending.start([self = Operation::shared_from_this(), id, on_outcome = std::move(on_outcome)](
                              std::expected<McpResponse, Error> answer) mutable noexcept {
            const auto secret = self->secret();
            auto outcome = read_exchange_outcome(id, answer, secret);
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
        const auto& result = std::get<JsonValue>(outcome);
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
        auto page = dto::read_tool_list_page(std::get<JsonValue>(outcome));
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
/// validated `x-mcp-header` annotation.
class CallToolOperation final : public Operation {
public:
    CallToolOperation(
            std::shared_ptr<UpstreamClient::Connection> connection, UpstreamToolCall call, std::stop_token token)
        : Operation(std::move(connection), protocol::kMethodCallTool, token), call_(std::move(call)) {}

    void start(AsyncCompletion<UpstreamToolCallResult, Error> completion) {
        completion_ = std::move(completion);
        start_after_era();
    }

protected:
    void on_era_selected() override { issue_call(); }

    void on_probe_failed(Error error) override { complete_with(std::unexpected(std::move(error))); }

private:
    void issue_call() {
        auto mirrored = headers::mirror_parameter_headers(call_.tool, call_.arguments);
        if (!mirrored) {
            return complete_with(std::unexpected(mirrored.error()));
        }
        JsonValue params = JsonValue::object_t{
                {"name", JsonValue(call_.tool.name)},
                {"arguments", call_.arguments},
        };
        send(std::move(params),
                call_.tool.name,
                std::move(*mirrored),
                [self = self<CallToolOperation>()](
                        ExchangeOutcome outcome) mutable { self->absorb(std::move(outcome)); });
    }

    void absorb(ExchangeOutcome outcome) {
        if (const auto* error = std::get_if<Error>(&outcome); error != nullptr) {
            return complete_with(std::unexpected(*error)); // transport failure, cancellation included
        }
        if (const auto* failure = std::get_if<ProtocolFailure>(&outcome); failure != nullptr) {
            return complete_with(failed(diagnostic_for(*failure)));
        }
        auto result = dto::read_tool_call_result(std::get<JsonValue>(outcome));
        if (!result) {
            return complete_with(failed(diagnostic_of(result.error(), secret())));
        }
        // The Upstream's own content is handed on with the connection's
        // credential value erased from it, so a tool result that echoed the
        // bearer back cannot carry it into the model's context or a transcript
        // (issue #838).
        complete_with(UpstreamToolCallResult{
                .is_error = result->is_error,
                .content = redaction::redacted_value(std::move(result->content), secret()),
                .diagnostic = {},
        });
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
        auto completion = std::move(completion_);
        completion(std::move(outcome));
    }

    AsyncCompletion<UpstreamToolCallResult, Error> completion_{};
    UpstreamToolCall call_{};
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

AsyncResult<UpstreamToolCallResult> UpstreamClient::call_tool(UpstreamToolCall call, std::stop_token stop_token) {
    return start_operation<UpstreamToolCallResult>(connection_,
            stop_token,
            [call = std::move(call)](auto connection,
                    std::stop_token token,
                    AsyncCompletion<UpstreamToolCallResult, Error> completion) mutable noexcept {
                auto operation = std::make_shared<CallToolOperation>(std::move(connection), std::move(call), token);
                operation->start(std::move(completion));
            });
}

} // namespace cch::mcp
