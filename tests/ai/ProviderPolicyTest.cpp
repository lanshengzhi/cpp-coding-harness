#include <cch/ai/RequestOptions.hpp>
#include "ai/api/Termination.hpp"
#include "ai/providers/RetryPolicy.hpp"
#include "support/PiFixture.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <array>
#include <optional>
#include <string_view>

using namespace cch;

namespace {

std::string_view stop_reason_name(ai::AssistantStopReason reason) {
    switch (reason) {
    case ai::AssistantStopReason::Stop:
        return "stop";
    case ai::AssistantStopReason::Length:
        return "length";
    case ai::AssistantStopReason::ToolUse:
        return "toolUse";
    case ai::AssistantStopReason::Error:
        return "error";
    case ai::AssistantStopReason::Aborted:
        return "aborted";
    }
    return "unknown";
}

} // namespace

TEST_CASE(
    "Provider termination mapping follows the committed terminal matrix",
    "[ai][provider-policy][issue339]") {
    const auto fixture = tests::read_pi_fixture("termination/matrix.json");
    REQUIRE(fixture);

    const auto& responses = fixture->at("responses").get_object();
    for (const auto& [terminal, expected] : responses) {
        const bool has_tool_call = terminal == "completed_with_tool_call";
        const auto wire_terminal = has_tool_call ? "completed" : terminal == "missing" ? "" : terminal;
        const auto mapped = ai::api::map_responses_termination(wire_terminal, has_tool_call);
        REQUIRE(mapped);
        CHECK(stop_reason_name(mapped->reason) == expected.get_string());
    }

    const auto& anthropic = fixture->at("anthropic").get_object();
    for (const auto& [terminal, expected] : anthropic) {
        const auto wire_terminal = terminal == "missing_message_stop"
            ? std::string_view{}
            : terminal == "unknown" ? std::string_view{"future_reason"} : terminal;
        const auto mapped = ai::api::map_anthropic_termination(wire_terminal);
        if (expected.get_string() == "stream_error") {
            REQUIRE_FALSE(mapped);
            CHECK(mapped.error().message.find("future_reason") != std::string::npos);
            continue;
        }
        REQUIRE(mapped);
        CHECK(stop_reason_name(mapped->reason) == expected.get_string());
        if (terminal == "refusal" || terminal == "sensitive" ||
            terminal == "missing_message_stop") {
            REQUIRE(mapped->error_message);
        }
    }
}

TEST_CASE(
    "Provider retry policy classifies transient failures and bounds server delays",
    "[ai][provider-policy][issue339]") {
    CHECK(ai::SimpleStreamOptions{}.max_retries == 0);
    CHECK(ai::SimpleStreamOptions{}.max_retry_delay_ms == std::nullopt);

    CHECK(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{.network_error = true}));
    constexpr std::array<int, 6> kTransientStatuses{408, 409, 429, 500, 502, 599};
    for (const auto status : kTransientStatuses) {
        CHECK(ai::providers::is_retryable_provider_failure(
            ai::providers::ProviderFailure{.status = status}));
    }
    CHECK_FALSE(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{.status = 400}));
    CHECK_FALSE(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{
            .status = 429,
            .message = "insufficient quota: update billing",
        }));
    CHECK_FALSE(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{
            .status = 500,
            .headers = {{"x-should-retry", "true"}},
            .terminal_quota_or_billing = true,
        }));
    CHECK(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{
            .status = 400,
            .headers = {{"X-Should-Retry", "true"}},
        }));
    CHECK_FALSE(ai::providers::is_retryable_provider_failure(
        ai::providers::ProviderFailure{
            .status = 500,
            .headers = {{"x-should-retry", "false"}},
        }));

    const auto exponential = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{}, 2, std::nullopt, 0);
    REQUIRE(exponential);
    CHECK(*exponential == 4000);

    const auto numeric = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{.headers = {{"Retry-After", "2.5"}}},
        0,
        std::nullopt,
        0);
    REQUIRE(numeric);
    CHECK(*numeric == 2500);

    const auto date = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{
            .headers = {{"Retry-After", "Thu, 01 Jan 1970 00:00:03 GMT"}},
        },
        0,
        std::nullopt,
        1000);
    REQUIRE(date);
    CHECK(*date == 2000);

    const auto too_long = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{.headers = {{"retry-after-ms", "61000"}}},
        0,
        std::nullopt,
        0);
    REQUIRE_FALSE(too_long);

    const auto custom_cap = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{.headers = {{"retry-after-ms", "2001"}}},
        0,
        2000,
        0);
    REQUIRE_FALSE(custom_cap);

    const auto unbounded = ai::providers::provider_retry_delay_ms(
        ai::providers::ProviderFailure{.headers = {{"retry-after-ms", "61000"}}},
        0,
        0,
        0);
    REQUIRE(unbounded);
    CHECK(*unbounded == 61000);
}
