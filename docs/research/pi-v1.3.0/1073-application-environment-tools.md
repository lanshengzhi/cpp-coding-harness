# Application execution environment and built-in tools — #1073

## Scope

B15 / P9 moves the application's concrete execution environment and built-in
read/write/edit/bash tools out of Agent core, under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and [#1073](https://github.com/lanshengzhi/cpp-coding-harness/issues/1073).
The Named Baseline remains `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.
This is an ownership relocation that preserves existing behavior; it makes no
new upstream comparison claim.

## Delivered behavior

The application owns the local filesystem, shell, process runner, runtime
admission/mailboxes, workspace path resolution, atomic writes, and built-in tool
factories. Their implementation lives in `src/coding_agent/environment/` and
`src/coding_agent/tools/`; the filesystem/shell contracts and local adapters use
`<cch/coding_agent/environment/...>`, and tool factories use
`<cch/coding_agent/tools/ToolFactories.hpp>`. Application assembly still injects
filesystem and shell capabilities into tools, then supplies passive `agent::Tool`
values to the Agent. The existing `cch::harness` and `cch::tools` namespaces remain.

Core owns Agent execution, tool execution, nested calls, and the tool registry.
Its 11 concrete environment/tool translation units and utf8proc dependency move
to `cch_coding_agent`. The CMake File API reports exactly 10 core translation
units and a target dependency closure of `cch_agent_core`, `cch_ai`, and
`cch_support`, with no application translation unit. Inspecting the three built
archives with `nm -C -g --defined-only` finds zero `cch::harness` or `cch::tools`
definitions. These observations check the compiled closure as well as the source
placement: changing only header spellings while retaining the old concrete
objects in the archive would fail that separation.

The runtime's measured admission policy and the POSIX/atomic-write helpers have
application consumers only, so they remain private to that Owner. Existing
pi-neutral async, JSON, fd, text-limiting and redaction mechanics remain in
`cch_support`; no additional shared interface is introduced. Session journal and
export now use application-owned helpers. Both temporary #1073 exceptions leave
the manifest. `PARITY-8007` retains its historical rule ID but rejects all
application private and Owner Interface includes from core; `PARITY-8008` has no
remaining helper exception. There are no environment/tool forwarding headers.

The six existing environment/tool test files move under `tests/coding_agent/`,
retain their focused `cch_tests_harness_tools` shard, link the application Owner,
and gain its module label. Benchmark routing follows their new paths.

## Validation

- Focused build: `cmake --build --preset vcpkg --target
  cch_tests_coding_agent cch_tests_harness_tools -j4` passed after reconfiguring
  with `VCPKG_ROOT=<root> cmake --preset vcpkg` for changed include evidence.
- `ctest --preset vcpkg -N -L issue1073` selected **3** cases;
  `ctest --preset vcpkg -L issue1073` passed **3/3** in **0.37 s**.
  `ApplicationToolsTest.cpp` drives real tools through assembled Agent Session:
  write/read/edit/read leaves `after\n`, and bash reads it together with a
  workspace-only marker and returns stderr. The marker distinguishes a correctly
  bound workspace from tools consistently using the process working directory.
  A failed edit followed by read retains `original\n`, while a shell exit of 7
  retains its output and error status. The cancellation case waits for the real
  shell to create its readiness file, aborts Session, waits for prompt settlement,
  and observes `Command aborted` with Session idle. The existing model bash tool
  buffers output; this test does not infer streaming Session updates.
- `ctest --preset vcpkg -N -L '^(harness|tools)$' -LE architecture` selected
  **182** cases; the matching focused run passed **182/182** in **2.65 s**,
  including real shell cancellation/process-group reaping and environment/tool
  regressions.
- `python3 tests/architecture/parity_gate_test.py` passed **121/121** cases.
  Updated cases exercise actual rejection of application environment/tool
  includes from core, including canonical Owner Interface spellings, and both
  formerly excepted journal/export sources using retired helper paths.
- All **29** relocated production files compare equal to the initial revision
  `c454477cc2a9a11c980b949ddf601b660308abf0` after applying the declared include
  substitutions and the same clang-format configuration. No implementation body
  changes are hidden in the relocation or required whole-file formatting.

- `ctest --preset vcpkg -N -L architecture` selected **43** cases;
  `ctest --preset vcpkg -L architecture` passed **43/43** in **321.58 s**,
  including standalone Owner Interface compilation, the warning gate,
  configured ownership/include evidence, and strict no-exception checks.
- Standards and Spec reviews completed independently. The one Standards
  include-spacing finding was fixed; a follow-up review of the corrected
  cancellation synchronization found no remaining issue. Spec had no finding.
- The full incremental build passed. It emitted one existing unused-helper
  warning for `scenario_of` in the unchanged `SystemThemeTest.cpp`.

- The first unfiltered `ctest --preset vcpkg` selected **3,723** cases in
  **175.35 s**: **3** failed and **1** optional differential-capture case was
  skipped. Two failures rejected stale #1101 delivery digests after this move;
  the third reported a scrollbar-cell mismatch in the unchanged fullscreen
  scroll-indicator test. That TUI case passed alone (**1/1**, **1.45 s**);
  the failure's underlying cause is not established and its assertion is intact.
- The existing #1101 frozen-comparison selection was rerun with a new JUnit
  artifact, [1073-1101-validation.xml](1073-1101-validation.xml): **14/14** passed
  in **0.81 s**. Its receipt refreshes only four affected input hashes, the
  validation command/result pointer, timestamp and tested checkout base. The
  historical result file is retained. Consolidation `build` and `check` pass;
  the two consolidation CTest cases passed **2/2** in **3.06 s**.

- Final Full Validation: `cmake --build --preset vcpkg -j4` passed, then
  unfiltered `ctest --preset vcpkg -j2` selected **3,723** cases and completed
  with **3,722 passed**, **0 failed**, and **1 skipped** in **261.73 s** (exit 0).
  The skipped Native TUI differential-capture case requires a separately
  selected dual-runtime capture environment and supplies no terminal acceptance
  evidence. All three cases that failed in the first run passed in this run;
  no assertion, timeout, test label, or selection was relaxed.
- `scripts/format-check.sh c454477cc2a9a11c980b949ddf601b660308abf0`,
  `git diff --check`, and report-relative link checks pass. The follow-up review
  of the receipt refresh found no issue.

## Capability status

No capability row changes. Membership, implementation, and frozen-baseline
verification remain separate. The new Session regressions and existing shell
regressions establish this C++ relocation's behavior; they do not replace
independent frozen-pi observations. All **1,464** #1058 capability rows, including
source audit facts, and every non-delivery ledger field compare exactly equal to
the initial revision. Only the existing #1101 delivery metadata is reconciled
with the current selected inputs and its fresh passing comparison receipt.

## Remaining boundary

Durable's independent environment/tools and all remaining #1044 behavior and
parity gaps remain on their own tickets. This slice preserves Magpie-only AI
access, the XDG root, strict no-exception behavior, and the existing security and
output policies. It adds no historical Pike durable migration, third-party
plugin execution, or terminal-emulator verification claim. #1044 remains open.
