#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/StreamEvent.hpp>
#include "ai/providers/StreamTransport.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "support/ExpectedMacros.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::tests {

/// Stop reasons of every non-terminal (partial) assistant event, in emission
/// order. Terminal done/error events are excluded.
[[nodiscard]] inline std::vector<ai::AssistantStopReason> partial_stop_reasons(
    const std::vector<ai::AssistantStreamEvent>& events) {
    std::vector<ai::AssistantStopReason> reasons;
    for (const auto& event : events) {
        std::visit(
            [&reasons](const auto& concrete) {
                using Event = std::decay_t<decltype(concrete)>;
                if constexpr (!std::is_same_v<Event, ai::AssistantDoneEvent> &&
                              !std::is_same_v<Event, ai::AssistantErrorEvent>) {
                    reasons.push_back(concrete.partial.stop_reason);
                }
            },
            event);
    }
    return reasons;
}

template <typename T>
[[nodiscard]] T run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
    io.run();
    return result.get();
}

template <typename T, typename E>
[[nodiscard]] std::expected<T, E> run_async_result(cch::support::AsyncResult<T, E> result) {
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io,
        [](cch::support::AsyncResult<T, E> op) -> boost::asio::awaitable<std::expected<T, E>> {
            co_return co_await cch::ai::detail::await_async_result(std::move(op));
        }(std::move(result)),
        boost::asio::use_future);
    io.run();
    return future.get();
}

/// Run a `CredentialModifyHook` to completion on a throwaway io_context. Test
/// fakes use this to consume a modifier hook inside a plain `AsyncResult`-returning
/// method; real hooks used by fakes are deterministic and never perform blocking I/O.
template <typename T>
[[nodiscard]] support::Expected<T> run_hook(cch::support::AsyncResult<T> hook) {
    return run_async_result(std::move(hook));
}

class EmptyCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> read(
        std::string) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::vector<ai::CredentialInfo>> list() override {
        return cch::support::AsyncResult<std::vector<ai::CredentialInfo>>(
            std::expected<std::vector<ai::CredentialInfo>, cch::support::Error>{
                std::vector<ai::CredentialInfo>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<std::optional<ai::Credential>> modify(
        std::string,
        ai::CredentialModifyHook) override {
        return cch::support::AsyncResult<std::optional<ai::Credential>>(
            std::expected<std::optional<ai::Credential>, cch::support::Error>{
                std::optional<ai::Credential>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<void> remove(std::string) override {
        return cch::support::AsyncResult<void>(
            std::expected<void, cch::support::Error>{});
    }
};

class EmptyAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] cch::support::AsyncResult<std::optional<std::string>> environment(
        std::string) const override {
        return cch::support::AsyncResult<std::optional<std::string>>(
            std::expected<std::optional<std::string>, cch::support::Error>{
                std::optional<std::string>{}});
    }

    [[nodiscard]] cch::support::AsyncResult<bool> file_exists(
        std::string) const override {
        return cch::support::AsyncResult<bool>(
            std::expected<bool, cch::support::Error>{false});
    }
};

struct TransportAttempt {
    ai::providers::StreamResponseHead head{
        .status_code = 200,
        .headers = {},
    };
    std::vector<std::string> chunks;
    std::optional<support::Error> failure{std::nullopt};
};

class ScriptedTransport final : public ai::providers::StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::providers::StreamResponse>> async_stream(
        const ai::providers::StreamRequest& request,
        ai::providers::BodyChunkHandler on_body_chunk) override {
        requests.push_back(request);
        if (on_request) {
            on_request();
        }
        if (attempt_index >= attempts.size()) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Network,
                "no scripted transport attempt"));
        }
        auto attempt = attempts[attempt_index++];
        if (attempt.failure) {
            co_return std::unexpected(*attempt.failure);
        }

        ai::providers::StreamResponse response{
            .head = std::move(attempt.head),
            .body = {},
        };
        for (const auto& chunk : attempt.chunks) {
            response.body += chunk;
            if (response.head.status_code >= 200 && response.head.status_code < 300) {
                CCH_TRY_VOID(on_body_chunk(chunk));
            }
        }
        co_return response;
    }

    std::vector<TransportAttempt> attempts;
    std::vector<ai::providers::StreamRequest> requests;
    std::size_t attempt_index{0};
    std::function<void()> on_request;
};

} // namespace cch::tests
