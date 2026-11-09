# Remote retirement candidate slice, 2026-11-09

## Scope and contract

This is an unintegrated candidate based on `bae3a2b58345287d560048b4d938e2affe6a8a79`. `RemoteRetirement` counts admitted participants and stop-registration leases, including callback invocation and capture destruction. Closing admission and releasing the last lease publishes the participant witness. An abandoned participant intentionally leaves it pending. A callback may synchronously reenter retirement during construction, and the lease is reserved before user copy/move code runs. Individual participant access requires serialization.

An executor observer uses a posted notification and a concurrent channel. A false wait result is not a witness. The participant witness excludes terminal delivery, unregistered work, the posting tail, queued notification, completion handlers, captures, and executor-bearing tails. The caller must retain the executor and borrowed resources until these independently quiesce; a completed wait alone does not prove this. Executor destruction concurrent with publication has not been proved safe. Post failures are counted in exception-enabled builds and can leave a wait pending; supported no-exception builds terminate on throwing allocation. This candidate is not integrated with the production Remote producer/consumer interfaces and is not a complete teardown proof. Observer-list cost, multithread stress and TSan remain unmeasured.

## Slice and dependency audit

The only code inputs are `src/codemode/RemoteRetirement.hpp`, `tests/codemode/RemoteRetirementTest.cpp`, `cmake/tests/TestAggregate.cmake`, and `tests/codemode/retirement-standalone/CMakeLists.txt`. The standalone target also uses the tracked `src/support/BoostExceptionHandler.cpp` and pinned Boost and Catch2. No untracked Sandbox, Remote, worker-test, ADR or other research candidate is required. The independent normal-graph shard links the imported Catch2 target, is included in `pike_tests`, discovers cases individually with tags and a 10-second CTest timeout (§11.1 and §11.6); actual normal-graph build and discovery have not completed. Its dependency on the parity build gate causes a large cold build. Test threads are released and joined before discriminating assertions, with individual waits bounded and CTest as the process backstop (§11.10).

Remaining review includes the owning raw `new` in callback construction and test setup (§7.7), the private failure injection seam, the notification lifetime contract, and full evidence-pin/transitive build impact review. No receipt was rebound. The earlier [red observation](1232-retirement-review-red-2026-11-09.md) and [intermediate focused record](1232-retirement-fix-focused-2026-11-09.md) explicitly describe historical mixed trees, not this slice.

## Slice validation

Under the main checkout's `retirement-slice-1232` lane lock, a separate `/tmp` clone at the base above was configured with `dev-fast`, using the pinned vcpkg checkout. Its configure-phase Parity Architecture Gate passed (13 targets, 68 dependencies, 287 sources, 3960 includes). The normal-graph shard cold build failed with `Disk quota exceeded` while archiving `libcch_tui.a` and writing a compiler temporary file. It did not reach the build-phase gate or test discovery. Rebuildable partial objects were removed from this clone afterward. No full suite or formal shard ASan result is claimed.

The standalone target was configured and built inside the slice. Strict Clang tests passed 15/15; strict GCC ASan/UBSan tests passed 15/15 with leak detection. The test and Boost exception-handler translation units were sanitized; the prebuilt Catch2 library's instrumentation was not verified. The exception-enabled Clang debugging variant passed 18/18, including the injected post-failure and throwing-copy/move cases. That variant is not a supported production configuration. CTest gives each case a 10-second timeout. These results do not prove race freedom or production integration.

From the root of this slice, with `VCPKG_ROOT` set to a checkout at the manifest's pinned baseline, reproduce the standalone results (the initial configure populates this slice's own pinned dependency directory):

```sh
cmake --preset dev-fast
cmake -S tests/codemode/retirement-standalone -B build/retirement-standalone -G Ninja -DCMAKE_PREFIX_PATH="$PWD/build/dev-fast/vcpkg_installed/x64-linux" -DCMAKE_CXX_COMPILER=clang++
cmake --build build/retirement-standalone -j 2
ctest --test-dir build/retirement-standalone --output-on-failure --timeout 10
cmake -S tests/codemode/retirement-standalone -B build/retirement-asan -G Ninja -DCMAKE_PREFIX_PATH="$PWD/build/dev-fast/vcpkg_installed/x64-linux" -DCMAKE_CXX_COMPILER=g++ '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -O0' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/retirement-asan -j 2
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/retirement-asan --output-on-failure --timeout 10
cmake -S tests/codemode/retirement-standalone -B build/retirement-exceptions -G Ninja -DCMAKE_PREFIX_PATH="$PWD/build/dev-fast/vcpkg_installed/x64-linux" -DCMAKE_CXX_COMPILER=clang++ -DRETIREMENT_NO_EXCEPTIONS=OFF
cmake --build build/retirement-exceptions -j 2
ctest --test-dir build/retirement-exceptions --output-on-failure --timeout 10
```

Tested source SHA-256 values: header `e7ce1fc1269b4fd4e1bfe2a33125f8775ead0238024f9aed876648f4c4f9d2d8`, test `415cd14a86b564c2414f374db35c22524d98108d1c910fdb65ba1f488ad4e7d0`, aggregate `35089a707b3d2d5331d2508bb62778c8b4b177197114ca3ed1c372706cdf1ff6`, standalone CMake `b3454199b9984c4bf25a1595d28ae8c303fa978aef77e824bf1620d98c98c1ef`. These hashes identify the tested files, not a commit.

An initial configure command was inadvertently run in the main checkout; it reconfigured `build/dev-fast` and refreshed that build directory's vcpkg installation. No output from that mixed checkout is used as slice evidence. It did not edit tracked source. The user-owned candidate worktrees remain untouched. The slice is retained uncommitted because formal shard validation and production integration remain open.
