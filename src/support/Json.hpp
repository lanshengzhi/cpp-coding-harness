#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <string>
#include <string_view>

namespace cch::support {

/// Parse one JSON document into the passive `JsonValue` value. Implemented
/// without Glaze in `src/support/Json.cpp` so non-serialization translation units
/// that only touch `JsonValue` never parse Glaze headers. Like the Glaze path
/// it replaces, the first complete JSON value is returned and any trailing
/// content is ignored. Object members keep the source text's order (#1149).
/// Generic DTO parsing (`read_json<T>` for Glaze structs)
/// lives in the private serialization layer `support/JsonGlaze.hpp`.
[[nodiscard]] Expected<JsonValue> read_json(std::string_view json);

/// Serialize the passive `JsonValue` to compact JSON, byte-identical to the
/// previous `glz::write_json(json_to_glaze(value))` output (see Json.cpp).
/// Generic DTO serialization (`write_json(const T&)`) lives in the private
/// serialization layer `support/JsonGlaze.hpp`.
[[nodiscard]] Expected<std::string> write_json(const JsonValue& value);

/// Serialize the passive `JsonValue` with every object's members in sorted key
/// order at every depth. This is the explicit canonical form: two values that
/// differ only in member order produce the same text. Callers that compare or
/// pin bytes compare through this function rather than relying on a container's
/// iteration order (#1149).
[[nodiscard]] Expected<std::string> canonical_json(const JsonValue& value);

} // namespace cch::support
