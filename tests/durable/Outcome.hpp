#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <expected>
#include <utility>

namespace cch::tests::durable {

/// The terminal outcome of one consumed `AsyncResult` (ADR 0040 §AsyncResult).
///
/// The `AsyncResult` is a move-only, single-consumption value: it is not an
/// `std::expected` and has no `has_value()`. A test consumes it through
/// `start()` and observes the terminal outcome through this holder.
template <typename T> struct Outcome {
    /// The terminal outcome: a value, or the error that ended the operation.
    std::expected<T, cch::support::Error> result{std::unexpected(cch::support::Error{})};

    [[nodiscard]] bool has_value() const noexcept { return result.has_value(); }

    [[nodiscard]] const cch::support::Error& error() const noexcept { return result.error(); }

    /// The terminal value; only meaningful when `has_value()` is true.
    [[nodiscard]] T& value() noexcept { return *result; }

    [[nodiscard]] const T& value() const noexcept { return *result; }
};

/// The terminal outcome of one consumed `AsyncResult` of a `void` operation.
template <> struct Outcome<void> {
    std::expected<void, cch::support::Error> result{std::unexpected(cch::support::Error{})};

    [[nodiscard]] bool has_value() const noexcept { return result.has_value(); }

    [[nodiscard]] const cch::support::Error& error() const noexcept { return result.error(); }
};

/// Consume one `AsyncResult` and return its terminal outcome.
template <typename T> [[nodiscard]] Outcome<T> consume(cch::support::AsyncResult<T> result) {
    Outcome<T> outcome;
    std::move(result).start([&outcome](std::expected<T, cch::support::Error> terminal) mutable noexcept {
        outcome.result = std::move(terminal);
    });
    return outcome;
}

} // namespace cch::tests::durable
