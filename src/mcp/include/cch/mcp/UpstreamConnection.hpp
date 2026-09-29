#pragma once

#include <cch/mcp/McpTransport.hpp>
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

/// One delay on the connection's own timer, supplied by the owner of the
/// connection's execution domain: `cch_mcp` owns no event loop, and timer
/// state stays local to the operation that needs it (ADR 0040). The operation
/// completes when the delay elapses and fails with `Cancelled` when
/// `stop_token` fires first, so the connection can both wait out the
/// reconnect ladder and bound its own cleanup.
using UpstreamDelay = std::move_only_function<cch::support::AsyncResult<void>(
        std::chrono::milliseconds delay,
        std::stop_token stop_token)>;

struct UpstreamConnectionOptions {
    /// The Upstream MCP Server's Streamable HTTP endpoint.
    std::string url{};
    /// Bounds one exchange. The 30 s default is the product's per-call
    /// deadline; a larger value is capped, never honored, because the cap is
    /// containment rather than tuning.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// Optional status surface. Absent, the connection keeps its state and
    /// publishes nothing.
    std::optional<UpstreamStatusSink> status_sink{};
    /// The connection's timer. Every connection needs one: without it a
    /// failed connection never reconnects and a close cannot wait out its
    /// cleanup bound.
    UpstreamDelay delay{};
};

/// How a two-phase close ended (ADR 0011).
struct UpstreamCloseOutcome {
    /// The cleanup bound expired before every operation reached a terminal
    /// outcome, so the operations below were abandoned rather than awaited.
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
            std::string server_id,
            std::shared_ptr<McpTransport> transport,
            UpstreamConnectionOptions options);
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

    /// The Upstream's tool catalog, through the connected client. A call
    /// against a connection that is not connected fails with `Busy` rather
    /// than silently connecting, so a tool call never waits on a dead server.
    [[nodiscard]] cch::support::AsyncResult<UpstreamCatalog> list_tools(std::stop_token stop_token = {});

    /// One upstream tool call, through the connected client, bounded by the
    /// per-call deadline.
    [[nodiscard]] cch::support::AsyncResult<UpstreamToolCallResult> call_tool(
            UpstreamToolCall call, std::stop_token stop_token = {});

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
