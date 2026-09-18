#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <expected>
#include <utility>

namespace cch::tests {

/// Test brevity for the inline-completing AsyncResult path: wrap a ready
/// value (or a failure) without spelling out the std::expected constructor.

template <typename T> [[nodiscard]] cch::support::AsyncResult<T> ready_result(T value) {
    return cch::support::AsyncResult<T>(std::expected<T, cch::support::Error>{std::move(value)});
}

template <typename T> [[nodiscard]] cch::support::AsyncResult<T> failed_result(cch::support::Error error) {
    return cch::support::AsyncResult<T>(std::expected<T, cch::support::Error>{std::unexpected(std::move(error))});
}

} // namespace cch::tests
