#pragma once

// TEMPORARY LEGACY SUPPORT ALIAS — do not add new uses.
//
// `JsonValue` moved to the pi-neutral `cch_support` package
// (include/cch/support/JsonValue.hpp, ADR 0039). This forwarding header keeps
// the historical `cch::util` spelling compiling during the package migration.
// It is scheduled for deletion by the support contraction ticket #469; until
// then the two spellings name the same type.
#include <cch/support/JsonValue.hpp>

namespace cch::util {
using cch::support::JsonValue;
} // namespace cch::util
