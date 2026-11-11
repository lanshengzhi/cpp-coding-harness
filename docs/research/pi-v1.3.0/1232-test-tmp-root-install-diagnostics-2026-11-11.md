# Test temporary root, staged-install diagnostics, and space pre-flight (2026-11-11)

## Scope

Close-out step 3 for #1232: remove the test runner's hardcoded `/tmp`
isolation base, keep a staged-install failure's first cause visible, and add a
space pre-flight before the ≈417 MiB install stage. The work is confined to the
Catch2 test harness and the staged-install/TUI tests it runs; no production
runtime behavior is changed.

## Delivered behavior

- `tests/Catch2Main.cpp` resolves its isolation base with
  `std::filesystem::temp_directory_path(std::error_code&)` **before**
  `setenv(TMPDIR)`. It rejects an unresolved, non-directory, or non-writable
  base with one line naming the base and the selected `TMPDIR`; the 100-attempt
  unique-root loop and `remove_root()` semantics are retained. The resolved base
  and the inherited `TMPDIR` are exported as `CCH_TEST_ISOLATION_BASE` and
  `CCH_TEST_INHERITED_TMPDIR` for the regression.
- `tests/support/TestRunnerIsolationTest.cpp` asserts the isolation root is a
  direct child of the resolved base, that the base is the inherited `TMPDIR`
  when one was set, and that it is `/tmp` when none was set.
- `tests/cli/InstallRelocationTest.cpp` reports `WIFEXITED` and `WIFSIGNALED`
  separately, includes the capture directory and errno text when capture setup
  fails, quotes paths through `shell_quote`, and uses `UNSCOPED_INFO` so a
  failing assertion does not leave a scoped Catch2 message behind on this
  `-fno-exceptions` test TU.
- A new test proves the command result distinguishes exit code 7 from
  `SIGTERM`, and that a capture-directory failure names its path.
- Before `cmake --install`, the staged-install case checks free space on the
  staging filesystem. The requirement is `max(CCH_TEST_MIN_TMP_BYTES,
  2 * pike + 64 MiB)`; insufficient space `SKIP`s with one message containing
  the path, available bytes, and required bytes. An invalid injected floor is a
  normal failure. The gate never invokes the install command when insufficient.
- The TUI renderer/export/clipboard assertions that assumed a short `/tmp`
  path now assert the unwrapped warning text, the produced export file, and the
  `fake:` echo label instead of a fixed row/path layout.
- Thirteen delivery receipts pin `tests/Catch2Main.cpp`. Their digest is
  refreshed to
  `36c057ae082c40086358500d486b7bbff9839fa43b94f9981e3efb6b55e4631e`.
  Receipt `1101-structured-content` reran its exact 36-case selection into
  `deliveries/1101-revalidation-test-tmp-root.xml`; the other twelve retain
  their existing JUnit and carry a harness-only `revalidationTestTmpRoot`
  entry. No rows, mappings, or scenarios changed.

## Validation

The full Debug suite ran with `CCH_LANE_NAME=test-tmp-root`,
`TERM_PROGRAM=herdr`, `TMPDIR=/tmp`, and `ctest --preset vcpkg` (`-j 4`):
**selected 4593, passed 4581, skipped 11, failed 1, 288.51 s**. The only failure
was the known `cch_parity_gate_fixture` `PARITY-5005` PCH rejection. The actual
temporary base was `/tmp`; isolation roots have the form
`/tmp/cpp-harness-catch2-<pid>-<stamp>-0`. The 1 s watcher recorded a minimum of
`8,173,268,992` bytes available on `/tmp` (start/end delta `860,160` bytes);
37 stale `cpp-harness-catch2-*` roots were present before and after the run, and
the run added none.

A focused root-disk run with `TMPDIR=/var/tmp/cch-relocation-evidence` passed
4/4 (isolation regression, command diagnostics, space pre-flight, staged
install). The first isolation root was
`/var/tmp/cch-relocation-evidence/cpp-harness-catch2-1149262-70564099294993-0`;
`/tmp` free bytes did not change (`delta=0`, minimum `8,178,601,984`). The
minimum on the root-disk filesystem during the sample was `97,152,659,456`
bytes.

The forced-failure run (`CCH_CMAKE_COMMAND=/bin/false`) ended as a normal
Catch2 assertion failure with `command exit code 1, no terminating signal` and
both streams, not `Subprocess aborted`. With
`CCH_TEST_MIN_TMP_BYTES=999999999999999` and a nonexistent CMake command, the
staged-install case skipped in 0.01 s with path, available
`98,745,720,832`, and required `999,999,999,999,999` bytes; the install command
was not executed. A nonexistent `TMPDIR` produced
`Catch2 test environment setup failed: could not resolve the test temporary
directory (TMPDIR=/nonexistent/cch-test-root): No such file or directory` and
exit 1. With `TMPDIR` unset the isolation regression passed and resolved `/tmp`.

Consolidation `build` and `check` passed (1464 rows, 1009 scenarios, 331
explicit orphans, 0 uncovered); both consolidation CTest cases passed
(32.99 s). `scripts/format-check.sh` and `git diff --cached --check` are clean.

## Failure mechanism and residual roots

On this `-fno-exceptions` test TU a `REQUIRE` failure throws from Catch2
through a frame with no cleanup landing pads, so the failing test's local
`TempWorkspace` destructor never runs. The install case's stage is the
`pike` binary plus notices, observed previously at ≈417 MiB. When the old
hardcoded `/tmp` filled, each failed install case leaked its whole isolation
root, including the stage, making the next run more likely to fail (single-run
pass, full-suite fail). The same no-destructor rule left a scoped `INFO`
unpopped, and Catch2 aborted on the leftover message before the installer's
later output could be printed. The 37 roots observed here are empty leftovers
from earlier runs; the 2026-11-10 record reported 38, so one was removed
between observations.

## Capability status

No capability row advances. The change makes the test harness's chosen
temporary filesystem observable and keeps staged-install failures diagnosable;
it does not change Runtime, install, or provider behavior.

## Remaining boundary

- CI does not exercise this fix: the workflow does not export `TMPDIR`, and the
  hosted runner's `/tmp` is on disk rather than the 7.8 GiB tmpfs that produced
  the original failure. The hardcoded path was therefore never exhausted in CI.
- A root-disk (`/var/tmp`, btrfs) full-suite attempt exposed two
  filesystem-timing failures that pass on tmpfs: `deferred User Bash
  persistence failure ...` completes its 293-byte append but does not resume
  within the 30 s budget, and `repeated tool_search calls ...` reports
  `runtime admission capacity is saturated`. These are production
  persistence/admission timing boundaries, not caused by the path-length fixes;
  they are not fixed in this slice.
- Stale-root cleanup was deliberately not added. The names carry a PID and
  timestamp but no lane identity, so sweeping `/tmp/cpp-harness-catch2-*` could
  delete a live parallel lane's isolation tree.
