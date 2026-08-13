#pragma once

// TEMPORARY LEGACY SUPPORT ALIAS — do not add new uses.
//
// `Error`/`Expected` moved to the pi-neutral `cch_support` package
// (include/cch/support/Error.hpp, ADR 0039). This forwarding header keeps the
// historical `cch::util` spelling compiling during the package migration. It
// is scheduled for deletion by the support contraction ticket #469; until then
// the two spellings name the same types.
#include <cch/support/Error.hpp>

namespace cch::util {
using cch::support::Error;
using cch::support::ErrorCode;
using cch::support::Expected;
using cch::support::ExpectedVoid;
using cch::support::make_error;
using cch::support::to_string;
} // namespace cch::util
