#include "ai/AsyncResultBridge.hpp"
#include "ai/ModelStreamBridge.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/StreamEvent.hpp>
#include <cch/support/Error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <expected>
#include <optional>
#include <utility>
#include <vector>

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
#error "Async bridge tests must compile Boost.Asio with exceptions disabled"
#endif

namespace {

template <typename T>
struct SpawnResult {
    bool completed{false};
    bool exception{false};
    std::optional<T> value;
};

template <typename T>
SpawnResult<T> run_awaitable(boost::asio::awaitable<T> operation) {
    boost::asio::io_context io;
    SpawnResult<T> result;
    boost::asio::co_spawn(
        io,
        std::move(operation),
        [&result](auto exception, T value) noexcept {
            result.completed = true;
            if (exception) {
                result.exception = true;
                return;
            }
            result.value.emplace(std::move(value));
        });
    io.run();
    return result;
}

} // namespace

TEST_CASE(
    "the no-exception AsyncResult bridge delivers a successful terminal outcome",
    "[ai][bridge][issue482]") {
    auto operation = cch::ai::detail::make_async_result(
        []() -> boost::asio::awaitable<cch::support::Expected<int>> {
            co_return 42;
        });

    const auto run = run_awaitable(
        cch::ai::detail::await_async_result(std::move(operation)));

    REQUIRE(run.completed);
    CHECK_FALSE(run.exception);
    REQUIRE(run.value.has_value());
    REQUIRE(run.value->has_value());
    CHECK(run.value->value() == 42);
}

TEST_CASE(
    "the no-exception AsyncResult bridge preserves an explicit terminal error",
    "[ai][bridge][issue482]") {
    auto operation = cch::ai::detail::make_async_result(
        []() -> boost::asio::awaitable<cch::support::Expected<int>> {
            co_return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Cancelled,
                "cancelled by the producer"));
        });

    const auto run = run_awaitable(
        cch::ai::detail::await_async_result(std::move(operation)));

    REQUIRE(run.completed);
    CHECK_FALSE(run.exception);
    REQUIRE(run.value.has_value());
    REQUIRE_FALSE(run.value->has_value());
    CHECK(run.value->error().code == cch::support::ErrorCode::Cancelled);
    CHECK(run.value->error().message == "cancelled by the producer");
}

TEST_CASE(
    "the no-exception ModelStream bridge forwards events and its terminal message",
    "[ai][bridge][issue482]") {
    auto stream = cch::ai::detail::make_model_stream(
        [](cch::ai::AssistantEventSink sink)
            -> boost::asio::awaitable<cch::support::Expected<cch::ai::AssistantMessage>> {
            auto message = cch::ai::assistant_text_message("bridged message");
            if (auto emitted = sink(cch::ai::AssistantStartEvent{.partial = message}); !emitted) {
                co_return std::unexpected(std::move(emitted.error()));
            }
            co_return message;
        });

    std::vector<cch::ai::AssistantStreamEvent> events;
    auto result = std::move(stream).run(
        [&events](const cch::ai::AssistantStreamEvent& event) -> cch::support::ExpectedVoid {
            events.push_back(event);
            return {};
        });
    const auto run = run_awaitable(
        cch::ai::detail::await_async_result(std::move(result)));

    REQUIRE(run.completed);
    CHECK_FALSE(run.exception);
    REQUIRE(run.value.has_value());
    REQUIRE(run.value->has_value());
    CHECK(cch::ai::text_from_assistant_content(run.value->value().content) == "bridged message");
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<cch::ai::AssistantStartEvent>(events.front()));
}
