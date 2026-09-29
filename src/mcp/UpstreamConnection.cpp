#include <cch/mcp/UpstreamClient.hpp>
#include <cch/mcp/UpstreamConnection.hpp>

#include "mcp/Diagnostics.hpp"
#include "mcp/Protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace cch::mcp {

namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::make_error;

/// Why one request was not admitted at all. A connection that is closing or
/// closed reports the close; one the user turned off reports that instead,
/// because the two call for different things from the owner and neither is an
/// Upstream failure.
[[nodiscard]] Error not_admitted_error(const std::string& server_id, bool disabled) {
    return make_error(ErrorCode::Cancelled,
            disabled ? "the Upstream MCP Server is disabled" : "the Upstream MCP Server connection is closed",
            "Server Id \"" + server_id + "\"");
}

[[nodiscard]] Error not_connected_error(const std::string& server_id) {
    return make_error(ErrorCode::Busy,
            "the Upstream MCP Server is not connected",
            "Server Id \"" + server_id + "\" has no open connection to send the request on");
}

/// Whether one failed operation says anything about the connection rather
/// than about the Upstream's behavior. A protocol violation is the Upstream's
/// and leaves the connection usable, and a cancelled call is the caller's
/// decision; only a transport-level failure — a lost connection, an exceeded
/// deadline, a broken response stream, or an authentication challenge — is
/// health evidence.
[[nodiscard]] bool is_transport_failure(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Network:
    case ErrorCode::Timeout:
    case ErrorCode::Stream:
    case ErrorCode::Auth:
        return true;
    default:
        return false;
    }
}

} // namespace

struct UpstreamConnection::Impl : std::enable_shared_from_this<UpstreamConnection::Impl> {
    /// One admitted operation's cancellation record. Its token is the one the
    /// request carries, so the caller's cancellation and the connection's
    /// close request cancel the same call (ADR 0020).
    ///
    /// `join` holds a strong reference to the record it is registered on. A
    /// cancellation that completes the call inline therefore cannot destroy the
    /// registration while the registration is running: the record outlives the
    /// callback, and the connection drops it at the next admission, which is
    /// always outside a completion callback.
    struct Call {
        struct Forward {
            std::shared_ptr<Call> call;
            void operator()() const noexcept { call->source.request_stop(); }
        };

        std::stop_source source;
        std::optional<std::stop_callback<Forward>> join;
    };

    Impl(std::string id, std::shared_ptr<McpTransport> shared_transport, UpstreamConnectionOptions connection_options)
        : server_id(std::move(id)), transport(std::move(shared_transport)), options(std::move(connection_options)),
          request_timeout(bounded_request_timeout(options.request_timeout)),
          catalog_cache(std::move(options.catalog_cache)) {}

    // ---- reading and reporting ----

    [[nodiscard]] UpstreamConnectionSnapshot snapshot() const {
        return UpstreamConnectionSnapshot{
                .server_id = server_id,
                .status = status,
                .diagnostic = diagnostic,
                .consecutive_failures = consecutive_failures,
                .next_reconnect_delay = retry_timer.has_value() ? next_backoff() : std::chrono::milliseconds{0},
                .in_flight = in_flight,
        };
    }

    /// Move to `next` and report it. A reading that repeats the current status
    /// and diagnostic is not a transition and is not published, so a status
    /// surface sees changes rather than one event per request.
    void publish(UpstreamConnectionStatus next, std::string detail) {
        if (status == next && diagnostic == detail) {
            return;
        }
        status = next;
        diagnostic = std::move(detail);
        if (options.status_sink.has_value()) {
            (*options.status_sink)(snapshot());
        }
    }

    /// Report the current reading without changing it, for the moment a
    /// reading changed meaning rather than status: an armed reconnect is one.
    void report() {
        if (options.status_sink.has_value()) {
            (*options.status_sink)(snapshot());
        }
    }

    // ---- health ----

    /// The request path reached the Upstream and came back: the connection is
    /// not stale, and the ladder it was on is spent. Only a fresh connection
    /// attempt clears `disabled` or `needs_auth`, because neither a concurrent
    /// success nor the user turning the Upstream off again can supply what they
    /// wait for.
    void note_success() {
        if (settled()) {
            return;
        }
        cancel_retry();
        consecutive_failures = 0;
        publish(UpstreamConnectionStatus::Connected, {});
    }

