#pragma once

#include <cch/support/Error.hpp>

namespace cch::durable {

/// The frozen message of a Session that failed (frozen pi `SessionFailed`).
/// Header-inline so every durable TU seals with the exact same error the
/// Session reports instead of duplicating the literal.
[[nodiscard]] inline support::Error session_failed_error() {
    return support::make_error(
            support::ErrorCode::Session, "Session failed after a storage error; close and reopen it");
}

} // namespace cch::durable
