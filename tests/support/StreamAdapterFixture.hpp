#pragma once

#include <cch/ai/Auth.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/ai/providers/StreamTransport.hpp>
#include "util/ExpectedMacros.hpp"

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

class EmptyCredentialStore final : public ai::CredentialStore {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> read(
        std::string) override {
        co_return std::optional<ai::Credential>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::vector<ai::CredentialInfo>>> list() override {
        co_return std::vector<ai::CredentialInfo>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ai::Credential>>> modify(
        std::string,
        ai::CredentialModifyHook) override {
        co_return std::optional<ai::Credential>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> remove(std::string) override {
        co_return util::ExpectedVoid{};
    }
};

class EmptyAuthContext final : public ai::AuthContext {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<std::string>>> environment(
        std::string) const override {
        co_return std::optional<std::string>{};
    }

    [[nodiscard]] boost::asio::awaitable<util::Expected<bool>> file_exists(
        std::string) const override {
        co_return false;
    }
};

struct TransportAttempt {
    ai::providers::StreamResponseHead head{
        .status_code = 200,
        .headers = {},
    };
    std::vector<std::string> chunks;
    std::optional<util::Error> failure{std::nullopt};
};

class ScriptedTransport final : public ai::providers::StreamTransport {
public:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ai::providers::StreamResponse>> async_stream(
        const ai::providers::StreamRequest& request,
        ai::providers::BodyChunkHandler on_body_chunk) override {
        requests.push_back(request);
        if (on_request) {
            on_request();
        }
        if (attempt_index >= attempts.size()) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Network,
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