    /// The request path reported a transport failure. An authentication
    /// challenge waits for the user rather than for another attempt; every
    /// other transport failure arms the ladder.
    void note_failure(const Error& error) {
        if (settled() || status == UpstreamConnectionStatus::NeedsAuth || !is_transport_failure(error.code)) {
            return;
        }
        if (error.code == ErrorCode::Auth) {
            cancel_retry();
            consecutive_failures = 0;
            publish(UpstreamConnectionStatus::NeedsAuth, diagnostics::of(error));
            return;
        }
        publish(UpstreamConnectionStatus::Failed, diagnostics::of(error));
        arm_retry();
    }

    // ---- the reconnect ladder ----

    /// The delay before the next attempt: doubling from the first rung, never
    /// past the cap.
    [[nodiscard]] std::chrono::milliseconds next_backoff() const {
        auto delay = protocol::kInitialReconnectBackoff;
        for (std::size_t rung = 1; rung < consecutive_failures; ++rung) {
            delay = std::min(delay * 2, protocol::kMaxReconnectBackoff);
        }
        return std::min(delay, protocol::kMaxReconnectBackoff);
    }

    /// Arm one reconnect. The armed delay absorbs every failure that arrives
    /// while it runs, which is what keeps a flapping Upstream from being
    /// reconnected once per failure; a spent ladder arms nothing, so the
    /// attempts a permanently dead Upstream can cause are bounded even when
    /// nothing ever succeeds.
    void arm_retry() {
        if (settled() || status == UpstreamConnectionStatus::NeedsAuth || retry_timer.has_value() ||
                consecutive_failures >= protocol::kMaxConnectAttempts || !options.delay) {
            return;
        }
        // The armed delay absorbs every failure that arrives while it runs, so
        // the rung advances here rather than once per failure: fifty failures
        // inside one delay are one rung, not fifty.
        consecutive_failures += 1;
        retry_timer.emplace();
        report(); // the reading now advertises the armed delay
        options.delay(next_backoff(), retry_timer->get_token())
                .start([self = shared_from_this()](std::expected<void, Error> outcome) noexcept {
                    self->on_retry_elapsed(std::move(outcome));
                });
    }

    void on_retry_elapsed(std::expected<void, Error> outcome) {
        retry_timer.reset();
        if (settled() || !outcome) {
            return; // no reconnect fires after a close, a disable, or a cancelled delay
        }
        reconnect().start([](std::expected<UpstreamServerInfo, Error>) noexcept {});
    }

    void cancel_retry() {
        if (retry_timer.has_value()) {
            retry_timer.reset();
        }
    }

    // ---- the tool-catalog cache (issue #848) ----

    /// One live `tools/list` walk, cached on success. The walk is the client
    /// stack's own, so every defensive rule from issue #836 — the per-server
    /// tool cap, the pagination-cursor cap, and duplicate-name and
    /// cursor-loop rejection — applies to a refresh exactly as it does to a
    /// cold fetch: a refresh *is* a cold fetch, and the cache only decides
    /// whether one is needed.
    [[nodiscard]] AsyncResult<UpstreamCatalog> walk_catalog(std::stop_token request_token) {
        if (status != UpstreamConnectionStatus::Connected || !client.has_value()) {
            return AsyncResult<UpstreamCatalog>(std::unexpected(not_connected_error(server_id)));
        }
        auto walk = client->list_tools(request_token);
        return AsyncResult<UpstreamCatalog>([walk = std::move(walk), self = shared_from_this()](
                                                   AsyncCompletion<UpstreamCatalog, Error> completion) mutable noexcept {
            walk.start([self, completion = std::move(completion)](
                               std::expected<UpstreamCatalog, Error> outcome) mutable noexcept {
                if (outcome) {
                    self->cache_catalog(*outcome);
                }
                completion(std::move(outcome));
            });
        });
    }

    void cache_catalog(const UpstreamCatalog& catalog) {
        if (catalog_cache == nullptr) {
            return;
        }
        // An entry the cache cannot admit is simply not cached: the walk
        // already produced a correct catalog, and a cache never changes the
        // answer a caller gets.
        (void)catalog_cache->admit(server_id, catalog);
    }

