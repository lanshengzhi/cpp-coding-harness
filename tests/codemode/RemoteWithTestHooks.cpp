// Compile the Remote implementation a second time for the codemode shard with
// its private lifetime hooks enabled (#1232). The archive's `Remote.o` is not
// linked here: this translation unit defines every symbol it would supply, so
// the production archive stays free of test-only seams.
// GCC warns about coroutine frames raised by anonymous-namespace lambdas when
// the implementation is included; the ordinary translation unit does not.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "../../src/codemode/Remote.cpp"
