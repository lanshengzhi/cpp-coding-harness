#pragma once

#include <cch/mcp/McpTransport.hpp>
#include <cch/mcp/UpstreamAuth.hpp>
#include <cch/mcp/UpstreamCatalogCache.hpp>
#include <cch/mcp/UpstreamDelay.hpp>
#include <cch/mcp/UpstreamElicitation.hpp>
#include <cch/mcp/UpstreamServer.hpp>
#include <cch/mcp/UpstreamToolCall.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp {

/// The per-Upstream state machine, exactly the five states `CONTEXT.md` names
/// for Upstream Connection Status (ADR 0065; spec #833 stories 8 and 11).
/// There is no sixth state and no ad-hoc connection boolean anywhere in the
/// product: `connected` is what a connection reached and nothing has since
/// contradicted, and there is no heartbeat that could keep it there, because
/// the 2026-07-28 revision removed `ping`.
enum class UpstreamConnectionStatus { Pending, Connected, Failed, NeedsAuth, Disabled };

/// The state name as the user sees it, in the glossary's spelling.
[[nodiscard]] std::string_view to_string(UpstreamConnectionStatus status) noexcept;

/// One reading of one Upstream connection, as the status surface publishes it.
struct UpstreamConnectionSnapshot {
    /// The Server Id, the sole stable identity for an Upstream MCP Server.
    std::string server_id{};
    UpstreamConnectionStatus status{UpstreamConnectionStatus::Pending};
    /// Bounded, redacted explanation of the last failure. Empty unless the
    /// connection failed or is waiting on the user to authenticate.
    std::string diagnostic{};
    /// Consecutive transport-level failures on the current reconnect ladder —
    /// one per failure, whether it arrived on a connection attempt or on a
    /// request — and the delay before the next attempt. Zero failures is a
    /// fresh ladder; a zero delay means no reconnect is armed, which is the
    /// state a spent ladder rests in.
    std::size_t consecutive_failures{0};
    std::chrono::milliseconds next_reconnect_delay{std::chrono::milliseconds{0}};
    /// Operations the Upstream had in flight when this reading was taken.
    std::size_t in_flight{0};
};

/// The one status sink. It is a weak observer: it runs on the connection's
/// owning execution domain during a status change and may only record or
/// forward the value (ADR 0040 §Connection strength). It is never required to
/// fail, is not called concurrently with itself, and must not re-enter the
/// connection it observes.
using UpstreamStatusSink = std::move_only_function<void(const UpstreamConnectionSnapshot&)>;

struct UpstreamConnectionOptions {
    /// The Upstream MCP Server's Streamable HTTP endpoint.
    std::string url{};
    /// Bounds one exchange. The 30 s default is the product's per-call
    /// deadline; a larger value is capped, never honored, because the cap is
    /// containment rather than tuning.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// The *name* of the environment variable a `bearer-env:<VAR>` reference
    /// in `settings.json` declared (issue #835, #838), and the store the
    /// resolved bearer is persisted under and, when the variable is unset,
    /// read back from. Both are handed to every `UpstreamClient` this
    /// connection builds, so a reconnect authenticates exactly as the first
    /// attempt did. `std::nullopt` means the server declares no credential
    /// and every request authenticates nothing; a declared reference with no
    /// `credentials` is a connection failure, never an unauthenticated
    /// request.
    std::optional<std::string> bearer_env_var{std::nullopt};
    std::shared_ptr<UpstreamCredentialStore> credentials{nullptr};
    /// Optional status surface. Absent, the connection keeps its state and
    /// publishes nothing.
    std::optional<UpstreamStatusSink> status_sink{};
    /// The connection's timer. Every connection needs one: without it a failed
    /// connection never reconnects, and a close with work still in flight
    /// abandons that work instead of bounding the wait for it.
    UpstreamDelay delay{};
    /// The per-Upstream tool-catalog cache this connection reads and fills
    /// (spec #833 story 17; issue #848). Null is a connection that always
    /// walks `tools/list`, which is the behaviour the host had before the
    /// cache existed. It is shared rather than owned so that one instance can
    /// serve every connection of every session the host owns.
    std::shared_ptr<UpstreamCatalogCache> catalog_cache{nullptr};
    /// How this connection asks the user a Pending Elicitation question
    /// (issue #845, spec #833 stories 27-29). Null is a session with no user
    /// to ask — a headless run, or a caller that has not wired the port — and
    /// an `input_required` result then fails exactly one tool call with a
    /// diagnostic, which is the defensive-matrix behaviour issue #836
    /// established before the loop existed. A port that is present but cannot
    /// ask (a null prompter, or a null timer) is likewise a single failed
    /// call: a suspended tool call never waits forever.
    ///
    /// It is shared rather than owned so that one port serves every connection
    /// of every session the host owns, and so a reconnect re-asks through the
    /// same port — a port holds no per-call state, only the seam.
    std::shared_ptr<UpstreamElicitationPort> elicitation{nullptr};
};

/// How a two-phase close ended (ADR 0011).
struct UpstreamCloseOutcome {
    /// Every operation reached a terminal outcome before the cleanup bound
    /// expired, or the connection had no timer to bound the wait with. When
    /// this is false the bound expired, or could not be waited on, and the
    /// operations below were abandoned rather than awaited.
    bool within_bound{true};
    /// Operations still in flight when the bound expired. Zero is the normal
    /// outcome: cancellation reaches a conforming transport, which answers a
    /// stopped request with a `Cancelled` error (ADR 0020).
    std::size_t abandoned_operations{0};
};

