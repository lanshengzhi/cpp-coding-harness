#pragma once

#include "../../include/cch/util/Error.hpp"
#include "../../include/cch/util/JsonValue.hpp"

#include <string>
#include <string_view>

namespace cch::util {

/// Parse one JSON document into the passive `JsonValue` value. Implemented
/// without Glaze in `src/util/Json.cpp` so non-serialization translation units
/// that only touch `JsonValue` never parse Glaze headers. Like the Glaze path
/// it replaces, the first complete JSON value is returned and any trailing
/// content is ignored. Generic DTO parsing (`read_json<T>` for Glaze structs)
/// lives in the private serialization layer `util/JsonGlaze.hpp`.
[[nodiscard]] Expected<JsonValue> read_json(std::string_view json);

/// Serialize the passive `JsonValue` to compact JSON, byte-identical to the
/// previous `glz::write_json(json_to_glaze(value))` output (see Json.cpp).
/// Generic DTO serialization (`write_json(const T&)`) lives in the private
/// serialization layer `util/JsonGlaze.hpp`.
[[nodiscard]] Expected<std::string> write_json(const JsonValue& value);

} // namespace cch::util
