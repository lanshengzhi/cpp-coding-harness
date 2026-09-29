#pragma once

#include <cch/support/AsyncResult.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::mcp::oauth {

/// One HTTP request the loopback callback server received, reduced to what the
/// authorization flow reads.
struct CallbackRequest {
    std::string method{};
    /// The request target exactly as sent: path and query, undecoded. The
    /// flow parses the query itself, because the parameters it must validate
    /// (`code`, `state`, `iss`, `error`) are compared, not merely displayed.
    std::string target{};
    std::string path{};
    std::string query{};
};

/// Parse one raw HTTP request head — a request line and whatever headers were
/// read with it, terminated by a blank line — into the request the loopback
/// server received. A head with no request line, or a request line with no
/// target, reads as no request at all; headers are not inspected, because a
/// browser redirect to a loopback callback carries nothing this flow needs.
[[nodiscard]] std::optional<CallbackRequest> parse_callback_request(std::string_view head);

/// The loopback redirect listener of one browser authorization (ADR 0032's
/// division of labour, reimplemented for this package; issue #849).
///
/// The listener is private `src/mcp/` code: it never appears in the Owner
/// Interface, and the only thing the flow learns from it is one parsed
/// callback request. It binds `127.0.0.1` on an ephemeral port, so two
/// authorizations in one process never collide on a fixed port, and it is
/// plain HTTP on the loopback interface only — the package's client
/// transports remain TLS-only (ADR 0054).
///
/// The listener owns one thread for its own accept-and-answer loop, which is
/// the whole of its execution machinery; closing it stops that thread and the
/// socket with it, and no acceptor outlives the object.
class LoopbackCallbackServer {
public:
    struct Options {
        /// The loopback interface to bind. Overridable for a test that needs
        /// a specific address; the product always binds `127.0.0.1`.
        std::string host{"127.0.0.1"};
        /// The port to bind. `0` asks the OS for an ephemeral port, which is
        /// what every authorization does.
        std::uint16_t port{0};
        /// The one exact path the callback may arrive on. Anything else is
        /// answered `404` and is not a callback.
        std::string path{"/callback"};
    };

    LoopbackCallbackServer(const LoopbackCallbackServer&) = delete;
    LoopbackCallbackServer& operator=(const LoopbackCallbackServer&) = delete;
    ~LoopbackCallbackServer();

    /// Bind the listener and start accepting. `std::nullopt` when the bind or
    /// the listen failed, which is a hard failure for the flow: an
    /// authorization whose redirect URI nothing is listening on would wait
    /// for a callback that can never arrive, so the flow fails closed instead
    /// of degrading to a wait the user cannot end.
    [[nodiscard]] static std::optional<std::shared_ptr<LoopbackCallbackServer>> start(Options options);

    /// The port actually bound, which the flow puts in the `redirect_uri` it
    /// asks the authorization server to redirect to.
    [[nodiscard]] std::uint16_t bound_port() const noexcept;

    /// The one path the callback must arrive on.
    [[nodiscard]] const std::string& path() const noexcept;

    /// Wait for one callback request and answer it. Completes once: a second
    /// wait on the same listener is refused rather than silently joining the
    /// first. `stop_token` completes the wait as `Cancelled`, and closing the
    /// listener does the same.
    ///
    /// The response the browser receives is decided by the request itself: a
    /// path this listener does not serve is `404`, a request carrying an
    /// `error` parameter or no `code` is `400`, and anything else is `200`.
    [[nodiscard]] cch::support::AsyncResult<CallbackRequest> wait(std::stop_token stop_token = {});

    /// Stop accepting, close the socket, and join the listener's thread.
    /// Idempotent, and safe to call from the flow's teardown after the wait
    /// has settled — including from the accept handler that settled it, where
    /// the thread is left to finish on its own rather than joined onto itself.
    void close();

private:
    struct State;
    explicit LoopbackCallbackServer(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace cch::mcp::oauth
