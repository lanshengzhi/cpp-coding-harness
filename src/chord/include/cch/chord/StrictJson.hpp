#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

namespace cch::chord {

/// Validate and detach a JSON value for durable storage (pi `copyJson`).
///
/// The C++ value carrier cannot represent JavaScript `undefined`, functions,
/// class instances or sparse arrays, so those pi rejection classes are
/// unrepresentable here. The remaining strictness check is the one the carrier
/// can violate: every number must be finite. The returned tree owns its
/// strings, arrays and objects and shares nothing with `value`.
[[nodiscard]] support::Expected<support::JsonValue> copy_strict_json(const support::JsonValue& value);

/// Whether `value` is finite strict JSON at every depth. `support::JsonValue`
/// already excludes the non-JSON JavaScript value classes; non-finite doubles
/// remain representable and are rejected here.
[[nodiscard]] bool is_strict_json_value(const support::JsonValue& value);

} // namespace cch::chord
