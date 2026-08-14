#pragma once

#include "ai/providers/WebSocketTransport.hpp"
#include "util/ExpectedMacros.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

/// One scripted WebSocket connection. Frames are delivered from `frames` in
/// order; `receive_failures` (when non-empty) are returned first. An empty
/// queue with no failure either returns nullopt (when closed) or arms the
/// request's idle timeout before erroring, mirroring the real transport.
class ScriptedWebSocket final : public ai::providers::WebSocket {
public:
    struct Session {
        std::vector<std::string> sent_frames;
        std::vector<std::string> frames;
        std::vector<util::Error> receive_failures;
        bool closed{false};
        std::size_t close_count{0};
        /// Invoked synchronously on every send; enqueues frames into `frames`.
        std::function<void(ScriptedWebSocket&, std::string_view)> on_send;
    };

    ScriptedWebSocket(
        ai::providers::WebSocketConnectRequest request,
        std::shared_ptr<Session> session)
        : request_(std::move(request)), session_(std::move(session)) {}

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> async_send(
        std::string_view text) override {
        session_->sent_frames.push_back(std::string{text});
        if (session_->on_send) {
            session_->on_send(*this, text);
        }
        co_return util::ExpectedVoid{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> async_receive() override {
        if (!session_->frames.empty()) {
            auto frame = std::move(session_->frames.front());
            session_->frames.erase(session_->frames.begin());
            co_return std::optional<std::string>{std::move(frame)};
        }
        if (!session_->receive_failures.empty()) {
            auto failure = std::move(session_->receive_failures.front());
            session_->receive_failures.erase(session_->receive_failures.begin());
            co_return std::unexpected(std::move(failure));
        }
        if (session_->closed) {
            co_return std::optional<std::string>{};
        }
        if (request_.idle_timeout &&
            *request_.idle_timeout > std::chrono::milliseconds{0}) {
            auto executor = co_await boost::asio::this_coro::executor;
            boost::asio::steady_timer timer(executor, *request_.idle_timeout);
            boost::system::error_code error;
            co_await timer.async_wait(boost::asio::redirect_error(
                boost::asio::use_awaitable, error));
            if (session_->closed) {
                co_return std::optional<std::string>{};
            }
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Timeout,
                "WebSocket idle timeout after " +
                    std::to_string(request_.idle_timeout->count()) + "ms"));
        }
        co_return std::optional<std::string>{};
    }

    void close() override {
        session_->closed = true;
        ++session_->close_count;
    }

    [[nodiscard]] const ai::providers::WebSocketConnectRequest& request() const {
        return request_;
    }

    [[nodiscard]] const std::shared_ptr<Session>& session() const {
        return session_;
    }

private:
    ai::providers::WebSocketConnectRequest request_;
    std::shared_ptr<Session> session_;
};

/// Scripted WebSocket transport. `connect_scripts` are consumed in order;
/// beyond the last script (or with none) every connect creates a fresh socket
/// backed by a new session. Tests reach the sockets through `sockets` and
/// drive frames through `ScriptedWebSocket::Session`.
class ScriptedWebSocketTransport final : public ai::providers::WebSocketTransport {
public:
    struct ConnectScript {
        std::optional<util::Error> failure{std::nullopt};
        std::shared_ptr<ScriptedWebSocket::Session> session{nullptr};
    };

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::shared_ptr<ai::providers::WebSocket>>> async_connect(
        const ai::providers::WebSocketConnectRequest& request) override {
        requests.push_back(request);
        if (on_connect) {
            on_connect(request);
        }
        std::optional<util::Error> failure;
        std::shared_ptr<ScriptedWebSocket::Session> session;
        if (connect_index < connect_scripts.size()) {
            const auto script = connect_scripts[connect_index++];
            failure = script.failure;
            session = script.session;
        }
        if (failure) {
            co_return std::unexpected(*failure);
        }
        if (!session) {
            session = std::make_shared<ScriptedWebSocket::Session>();
        }
        auto socket = std::make_shared<ScriptedWebSocket>(request, session);
        sockets.push_back(socket);
        co_return std::shared_ptr<ai::providers::WebSocket>{socket};
    }

    std::vector<ai::providers::WebSocketConnectRequest> requests;
    std::vector<ConnectScript> connect_scripts;
    std::size_t connect_index{0};
    std::vector<std::shared_ptr<ScriptedWebSocket>> sockets;
    std::function<void(const ai::providers::WebSocketConnectRequest&)> on_connect;
};

} // namespace cch::tests
