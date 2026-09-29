#include "coding_agent/runtime/McpSessionHost.hpp"

#include <cch/support/AsyncResult.hpp>
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::runtime {
namespace {

namespace asio = boost::asio;

/// The connection machinery's five states, projected into this Owner's
/// vocabulary. The mapping is total and one-to-one: the projection adds no
/// state of its own, so `/mcp` can only ever show the five the glossary names
/// (ADR 0065).
[[nodiscard]] McpUpstreamState project(mcp::UpstreamConnectionStatus status) noexcept {
    switch (status) {
    case mcp::UpstreamConnectionStatus::Pending:
        return McpUpstreamState::Pending;
    case mcp::UpstreamConnectionStatus::Connected:
        return McpUpstreamState::Connected;
    case mcp::UpstreamConnectionStatus::Failed:
        return McpUpstreamState::Failed;
    case mcp::UpstreamConnectionStatus::NeedsAuth:
        return McpUpstreamState::NeedsAuth;
    case mcp::UpstreamConnectionStatus::Disabled:
        return McpUpstreamState::Disabled;
    }
    return McpUpstreamState::Pending;
}

/// Why a configured server is not connected, in the user's terms. The text is
/// constant per case: no Upstream-supplied text reaches this row, only the
/// connection's own bounded and redacted diagnostic, so the trust reason
/// cannot leak anything the trust store or an Upstream said.
[[nodiscard]] std::string_view disabled_reason(const McpServerTrustResolution& resolution) noexcept {
    switch (resolution.source) {
    case McpServerTrustSource::PromptPending:
        return "awaiting first-enable consent";
    case McpServerTrustSource::PromptDeclined:
        return "declined";
    case McpServerTrustSource::PromptCancelled:
        return "dismissed";
    case McpServerTrustSource::PromptUnavailable:
        return "no prompt answered in this session";
    case McpServerTrustSource::PromptFailed:
        return "the first-enable prompt failed";
    case McpServerTrustSource::StoreUnavailable:
        return "the trust store could not be read";
    case McpServerTrustSource::StoredDecision:
        // A recorded decision only reaches here as `Untrusted`: the gate
        // already asked, and the answer it kept was no.
        return "declined";
    case McpServerTrustSource::PromptAccepted:
        break;
    }
    return "not enabled";
}

/// One caller's `std::stop_token` bound to an Asio cancellation slot: the
/// bridge the transport seam already uses (`cch::mcp`'s private
/// `CancellationBridge`), restated here because the connection timer is
/// driven by this package and the bridging is three members.
class CancellationBridge {
public:
    CancellationBridge(std::stop_token stop_token, boost::asio::any_io_executor executor)
        : signal_(std::make_shared<asio::cancellation_signal>()),
          callback_(std::move(stop_token), SignalEmitter{std::move(executor), signal_}) {}

    CancellationBridge(const CancellationBridge&) = delete;
    CancellationBridge& operator=(const CancellationBridge&) = delete;

    template <typename CompletionToken> [[nodiscard]] auto bind(CompletionToken&& token) const {
        return asio::bind_cancellation_slot(signal_->slot(), std::forward<CompletionToken>(token));
    }

private:
    struct SignalEmitter {
        boost::asio::any_io_executor executor;
        std::shared_ptr<asio::cancellation_signal> signal;

        void operator()() const {
            asio::post(executor, [signal = signal] { signal->emit(asio::cancellation_type::all); });
        }
    };

    std::shared_ptr<asio::cancellation_signal> signal_;
    std::stop_callback<SignalEmitter> callback_;
};

/// The connection timer the Runtime owns: one `steady_timer` wait on the
/// session's serialized executor, cancellable through the caller's stop
/// token. It is the delay the connection's reconnect ladder and its cleanup
/// bound are expressed in, and it stays a value rather than becoming a new
/// virtual seam (ADR 0040).
[[nodiscard]] mcp::UpstreamDelay make_runtime_delay(boost::asio::any_io_executor executor) {
    return [executor](std::chrono::milliseconds delay, std::stop_token stop_token) {
        return support::detail::make_async_result_on(
                executor,
                [executor, delay, stop_token]() -> boost::asio::awaitable<support::Expected<void>> {
                    asio::steady_timer timer(executor);
                    timer.expires_after(delay);
                    const CancellationBridge cancellation(stop_token, executor);
                    boost::system::error_code error;
                    co_await timer.async_wait(
                            cancellation.bind(asio::redirect_error(asio::use_awaitable, error)));
                    if (error == asio::error::operation_aborted) {
                        co_return std::unexpected(
                                support::make_error(support::ErrorCode::Cancelled, "the wait was cancelled"));
                    }
                    if (error) {
                        co_return std::unexpected(
                                support::make_error(support::ErrorCode::Unknown, "the wait failed", error.message()));
                    }
                    co_return support::Expected<void>{};
                });
    };
}

} // namespace

/// The shared state: the trust gate, the connections, and the status rows
/// they publish into. It is shared with the connections' status sinks and
/// with the connect still in flight, so a status change that arrives after
/// the handle that started it is gone still lands on a live read model.
struct McpSessionHost::State : std::enable_shared_from_this<McpSessionHost::State> {
    explicit State(McpSessionHostOptions host_options)
        : options(std::move(host_options)),
          gate(McpServerTrustStore{options.trust_store_path}, std::move(options.trust_prompter)) {
        status.reserve(options.servers.size());
        for (const auto& server : options.servers) {
            status.push_back(McpUpstreamStatus{
                    .server_id = server.server_id,
                    .state = McpUpstreamState::Pending,
                    .status_message = {},
            });
        }
    }

    /// Start the connections the trust gate enables, and wait for none of
    /// them. A server the gate does not enable gets a `disabled` row and no
    /// connection at all, so it is unreachable rather than merely unused.
    void start_enabled_connections() {
        gate.assess(options.servers);
        for (const auto& server : options.servers) {
            const auto resolution = gate.resolution(server.server_id);
            if (!resolution || !resolution->enables_server()) {
                record_disabled(server.server_id, resolution ? disabled_reason(*resolution) : "not enabled");
                continue;
            }
            auto connection = make_connection(server);
            if (connection != nullptr) {
                connect(connection);
            }
        }
    }

    [[nodiscard]] std::shared_ptr<mcp::UpstreamConnection> make_connection(const UserMcpServerSettings& server) {
        const auto existing = connections.find(server.server_id);
        if (existing != connections.end()) {
            return existing->second;
        }
        // A test seam supplies the scripted delay; production gets the
        // Runtime timer. Both are values the connection stores and calls.
        mcp::UpstreamDelay delay = options.delay ? std::move(options.delay) : make_runtime_delay(options.executor);
        mcp::UpstreamConnectionOptions connection_options{
                .url = server.url,
                .bearer_env_var = server.bearer_env_var,
                .credentials = options.credentials,
                .status_sink = [self = shared_from_this()](
                                       const mcp::UpstreamConnectionSnapshot& snapshot) { self->record(snapshot); },
                .delay = std::move(delay),
                .catalog_cache = options.catalog_cache,
                .elicitation = options.elicitation,
        };
        auto connection = std::make_shared<mcp::UpstreamConnection>(server.server_id,
                options.transport ? options.transport : mcp::make_streamable_http_transport(),
                std::move(connection_options));
        connections.emplace(server.server_id, std::move(connection));
        return connections.at(server.server_id);
    }

    /// One connection attempt, now, on the session's executor. The attempt is
    /// handed to the connection and not waited for: this is the whole of
    /// "startup never blocks on MCP" (spec #833 story 7). Its outcome reaches
    /// the status surface through the connection's own sink, which is where
    /// health is derived from.
    ///
    /// A connection that answers is then asked for its catalog, on the same
    /// domain and still without waiting: discovery is a second step of the
    /// same non-blocking path, so an Upstream that takes its catalog slowly
    /// delays its tools and nothing else.
    void connect(const std::shared_ptr<mcp::UpstreamConnection>& connection) {
        auto self = shared_from_this();
        auto token = lifetime.get_token();
        auto operation = support::detail::make_async_result_on(
                options.executor, [self, connection, token]() -> boost::asio::awaitable<support::Expected<void>> {
                    auto connected = co_await support::detail::await_async_result(connection->connect(token));
                    if (connected) {
                        co_await self->discover_tools(connection, token);
                    }
                    co_return support::Expected<void>{};
                });
        operation.start([](std::expected<void, support::Error>) noexcept {});
    }

    /// Walk one connected server's catalog (issue #842 for the `eager` path,
    /// issue #847 for the `lazy` one).
    ///
    /// A `lazy` server's catalog is recorded for search and activation, and
    /// none of its tools is published: its JSON Schema reaches the model only
    /// after the model activates it, which is what keeps dozens of Upstreams
    /// out of the context window (spec #833 story 12). Recording it is also
    /// what stages the two meta-tools, whose registration condition is "at
    /// least one connected `lazy` Upstream" and not "at least one configured
    /// one" — a configured server that never connects registers nothing.
    ///
    /// A catalog that fails to walk records nothing and fails nothing: the
    /// server's status row already carries the connection's own diagnostic, and
    /// a hostile or slow Upstream degrades to tools that are absent rather than
    /// to a session that cannot be used (ADR 0008). A tool the binding refuses
    /// — the only reachable refusal is a Qualified Tool Name already held by a
    /// different Upstream tool — is dropped rather than merged onto that tool.
    [[nodiscard]] boost::asio::awaitable<support::Expected<void>> discover_tools(
            const std::shared_ptr<mcp::UpstreamConnection>& connection, std::stop_token token) {
        const auto binding = options.tool_binding;
        if (binding == nullptr) {
            co_return support::Expected<void>{};
        }
        const auto server_id = connection->server_id();
        const auto configured = std::ranges::find_if(options.servers,
                [&server_id](const UserMcpServerSettings& entry) { return entry.server_id == server_id; });
        if (configured == options.servers.end()) {
            co_return support::Expected<void>{};
        }
        auto catalog = co_await support::detail::await_async_result(connection->list_tools(token));
        if (!catalog) {
            co_return support::Expected<void>{};
        }
        if (configured->activation_policy() == McpServerActivation::Eager) {
            for (const auto& descriptor : catalog->tools) {
                (void)binding->publish(connection, descriptor);
            }
            co_return support::Expected<void>{};
        }
        (void)binding->record_lazy_catalog(connection, std::move(*catalog));
        co_return support::Expected<void>{};
    }

    /// The one path on which a first upstream request can happen after
    /// startup: the user's answer, recorded by the gate, enabling the server.
    void enable_and_connect(std::string_view server_id) {
        const auto server = std::ranges::find_if(options.servers, [server_id](const UserMcpServerSettings& entry) {
            return entry.server_id == server_id;
        });
        if (server == options.servers.end()) {
            return;
        }
        auto connection = make_connection(*server);
        if (connection == nullptr) {
            return;
        }
        // A connection the user turned off is re-enabled first, and `enable()`
        // is what attempts it; a connection that was never turned off is
        // simply attempted.
        if (connection->status() == mcp::UpstreamConnectionStatus::Disabled) {
            connection->enable();
            return;
        }
        connect(connection);
    }

    void record(const mcp::UpstreamConnectionSnapshot& snapshot) {
        const std::scoped_lock lock(status_mutex);
        const auto row = row_for(snapshot.server_id);
        if (row == nullptr) {
            return;
        }
        row->state = project(snapshot.status);
        row->status_message = snapshot.diagnostic;
        row->consecutive_failures = snapshot.consecutive_failures;
        row->next_reconnect_delay = snapshot.next_reconnect_delay;
    }

    void record_disabled(const std::string& server_id, std::string_view reason) {
        const std::scoped_lock lock(status_mutex);
        auto* row = row_for(server_id);
        if (row == nullptr) {
            return;
        }
        row->state = McpUpstreamState::Disabled;
        row->status_message = std::string{reason};
        row->consecutive_failures = 0;
        row->next_reconnect_delay = std::chrono::milliseconds{0};
    }

    /// The row for one Server Id, or null for a Server Id this session never
    /// configured. The caller holds `status_mutex`.
    [[nodiscard]] McpUpstreamStatus* row_for(const std::string& server_id) {
        const auto found = std::ranges::find_if(
                status, [&server_id](const McpUpstreamStatus& entry) { return entry.server_id == server_id; });
        return found == status.end() ? nullptr : &*found;
    }

    McpSessionHostOptions options;
    McpServerTrustGate gate;
    std::map<std::string, std::shared_ptr<mcp::UpstreamConnection>> connections;
    std::vector<McpUpstreamStatus> status;
    mutable std::mutex status_mutex;
    /// Cancelled when the host closes, so an attempt still in flight cannot
    /// outlive the session that started it. The connection's own two-phase
    /// close cancels the same operations; this is the session-level backstop
    /// for work admitted between the two.
    std::stop_source lifetime;
};

std::shared_ptr<McpSessionHost> McpSessionHost::start(McpSessionHostOptions options) {
    if (options.servers.empty() || !options.executor) {
        return nullptr;
    }
    // A Pending Elicitation wait is bounded on the same timer as the
    // connection's own waits, so a session has exactly one clock for "wait,
    // but not forever, and stop on request". A caller that supplied a timer
    // keeps it: the test seam drives both waits through one scripted clock.
    if (options.elicitation && !options.elicitation->delay) {
        options.elicitation->delay = make_runtime_delay(options.executor);
    }
    auto state = std::make_shared<State>(std::move(options));
    state->start_enabled_connections();
    auto host = std::shared_ptr<McpSessionHost>(new McpSessionHost());
    host->state_ = std::move(state);
    return host;
}

std::vector<McpUpstreamStatus> McpSessionHost::upstream_status() const {
    if (state_ == nullptr) {
        return {};
    }
    const std::scoped_lock lock(state_->status_mutex);
    return state_->status;
}

std::vector<McpServerTrustPromptRequest> McpSessionHost::pending_trust_requests() const {
    if (state_ == nullptr) {
        return {};
    }
    return state_->gate.pending_requests();
}

support::AsyncResult<McpServerTrustResolution> McpSessionHost::ask_trust(
        std::string_view server_id, std::stop_token stop_token) {
    auto state = state_;
    if (state == nullptr) {
        return support::AsyncResult<McpServerTrustResolution>(std::unexpected(
                support::make_error(support::ErrorCode::Validation, "this session has no MCP Host wiring")));
    }
    return support::AsyncResult<McpServerTrustResolution>(
            [state, id = std::string{server_id}, stop_token](
                    support::AsyncCompletion<McpServerTrustResolution, support::Error> completion) mutable noexcept {
                state->gate.ask(id, stop_token)
                        .start([state, id, completion = std::move(completion)](
                                       std::expected<McpServerTrustResolution, support::Error>
                                               outcome) mutable noexcept {
                            if (outcome && outcome->enables_server()) {
                                state->enable_and_connect(id);
                            }
                            completion(std::move(outcome));
                        });
            });
}

mcp::UpstreamConnection* McpSessionHost::connection(std::string_view server_id) const {
    if (state_ == nullptr) {
        return nullptr;
    }
    const auto found = state_->connections.find(std::string{server_id});
    return found == state_->connections.end() ? nullptr : found->second.get();
}

support::AsyncResult<void> McpSessionHost::close() {
    auto state = state_;
    if (state == nullptr) {
        return support::AsyncResult<void>(support::Expected<void>{});
    }
    // Ask for the session's own cancellation first, so an operation being
    // admitted cannot start a new attempt behind the close.
    state->lifetime.request_stop();
    auto closing = std::make_shared<std::vector<std::shared_ptr<mcp::UpstreamConnection>>>();
    closing->reserve(state->connections.size());
    for (const auto& [server_id, connection] : state->connections) {
        (void)server_id;
        closing->push_back(connection);
    }
    return support::AsyncResult<void>(
            [state, closing](support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                if (closing->empty()) {
                    completion(support::Expected<void>{});
                    return;
                }
                // Every connection reports whether its teardown met the
                // cleanup bound; a session close that had to abandon an
                // operation says so rather than reporting a clean close.
                auto remaining = std::make_shared<std::atomic<std::size_t>>(closing->size());
                auto abandoned = std::make_shared<std::atomic<std::size_t>>(0);
                auto settled = std::make_shared<support::AsyncCompletion<void, support::Error>>(
                        std::move(completion));
                for (const auto& connection : *closing) {
                    connection->close().start([remaining, abandoned, settled](
                                                      std::expected<mcp::UpstreamCloseOutcome, support::Error>
                                                              outcome) mutable noexcept {
                        if (outcome && !outcome->within_bound) {
                            abandoned->fetch_add(outcome->abandoned_operations);
                        }
                        if (remaining->fetch_sub(1) != 1) {
                            return;
                        }
                        if (abandoned->load() > 0) {
                            std::move(*settled)(std::unexpected(support::make_error(
                                    support::ErrorCode::Timeout,
                                    "an Upstream MCP Server did not close within the cleanup bound",
                                    std::to_string(abandoned->load()) + " operation(s) abandoned")));
                            return;
                        }
                        std::move(*settled)(support::Expected<void>{});
                    });
                }
            });
}

} // namespace cch::coding_agent::runtime