    /// Re-list this Upstream's catalog behind the caller's back, at most one
    /// refresh at a time. It is admitted like any other operation, so a close
    /// cancels it and the cleanup bound covers it, and it takes no caller
    /// token: the work belongs to the cache, not to whoever happened to read
    /// the stale entry. The single-refresh rule is what keeps an Upstream that
    /// declares `ttlMs: 0` from turning every lookup into a second walk.
    void start_cache_refresh() {
        if (refresh_in_flight || catalog_cache == nullptr || settled() || !client.has_value() ||
                status != UpstreamConnectionStatus::Connected) {
            return;
        }
        refresh_in_flight = true;
        auto refresh = admit<UpstreamCatalog>({}, [self = shared_from_this()](std::stop_token request_token) {
            return self->walk_catalog(request_token);
        });
        refresh.start([self = shared_from_this()](std::expected<UpstreamCatalog, Error>) noexcept {
            self->refresh_in_flight = false;
        });
    }

    /// The catalog the caller gets, without ever making it wait on the
    /// Upstream because of the cache.
    [[nodiscard]] AsyncResult<UpstreamCatalog> list_catalog(std::stop_token caller_token) {
        // A connection the owner turned off is answered from nothing: the
        // decision to stop talking to an Upstream outranks a warm entry. A
        // connection that is still `pending` is answered from the cache,
        // which is what lets a repeated session use a catalog it already has
        // instead of waiting for the era probe.
        std::optional<CachedUpstreamCatalog> cached;
        if (catalog_cache != nullptr && !settled()) {
            cached = catalog_cache->lookup(server_id);
        }
        if (!cached.has_value()) {
            return admit<UpstreamCatalog>(caller_token, [self = shared_from_this()](std::stop_token request_token) {
                return self->walk_catalog(request_token);
            });
        }
        if (cached->stale) {
            start_cache_refresh();
        }
        return AsyncResult<UpstreamCatalog>(std::move(cached->catalog));
    }

    // ---- one connection lifetime ----

    /// A fresh client, which re-probes the era: the era belongs to a
    /// connection, and a reconnect is a new connection. `fresh_budget` is set
    /// only for a request from the owner, which is how a spent ladder starts
    /// over.
    [[nodiscard]] AsyncResult<UpstreamServerInfo> open_connection(std::stop_token token, bool fresh_budget) {
        cancel_retry();
        if (fresh_budget) {
            consecutive_failures = 0;
        }
        client.emplace(server_id,
                transport,
                UpstreamClientOptions{
                        .url = options.url,
                        .request_timeout = request_timeout,
                        .bearer_env_var = options.bearer_env_var,
                        .credentials = options.credentials,
                });
        report(); // an attempt started: the reading is `pending` even when it was before
        return client->probe_era(token);
    }

    /// A connection attempt the ladder asked for. It is admitted like any
    /// other operation, so a close reaches it and its failure is the ladder's
    /// next rung rather than a second arming.
    [[nodiscard]] AsyncResult<UpstreamServerInfo> reconnect() {
        return admit<UpstreamServerInfo>({}, [self = shared_from_this()](std::stop_token request_token) {
            return self->open_connection(request_token, false);
        });
    }

    // ---- admission ----

    /// Admit one operation: count it, join the caller's token with the
    /// connection's close request, and hand its outcome to the connection so
    /// that health derives from request results.
    template <typename T, typename Issue>
    [[nodiscard]] AsyncResult<T> admit(std::stop_token caller_token, Issue issue) {
        using Result = AsyncResult<T>;
        return Result(
                typename Result::producer_type([self = shared_from_this(), caller_token, issue = std::move(issue)](
                                                       AsyncCompletion<T, Error> completion) mutable noexcept {
                    if (self->settled()) {
                        completion(std::unexpected(not_admitted_error(
                                self->server_id, self->status == UpstreamConnectionStatus::Disabled)));
                        return;
                    }
                    auto call = std::make_shared<Call>();
                    if (caller_token.stop_possible()) {
                        call->join.emplace(caller_token, Call::Forward{.call = call});
                    }
                    self->calls.push_back(call);
                    self->in_flight += 1;
                    issue(call->source.get_token())
                            .start([self, call, completion = std::move(completion)](
                                           std::expected<T, Error> outcome) mutable noexcept {
                                self->settle(call, outcome);
                                completion(std::move(outcome));
                            });
                }));
    }

