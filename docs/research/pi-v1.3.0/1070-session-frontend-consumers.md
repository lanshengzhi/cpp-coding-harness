# Application Session frontend consumers — #1070

## Scope

B12 / P6d migrates the CLI and Native TUI Session consumers onto the application
ownership established by [#1068](https://github.com/lanshengzhi/cpp-coding-harness/issues/1068)
and [#1069](https://github.com/lanshengzhi/cpp-coding-harness/issues/1069), under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md)
and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044). The selected
comparison baseline remains `pi-v1.3.0` / `71023aec1546d6d677a65669c5665c0927ce0dd0`,
Linux x86-64 with glibc. This is a consumer-migration and contraction slice: it moves the
frontend callers onto the application paths, removes every temporary Session forward, and
claims no new upstream parity.

## Delivered behavior

Every application source now reaches Session through its own Owner Interface spelling.
The five frontend translation units and headers — `TreeSelector` (component and header),
`SessionUiBinding`, `SessionFlowController`, `HtmlExport` and `InteractiveEngineHost` —
include `<cch/coding_agent/session/...>` for the Session value/store/tree/compaction
interfaces and `"coding_agent/session/TranscriptExport.hpp"` for the transcript-export
helper, matching the headless callers migrated in #1069. Twelve CLI and Native TUI test
sources follow (`tests/cli/CliSmokeTest.cpp` and eleven `tests/coding_agent/tui/` suites),
so no test still compiles against the legacy spelling.

Every temporary Session forward is removed. The five public redirects under
`src/agent/include/cch/agent/harness/session/` and the eight private redirects under
`src/agent/harness/session/` plus `src/agent/harness/TranscriptExport.hpp` are deleted, and
the `interface_forwarders` / `private_forwarders` records leave the manifest entirely
rather than lingering empty. The Gate's forwarder machinery — manifest parsing, the
redirect-content check, and the standalone-compile allowance — is removed with them: the
legacy spelling no longer resolves to a file owned by anyone. The `PARITY-8009` rule
`application-session-no-legacy-core-session-includes` keeps its name and diagnostic but
loses the `tui/`/`cli/` `excluded_source_prefixes`, so any application source that names a
legacy core Session path is a contract violation; the standalone Owner Interface compile
checks each legacy spelling only in the synthetic fixture trees that still declare it, and
the real Gate reports `PARITY-4008` for it. The miniature architecture and install
fixtures no longer mirror redirects; their shared helper is a plain tree copy. The two
session paths also leave the `no-agent-private-reach-through` forbidden list, since the
headers they guarded no longer exist; the text-limiting and compaction entries stay.

The change is call-site relocation and form removal, not behavior: no Session store,
tree, resume, compaction or export semantics move, and the `cch::harness::session`
namespace vocabulary is preserved exactly as #1068 left it.

## Validation

- Focused Validation: `cmake --build --preset vcpkg -j4` passed after a reconfigure
  (`VCPKG_ROOT=<root> cmake --preset vcpkg`, required by `PARITY-4011` because the
  include sets changed). The owning module label `ctest --preset vcpkg -L coding_agent`
  selected **1,613** cases and passed **1,613/1,613** (44.77 s); the migrated-subject
  selection `ctest --preset vcpkg -R 'session|Session|compaction|Compaction|transcript
  export'` selected **438** cases and passed **438/438** (9.10 s). The frontend TUI label
  `ctest --preset vcpkg -L tui` selected **1,653** cases and passed **1,653/1,653**
  (39.45 s) with the existing optional `Native TUI differential capture` skipped. The CLI
  label `ctest --preset vcpkg -L cli` selected **284** cases and passed **284/284**
  (5.22 s).
- Behavior is observed through the highest public caller, not through includes. A new
  `[issue1070]` case (`a frontend resume, fork and tree navigation reach the migrated
  application Session`, in `SessionSelectorInteractiveTest.cpp`) boots the interactive
  TUI over a real persisted transcript written through the application store, resumes it
  through the selector's asynchronous replacement seam (`ExplicitResumeSessionTarget`),
  navigates the session tree in place through the double-Escape overlay, and forks through
  the user-message selector (`ExplicitOpenOrCreateSessionTarget`), asserting the branched
  file's parent pointer and four-entry prefix history as reloaded through the application
  store. The discriminating failure case drives the same public selector: it lists a
  session, deletes the file between listing and Enter, and asserts that the missing path
  still reaches the seam as an `explicit-resume` request whose completion settles as the
  presented `session file does not exist` error while the forked session stays live —
  which a pure spelling migration cannot distinguish from a working resume.
  `ctest --preset vcpkg -L issue1070` selected **1** case and passed
  **1/1**.
- The migration is enforced, not just performed. `python3 tests/architecture/parity_gate_test.py`
  passes **121** cases, including the updated `PARITY-8009` cases: the rule's exclusion is
  empty; a `tui/` source, a `cli/` source, a Session source and an Owner Interface header
  using a legacy spelling each reach `PARITY-8009`; and a fixture tree declaring only the
  application interface reports `PARITY-4008` (unresolved) for the legacy spelling rather
  than a rule rejection — the property the removed forwards used to mask. A new manifest
  case asserts the redirect records are absent from the checked-in manifest.
- The architecture selection `ctest --preset vcpkg -L architecture` selected **43** cases
  and passed **43/43**, including the standalone Owner Interface compilation, the warning
  gate, both real-Gate miniature fixtures with their now plain-copy source preparation,
  the strict no-exception evidence and the build-phase Gate.
- Capability evidence: the #1058 consolidation check, boundaries and named-evidence cases
  (`ctest --preset vcpkg -R 'cch_pi_v130'`) passed **3/3**, so the capability rows stay
  exactly as #1069 left them; the relocation advances none.
- `scripts/format-check.sh b597ccb58eca54977a65355bc60a9bedaa998feb` and `git diff --check` pass.
- Full Validation: the repository default `ctest --preset vcpkg` (run after
  `cmake --preset vcpkg && cmake --build --preset vcpkg -j4` from a clean build state)
  selected **3,720** cases and passed **3,720/3,720** (140.18 s) with the existing
  optional `Native TUI differential capture` skipped.

## Capability status

No capability row changes. This slice relocates the remaining consumers and removes the
temporary forms; it observes no new upstream behavior, so membership, implementation and
verification statuses in the #1058 ledgers stay as #1068/#1069 left them. The
`cch_coding_agent:Session:assembly:createAgentSession` and
`cch_coding_agent:Session:event-commitment` rows remain `unverified` for the same reason
as before — the new `[issue1070]` case is an ordinary C++ regression through the
frontend seam, not a frozen-capture comparison, so it does not advance that status.

## Remaining boundary

#1073 removes the two residual `session-no-core-private-includes` private-helper
exceptions (the journal's core-private POSIX write helper and export's concrete workspace
filesystem) when the concrete environment/shared mechanics migrate. The Durable Runtime
workflows, MCP generic ownership and the rest of #1044 stay open on their own issues; this
slice does not edit or close the umbrella. #1044 remains open and is not edited by this slice.
