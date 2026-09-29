#include "mcp/OAuthCallbackServer.hpp"

#include "mcp/OAuthSupport.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace cch::mcp::oauth {
namespace {

using support::AsyncCompletion;
using support::AsyncResult;
using support::Error;
using support::ErrorCode;
using support::Expected;
using support::make_error;

/// The response a browser is shown once the redirect lands. The pages are
/// fixed: the flow has nothing to render, and an authorization code must
/// never reach a page.
constexpr std::string_view kSuccessPage =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!doctype html><html><head><title>Authorization complete</title></head>"
        "<body><p>Authorization complete. You can close this window and return to pike.</p></body></html>";
constexpr std::string_view kErrorPage =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!doctype html><html><head><title>Authorization failed</title></head>"
        "<body><p>Authorization failed. You can close this window and return to pike.</p></body></html>";
constexpr std::string_view kNotFoundPage =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!doctype html><html><head><title>Not found</title></head>"
        "<body><p>Not found. You can close this window and return to pike.</p></body></html>";
constexpr std::string_view kUnavailablePage =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!doctype html><html><head><title>Authorization stopped</title></head>"
        "<body><p>Authorization stopped. You can close this window and return to pike.</p></body></html>";

} // namespace

std::optional<CallbackRequest> parse_callback_request(std::string_view head) {
    const auto line_end = head.find("\r\n");
    const auto request_line = head.substr(0, line_end == std::string_view::npos ? head.size() : line_end);
    const auto first = request_line.find(' ');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = request_line.find(' ', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto method = request_line.substr(0, first);
    if (method != "GET" && method != "POST" && method != "HEAD") {
        return std::nullopt;
    }
    const auto target = request_line.substr(first + 1, second - first - 1);
    if (target.empty() || target.front() != '/') {
        return std::nullopt;
    }
    const auto query_start = target.find('?');
    return CallbackRequest{
            .method = std::string{method},
            .target = std::string{target},
            .path = std::string{query_start == std::string_view::npos ? target : target.substr(0, query_start)},
            .query =
                    query_start == std::string_view::npos ? std::string{} : std::string{target.substr(query_start + 1)},
    };
}

struct LoopbackCallbackServer::State {
    State(std::string callback_path) : path(std::move(callback_path)) {}

    /// The accept-and-answer loop. Everything in this group is touched only on
    /// the listener's own thread, except the close, which Asio defines as
    /// safe to call from another thread.
    boost::asio::io_context io{};
    boost::asio::ip::tcp::acceptor acceptor{io};
    boost::asio::ip::tcp::socket socket{io};
    std::jthread thread{};

    mutable std::mutex mutex{};
    std::optional<AsyncCompletion<CallbackRequest, Error>> completion{};
    /// A callback that arrived before anyone was waiting for it. A browser can
    /// redirect faster than the flow gets from presenting the URL to arming
    /// its wait, and dropping that redirect would leave the flow waiting for a
    /// callback that has already happened.
    std::optional<Expected<CallbackRequest>> early{};
    std::optional<std::stop_callback<std::function<void()>>> watcher{};
    bool settled{false};
    bool closed{false};

    std::string path{};
    std::uint16_t port{0};

    /// Settle the one wait, exactly once, and drop the cancellation watcher so
    /// a later stop request cannot re-enter a settled flow. Never called from
    /// inside the watcher's own callback: the watcher only closes the socket
    /// and the accept handler settles on the listener's thread.
    void settle(Expected<CallbackRequest> outcome) {
        AsyncCompletion<CallbackRequest, Error> deliver;
        {
            std::scoped_lock lock(mutex);
            if (settled) {
                return;
            }
            settled = true;
            if (completion.has_value()) {
                deliver = std::move(*completion);
                completion.reset();
            } else {
                early = std::move(outcome);
            }
            watcher.reset();
        }
        if (deliver) {
            deliver(std::move(outcome));
        }
    }

    [[nodiscard]] bool is_closed() const {
        std::scoped_lock lock(mutex);
        return closed;
    }

    static void answer(boost::asio::ip::tcp::socket& connection, std::string_view page) {
        boost::system::error_code ignored;
        boost::asio::write(connection, boost::asio::buffer(page), ignored);
        connection.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        connection.close(ignored);
    }

    void on_accept(const boost::system::error_code& error) {
        if (error) {
            if (error == boost::asio::error::operation_aborted || is_closed()) {
                settle(std::unexpected(make_error(ErrorCode::Cancelled, "the authorization was cancelled")));
            } else {
                settle(std::unexpected(
                        make_error(ErrorCode::Network, "the authorization callback listener failed", error.message())));
            }
            return;
        }
        boost::asio::streambuf head;
        boost::system::error_code read_error;
        const auto transferred = boost::asio::read_until(socket, head, "\r\n\r\n", read_error);
        if (read_error || transferred == 0) {
            answer(socket, kUnavailablePage);
            settle(std::unexpected(make_error(ErrorCode::Network,
                    "the authorization callback could not be read",
                    read_error ? read_error.message() : "the browser sent no request line")));
            return;
        }
        const auto begin = boost::asio::buffers_begin(head.data());
        const std::string raw{begin, begin + transferred};
        const auto request = parse_callback_request(raw);
        if (!request.has_value() || request->path != path) {
            answer(socket, kNotFoundPage);
            settle(std::unexpected(make_error(ErrorCode::OAuth,
                    "the authorization callback did not arrive on this listener's route",
                    "the browser reached the loopback listener on an unexpected path")));
            return;
        }
        const auto query = parse_query(request->query);
        const auto reported_error = query_value(query, "error");
        const auto code = query_value(query, "code");
        answer(socket, reported_error.empty() && !code.empty() ? kSuccessPage : kErrorPage);
        if (!reported_error.empty() || code.empty()) {
            settle(std::unexpected(make_error(ErrorCode::OAuth,
                    "the authorization server refused the authorization request",
                    reported_error.empty() ? "the callback carried no authorization code" : reported_error)));
            return;
        }
        settle(Expected<CallbackRequest>{std::move(*request)});
    }
};

LoopbackCallbackServer::LoopbackCallbackServer(std::shared_ptr<State> state) : state_(std::move(state)) {}

LoopbackCallbackServer::~LoopbackCallbackServer() { close(); }

std::optional<std::shared_ptr<LoopbackCallbackServer>> LoopbackCallbackServer::start(Options options) {
    auto state = std::make_shared<State>(options.path);
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(options.host, error);
    if (error) {
        return std::nullopt;
    }
    boost::asio::ip::tcp::endpoint endpoint{address, options.port};
    state->acceptor.open(endpoint.protocol(), error);
    if (error) {
        return std::nullopt;
    }
    state->acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
    state->acceptor.bind(endpoint, error);
    if (error) {
        return std::nullopt;
    }
    state->acceptor.listen(boost::asio::socket_base::max_listen_connections, error);
    if (error) {
        return std::nullopt;
    }
    state->port = state->acceptor.local_endpoint(error).port();
    if (error) {
        return std::nullopt;
    }
    auto server = std::shared_ptr<LoopbackCallbackServer>(new LoopbackCallbackServer(state));
    // The accept is armed before the thread starts: a thread that runs
    // `io.run()` with nothing pending returns immediately, and the accept
    // posted afterwards would have no thread left to run it.
    state->acceptor.async_accept(
            state->socket, [state](const boost::system::error_code& accept_error) { state->on_accept(accept_error); });
    state->thread = std::jthread([state] { state->io.run(); });
    return server;
}

std::uint16_t LoopbackCallbackServer::bound_port() const noexcept { return state_->port; }

const std::string& LoopbackCallbackServer::path() const noexcept { return state_->path; }

AsyncResult<CallbackRequest> LoopbackCallbackServer::wait(std::stop_token stop_token) {
    auto state = state_;
    return AsyncResult<CallbackRequest>(AsyncResult<CallbackRequest>::producer_type(
            [state, stop_token](AsyncCompletion<CallbackRequest, Error> completion) mutable noexcept {
                {
                    std::scoped_lock lock(state->mutex);
                    if (state->settled) {
                        // The callback already arrived; the flow has its
                        // answer rather than a second wait that would never
                        // be answered.
                        if (state->early.has_value()) {
                            auto answered = std::move(*state->early);
                            state->early.reset();
                            completion(std::move(answered));
                        } else {
                            completion(std::unexpected(
                                    make_error(ErrorCode::Busy, "the authorization callback was already answered")));
                        }
                        return;
                    }
                    if (state->completion.has_value()) {
                        completion(std::unexpected(
                                make_error(ErrorCode::Busy, "the authorization callback is already being awaited")));
                        return;
                    }
                    state->completion = std::move(completion);
                    // The watcher closes the listener rather than settling the
                    // wait itself: the accept handler settles on this
                    // listener's thread, which is the only place the watcher
                    // may be destroyed.
                    state->watcher.emplace(stop_token, [state] {
                        boost::system::error_code ignored;
                        state->acceptor.close(ignored);
                        state->socket.close(ignored);
                    });
                }
            }));
}

void LoopbackCallbackServer::close() {
    auto state = state_;
    if (state == nullptr) {
        return;
    }
    {
        std::scoped_lock lock(state->mutex);
        if (state->closed) {
            return;
        }
        state->closed = true;
    }
    boost::system::error_code ignored;
    state->acceptor.close(ignored);
    state->socket.close(ignored);
    state->io.stop();
    if (state->thread.joinable()) {
        if (state->thread.get_id() == std::this_thread::get_id()) {
            // The accept handler settles the wait on this very thread, and a
            // flow that settles from there closes the listener it is running
            // on. Joining would deadlock, so the thread is left to leave
            // `io.run()` on its own: the acceptor and the socket are already
            // closed, and the state the loop captured keeps itself alive until
            // it returns.
            state->thread.detach();
        } else {
            state->thread.join();
        }
    }
    state->settle(std::unexpected(make_error(ErrorCode::Cancelled, "the authorization was cancelled")));
}

} // namespace cch::mcp::oauth