    /// One operation reached its terminal outcome. The health reading happens
    /// before the caller is completed, so a caller reacting to a failure
    /// already sees the connection the failure came from.
    template <typename T> void settle(const std::shared_ptr<Call>& call, const std::expected<T, Error>& outcome) {
        in_flight -= 1;
        std::erase(calls, call);
        if (outcome) {
            note_success();
        } else {
            note_failure(outcome.error());
        }
        recheck_quiescence();
    }

    // ---- the deterministic two-phase close (ADR 0011) ----

    /// The first phase: stop admission and request cancellation. It waits for
    /// nothing, so a close is safe to request from anywhere, including from a
    /// completion the close is waiting for.
    void begin_close() {
        if (closing || closed) {
            return;
        }
        closing = true;
        cancel_retry();
        // The stop requests run over a copy: a call that answers its
        // cancellation inline completes and leaves the admitted list while
        // this loop is still running.
        const auto admitted = calls;
        for (const auto& call : admitted) {
            call->source.request_stop();
        }
        arm_cleanup_bound();
    }

    /// The second phase, bounded by the cleanup bound. An operation that
    /// answers its cancellation, which every conforming transport does,
    /// quiesces before the bound; one that does not is abandoned when the
    /// bound expires, and the outcome says so.
    void arm_cleanup_bound() {
        if (in_flight == 0) {
            finish_close(UpstreamCloseOutcome{.within_bound = true, .abandoned_operations = 0});
            return;
        }
        if (!options.delay) {
            // Nothing can be waited for, so the connection is released with
            // whatever is still outstanding rather than kept alive for it.
            finish_close(UpstreamCloseOutcome{.within_bound = false, .abandoned_operations = in_flight});
            return;
        }
        cleanup_timer.emplace();
        options.delay(protocol::kConnectionCleanupBound, cleanup_timer->get_token())
                .start([self = shared_from_this()](std::expected<void, Error>) noexcept {
                    self->finish_close(UpstreamCloseOutcome{
                            .within_bound = false,
                            .abandoned_operations = self->in_flight,
                    });
                });
    }

    void recheck_quiescence() {
        if (closing && in_flight == 0) {
            finish_close(UpstreamCloseOutcome{.within_bound = true, .abandoned_operations = 0});
        }
    }

    void finish_close(UpstreamCloseOutcome outcome) {
        if (close_outcome.has_value()) {
            return;
        }
        cancel_retry();
        cleanup_timer.reset();
        closed = true;
        client.reset();
        close_outcome = outcome;
        auto waiters = std::move(close_waiters);
        close_waiters.clear();
        for (auto& waiter : waiters) {
            waiter(Expected<UpstreamCloseOutcome>(outcome));
        }
    }

    /// Whether nothing may start or change any more: the connection is closing
    /// or closed, or the owner turned this Upstream off.
    [[nodiscard]] bool settled() const noexcept {
        return closed || closing || status == UpstreamConnectionStatus::Disabled;
    }

    /// A per-call deadline is a containment bound, so a request for more than
    /// the cap is capped rather than honored, and a request for none gets the
    /// product default.
    [[nodiscard]] static std::chrono::milliseconds bounded_request_timeout(std::chrono::milliseconds requested) {
        if (requested <= std::chrono::milliseconds{0}) {
            return protocol::kDefaultRequestTimeout;
        }
        return std::min(requested, protocol::kMaxRequestTimeout);
    }

    const std::string server_id;
    std::shared_ptr<McpTransport> transport;
    UpstreamConnectionOptions options;
    const std::chrono::milliseconds request_timeout;
    /// The host's shared per-Upstream catalog cache, or none (issue #848).
    std::shared_ptr<UpstreamCatalogCache> catalog_cache;
    /// Whether one background catalog refresh is already running. One
    /// connection serves one Server Id, so a single flag is the whole of the
    /// at-most-one-refresh rule.
    bool refresh_in_flight{false};

    UpstreamConnectionStatus status{UpstreamConnectionStatus::Pending};
    std::string diagnostic{};
    std::size_t consecutive_failures{0};

