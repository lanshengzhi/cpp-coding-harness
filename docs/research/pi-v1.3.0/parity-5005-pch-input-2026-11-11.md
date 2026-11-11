# PARITY-5005: declared PCH input never produced a generated PCH (2026-11-11)

## Scope

`cch_parity_gate_fixture` failed on every tree, including a clean `origin/main`
export: the legal fixture's `cch_ai` target declares a `PCH_INPUT`, but the
configure-time/post-generation Gate reported

```text
PARITY-5005: target 'cch_ai' declares PCH input '.../pch.hpp' but has
0 generated PCH compile commands; expected exactly one
```

The change is confined to the Parity Architecture Gate's CMake constructor and
its architecture fixture/tests. No production runtime behavior changes.

## Root cause

Not a depfile race (#1162). The failure is deterministic, occurs before any
compilation, and names `PARITY-5005`, not `PARITY-6002`/`6003`.

Commit `71ad3e83a` ("build: benchmark cold builds and validate PCH evidence")
added `_check_declared_pch_commands` in `cmake/parity/parity_gate.py`. For every
index target whose `pch_input` is non-null, it requires exactly one generated
`CMakeFiles/<target>.dir/cmake_pch.hxx.cxx` compile command, writing the
target-local `.gch`/`.pch` artifact and including CMake's `cmake_pch.hxx`
wrapper. That is the real CMake `target_precompile_headers` evidence shape.

The same commit did not change the constructor. Since #449
(`9b33477fa`), `cch_parity_declare_target` implemented `PCH_INPUT` as

```cmake
target_compile_options(${arg_TARGET} PRIVATE -include "${abs_pch_input}")
```

— a plain forced include. It recorded `pch_input` in the ownership index but
never created a PCH source, wrapper, artifact, or compile command. Only the
parity fixtures declare `PCH_INPUT`; production has none. The new rule therefore
rejected the fixture that had always represented the declared-PCH path, because
the field's producer and consumer had different contracts.

The failing fixture is the correct representation of the intended contract; the
constructor was the stale seam. Treating the rule as a false positive and
removing the generated-command requirement would let a target claim PCH context
it never compiles.

## Fix

- `cmake/ParityArchitecture.cmake`: `PCH_INPUT` now uses
  `target_precompile_headers(${arg_TARGET} PRIVATE "${abs_pch_input}")`
  instead of `-include`. CMake generates the wrapper and the target-local PCH
  compile command at configure time; its wrapper includes exactly the declared
  input, which is what `PARITY-5005` validates.
- `tests/architecture/ParityGateTest.cmake`: after the legal fixture's
  post-generation Gate, explicitly assert that the fixture still records
  `cch_ai`'s `pch_input` and that `compile_commands.json` contains exactly one
  generated PCH command writing `CMakeFiles/cch_ai.dir/cmake_pch.hxx.gch` (or
  `.pch`) and including the CMake wrapper. This makes the fixture fail on the
  old constructor and on any future removal of the declaration, not only inside
  the rule it is meant to exercise.
- `tests/architecture/parity_gate_test.py`: add
  `test_declared_pch_input_without_generated_command_is_rejected`. It models the
  old constructor (`-include Pch.hpp`, no generated command) and requires
  `PARITY-5005`. Run against `71ad3e83a^`'s validator, the test fails with
  `[] != ['PARITY-5005']`; the current validator passes it. The rule is therefore
  a discriminating regression, not a relaxed check.

The Gate rule itself is unchanged. No SKIP was added and no threshold was
lowered.

## Validation

- Focused: `ctest --preset vcpkg -R '^cch_parity_gate_fixture$'` — 1 selected,
  1 passed (5.75 s). Same case before the fix: 1 failed with `PARITY-5005` and
  the 0-generated-command message.
- Focused unit: `python3 tests/architecture/parity_gate_test.py
  CompileContextTest...` — 2/2 passed; `cch_parity_gate_unit` passed.
- Architecture: `ctest --preset vcpkg -L architecture -j4` — **43 selected,
  43 passed, 0 failed** (58.10 s).
- Full Debug: `cmake --build --preset vcpkg` passed, including the build-phase
  Parity Architecture Gate (`PASS`).
- Full CTest with the documented host precondition (`TERM_PROGRAM=herdr`):
  **4 593 selected, 4 582 passed, 11 skipped, 0 failed** (275.43 s). The first
  run without that variable had one host-precondition failure in
  `real PTY records Herdr markdown and OSC 52 terminal bytes`
  (`host_term_program == "herdr"`, actual `<unset>`), not a code failure.
- `scripts/format-check.sh` (working tree vs HEAD): clean; `git diff --check`:
  clean.

## Receipts

No delivery receipt in `docs/research/pi-v1.3.0/1058-deliveries.json` lists any
of the changed paths (`cmake/ParityArchitecture.cmake`,
`tests/architecture/ParityGateTest.cmake`,
`tests/architecture/parity_gate_test.py`) as an input. No receipt digest is
re-bound here. Historical records that quote earlier
`parity_gate_test.py` case counts (for example `1068-session-ownership.md`,
`1069-session-headless-consumers.md`, `1073-application-environment-tools.md`)
are dated observations, not live receipts, and are left intact.

## Residual risk

- Production currently declares no `PCH_INPUT`, so the constructor change does
  not alter any production compile command. A future target that declares one
  now pays real PCH generation cost and must satisfy the Gate's wrapper/artifact
  checks.
- The Gate validates the generated command, wrapper contents, and every declared
  source's use of the wrapper; it does not require the `.gch`/`.pch` file to
  exist at post-generation time. A build-phase PCH artifact check remains out of
  scope and is not implied by this fix.
- `PCH_INPUT` on an `INTERFACE` target remains a nonsensical declaration; the
  Gate fails it closed with `PARITY-5005` because no compile command can exist.