/// One Upstream MCP Server's connection: the owner of one `UpstreamClient` per
/// connection lifetime, the five-state Upstream Connection Status, the bounded
/// reconnect ladder, and the deterministic two-phase close.
///
/// Every operation returns without waiting for the Upstream, so a slow or
/// dead server can never delay a session start (spec #833 story 7). Health is
/// inferred from what the requests did and from a transport-closure
/// notification; there is no heartbeat to lose or to lie.
///
/// A connection's state changes run only in its owning serialized execution
/// domain, and its operations are driven from that same domain, exactly as the
/// transport seam it sits above requires. A connection is driven from one
/// execution domain at a time; the owner serializes it.
class UpstreamConnection {
public:
    /// The per-connection state, shared with the operations it has in flight
    /// so that releasing a connection mid-operation is safe. Defined in
    /// `UpstreamConnection.cpp`.
    struct Impl;

    /// `transport` is the one transport seam, shared across this connection's
    /// lifetimes: a reconnect reuses it. `options.delay` must be supplied.
    UpstreamConnection(
            std::string server_id, std::shared_ptr<McpTransport> transport, UpstreamConnectionOptions options);
    ~UpstreamConnection();
    UpstreamConnection(UpstreamConnection&&) noexcept;
    UpstreamConnection& operator=(UpstreamConnection&&) noexcept;
    UpstreamConnection(const UpstreamConnection&) = delete;
    UpstreamConnection& operator=(const UpstreamConnection&) = delete;

    /// The Server Id, the sole stable identity for an Upstream MCP Server.
    [[nodiscard]] const std::string& server_id() const noexcept;

    [[nodiscard]] UpstreamConnectionStatus status() const noexcept;

    [[nodiscard]] UpstreamConnectionSnapshot snapshot() const;

    /// One connection attempt, now: a fresh client that re-probes the era and
    /// a fresh reconnect budget. The attempt is admitted like any other
    /// operation, so the caller learns the outcome and the Upstream is not
    /// waited for twice.
    ///
    /// This never blocks. Against an Upstream that never answers, the returned
    /// operation stays pending, the connection reads `pending`, and everything
    /// else about the session continues.
    [[nodiscard]] cch::support::AsyncResult<UpstreamServerInfo> connect(std::stop_token stop_token = {});

    /// Re-enable a connection the owner turned off and attempt it again. A
    /// connection that is not disabled is left alone.
    void enable();

    /// Turn the connection off: the user did not enable this Upstream, or
    /// enabled it no longer. Admission stops and the armed reconnect is
    /// cancelled; operations already admitted finish on their own terms.
    /// Idempotent.
    void disable(std::string reason = {});

    /// The transport reports that this Upstream is no longer reachable. It is
    /// the only closure signal — the 2026-07-28 revision removed the session
    /// and resumability machinery a connection could otherwise be notified
    /// about — and it moves a `connected` connection to `failed` and arms one
    /// bounded reconnect. Repeats while a reconnect is armed are absorbed, so
    /// a flapping transport cannot storm the Upstream.
    void notify_transport_closed(std::string reason = {});

    /// The Upstream's tool catalog, through the connected client and the
    /// host's catalog cache. A call against a connection that is not
    /// connected fails with `Busy` rather than silently connecting, so a tool
    /// call never waits on a dead server.
    ///
    /// The cache is a shortcut the Upstream's own `ttlMs`/`cacheScope` hints
    /// asked for, and it never makes this call wait: a fresh entry is
    /// returned without an exchange, a stale one is returned immediately
    /// while at most one refresh runs behind it, and a cache that holds
    /// nothing degrades to exactly the `tools/list` walk that ran before it
    /// existed. A connection the owner has turned off is answered from
    /// neither.
    [[nodiscard]] cch::support::AsyncResult<UpstreamCatalog> list_tools(std::stop_token stop_token = {});

    /// One upstream tool call, through the connected client, bounded by the
    /// per-call deadline.
    ///
    /// `stop_token` is the caller's own cancellation — the run the user
    /// cancelled, or the connection's close, joined to the same token — and it
    /// is what stops the server work: the transport closes the response stream
    /// it is reading and the client stack sends `notifications/cancelled` for
    /// this call's request id, so the Upstream stops the operation behind the
    /// closed stream rather than running it to its own conclusion (ADR 0020;
    /// spec #833 story 30). The call then completes as one failed tool call and
    /// never as a connection failure (ADR 0008). The same token stops a
    /// Pending Elicitation wait, so cancelling during a suspended question
    /// withdraws that question and settles the call with nothing re-sent.
    ///
    /// `progress_sink` receives the Upstream's `notifications/progress` for
    /// this call and nothing else: a notification naming another request, or
    /// one the host no longer has in flight, is dropped rather than delivered
    /// (spec #833 story 31).
    ///
    /// A result the Upstream suspends for client input runs the Multi
    /// Round-Trip loop: the call is held while the user answers, and the
    /// retried request carries the original `name` and `arguments`, the
    /// opaque `requestState` echoed verbatim, and a new JSON-RPC id. The wait
    /// is bounded by the connection's elicitation port and cancellable through
    /// this call's `stop_token`, which the two-phase close also stops.
    [[nodiscard]] cch::support::AsyncResult<UpstreamToolCallResult> call_tool(UpstreamToolCall call,
            std::stop_token stop_token = {},
            UpstreamProgressSink progress_sink = nullptr);

    /// The deterministic two-phase close of ADR 0011. The first phase stops
    /// admission, requests cancellation of the admitted operations, and
    /// cancels the armed reconnect, and returns without waiting. The second
    /// phase completes once the operations have quiesced, or once the cleanup
    /// bound expires, whichever comes first, and reports which happened. No
    /// Upstream process, socket, or timer outlives it. Idempotent: a repeated
    /// close reports the same outcome immediately.
    [[nodiscard]] cch::support::AsyncResult<UpstreamCloseOutcome> close();

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::mcp