    bool closing{false};
    bool closed{false};
    std::size_t in_flight{0};
    std::vector<std::shared_ptr<Call>> calls;
    std::optional<UpstreamClient> client;
    std::optional<std::stop_source> retry_timer;
    std::optional<std::stop_source> cleanup_timer;
    std::optional<UpstreamCloseOutcome> close_outcome;
    std::vector<AsyncCompletion<UpstreamCloseOutcome, Error>> close_waiters;
};

std::string_view to_string(UpstreamConnectionStatus status) noexcept {
    switch (status) {
    case UpstreamConnectionStatus::Pending:
        return "pending";
    case UpstreamConnectionStatus::Connected:
        return "connected";
    case UpstreamConnectionStatus::Failed:
        return "failed";
    case UpstreamConnectionStatus::NeedsAuth:
        return "needs_auth";
    case UpstreamConnectionStatus::Disabled:
        return "disabled";
    }
    return "pending";
}

UpstreamConnection::UpstreamConnection(
        std::string server_id, std::shared_ptr<McpTransport> transport, UpstreamConnectionOptions options)
    : impl_(std::make_shared<Impl>(std::move(server_id), std::move(transport), std::move(options))) {}

UpstreamConnection::~UpstreamConnection() = default;

UpstreamConnection::UpstreamConnection(UpstreamConnection&&) noexcept = default;

UpstreamConnection& UpstreamConnection::operator=(UpstreamConnection&&) noexcept = default;

const std::string& UpstreamConnection::server_id() const noexcept { return impl_->server_id; }

UpstreamConnectionStatus UpstreamConnection::status() const noexcept { return impl_->status; }

UpstreamConnectionSnapshot UpstreamConnection::snapshot() const { return impl_->snapshot(); }

AsyncResult<UpstreamServerInfo> UpstreamConnection::connect(std::stop_token stop_token) {
    auto self = impl_;
    return impl_->admit<UpstreamServerInfo>(
            stop_token, [self](std::stop_token request_token) { return self->open_connection(request_token, true); });
}

void UpstreamConnection::enable() {
    if (impl_->closed || impl_->closing || impl_->status != UpstreamConnectionStatus::Disabled) {
        return;
    }
    impl_->status = mcp::UpstreamConnectionStatus::Pending;
    impl_->diagnostic.clear();
    impl_->reconnect().start([](std::expected<UpstreamServerInfo, Error>) noexcept {});
}

void UpstreamConnection::disable(std::string reason) {
    if (impl_->settled()) {
        return;
    }
    impl_->cancel_retry();
    impl_->client.reset();
    impl_->publish(UpstreamConnectionStatus::Disabled, diagnostics::bounded(std::move(reason)));
}

void UpstreamConnection::notify_transport_closed(std::string reason) {
    auto& self = *impl_;
    if (self.settled() || self.status == UpstreamConnectionStatus::NeedsAuth) {
        return;
    }
    // The era belongs to the connection that probed it, and a connection the
    // transport can no longer reach has lost its client.
    self.client.reset();
    self.publish(UpstreamConnectionStatus::Failed, diagnostics::bounded(std::move(reason)));
    self.arm_retry();
}

AsyncResult<UpstreamCatalog> UpstreamConnection::list_tools(std::stop_token stop_token) {
    auto self = impl_;
    return impl_->list_catalog(stop_token);
}

AsyncResult<UpstreamToolCallResult> UpstreamConnection::call_tool(UpstreamToolCall call, std::stop_token stop_token) {
    auto self = impl_;
    return impl_->admit<UpstreamToolCallResult>(
            stop_token, [self, call = std::move(call)](std::stop_token request_token) {
                if (self->status != UpstreamConnectionStatus::Connected || !self->client.has_value()) {
                    return AsyncResult<UpstreamToolCallResult>(std::unexpected(not_connected_error(self->server_id)));
                }
                return self->client->call_tool(std::move(call), request_token);
            });
}

AsyncResult<UpstreamCloseOutcome> UpstreamConnection::close() {
    auto self = impl_;
    return AsyncResult<UpstreamCloseOutcome>(
            [self](AsyncCompletion<UpstreamCloseOutcome, Error> completion) mutable noexcept {
                if (self->close_outcome.has_value()) {
                    completion(Expected<UpstreamCloseOutcome>(*self->close_outcome));
                    return;
                }
                self->close_waiters.push_back(std::move(completion));
                self->begin_close();
            });
}

} // namespace cch::mcp
