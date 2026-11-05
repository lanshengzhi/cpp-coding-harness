// The portable no-Node entry's self-containment translation unit (#1194).
// Including every portable header here, then failing the compile when a
// native-only header crossed the surface, is the build-level half of the
// portable guarantee: a target that secretly imports Boost.Asio or WasmEdge
// through the portable headers cannot build. `CodemodeTests.cmake` links the
// portable check shard against `cch_codemode_portable` alone, and
// `cmake/targets/Codemode.cmake` fail-closes the declared dependency graph at
// configure time.

#include <cch/codemode/Declarations.hpp>
#include <cch/codemode/Portable.hpp>
#include <cch/codemode/Prelude.hpp>
#include <cch/codemode/Source.hpp>
#include <cch/codemode/Types.hpp>

#if defined(BOOST_ASIO_HPP)
#error "the portable codemode subset must not import Boost.Asio"
#endif
#if defined(WASMEDGE_C_API_H)
#error "the portable codemode subset must not import the WasmEdge C API"
#endif

namespace cch::codemode {

// A translation unit that only compiles is not linkable on every toolchain;
// keep one trivially reachable symbol so the portable target is a real
// library the check shard links against.
const char kCodemodePortableSubset[] = "cch_codemode_portable";

} // namespace cch::codemode
