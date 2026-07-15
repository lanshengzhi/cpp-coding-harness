# 16 — Validate the parity slice and close verified plans

**What to build:** Produce final evidence that the event/prompt parity slice and the already-implemented SessionFactory assembly satisfy their contracts before changing any plan status.

**Blocked by:** 15 — Synchronize domain, routing, and parity documentation.

**Status:** implemented

- [x] Use PRD stories 48–52, the parity definition of done, and every SessionFactory completion criterion as the validation authority.
- [x] Run the focused agent, tool-executor, SDK, prompt, CLI, session, JSON-event, RPC, and architecture slices and record the commands and outcomes.
- [x] Build the system preset and finish with `ctest --preset system`; any failure is resolved or recorded as a blocker rather than waived.
- [x] Mark the SessionFactory plan complete only if its focused checks and full suite pass and every completion criterion has observable evidence.
- [x] Confirm removed event alternatives, PromptResult models, per-prompt sinks, session-owned CommandRegistry, SDK commands, schema metadata, and terminal records cannot return unnoticed.
- [x] Record that live-provider/network tests were intentionally skipped because fake providers and local sessions satisfy this PRD's validation scope.

## Comments

### Closing findings and fixes

- The initial completion audit found one real SessionFactory blocker: if `HOME` was the workspace, the default `~/.cpp-harness/trust.json` was project-controlled and could authorize project resources. `SessionFactory` now treats a default trust path equal to or below the workspace as unavailable, so project-resource authorization fails closed with a diagnostic. Explicit same-run trust overrides remain adapter-owned user intent.
- Added SDK assembly coverage for fresh session identity, creation timestamp preservation on resume, workspace-equal trust paths, project-controlled default trust state, symlinked-parent containment, indeterminate containment, and valid external not-yet-created trust paths. A process-level CLI scenario independently proves the shared default trust policy fails closed.
- Strengthened architecture coverage so the supported `AgentLifecycleEvent` alternatives are an explicit compile-time set; removed queued/thinking/tool-stream alternatives, session-owned `CommandRegistry`, SDK command contracts, prompt-result models, and duplicate prompt event paths are rejected.
- Existing JSON, RPC, and process-level CLI assertions reject `schemaVersion`, sequence/envelope metadata, and `runtime_terminal` records. Existing move-only callback tests continue to protect `std::move_only_function` event sinks.

### SessionFactory completion evidence

The plan at `docs/plans/2026-07-12-001-refactor-deepen-session-factory-assembly-plan.md` now records the criterion-by-criterion evidence and is marked `status: completed`. Both source-facing paths use the same private assembly implementation; persistence visibility, provider precedence, trust/resource policy, tool visibility, capability ownership, diagnostics, SDK topology limits, and documentation claims have focused evidence.

### Validation

```text
cmake --preset system                                      PASS
cmake --build --preset system                              PASS
./build/cpp_harness_tests "[agent][async]"                PASS (56)
./build/cpp_harness_tests "[agent][tool-executor]"        PASS (12)
./build/cpp_harness_tests "[tools][async]"                PASS (10)
./build/cpp_harness_tests "[sdk]"                         PASS (59)
./build/cpp_harness_tests "[coding_agent][prompt]"        PASS (46)
./build/cpp_harness_tests "[config][resolution]"          PASS (23)
./build/cpp_harness_tests "[coding_agent][project-resource-loader]" PASS (12)
./build/cpp_harness_tests "[cli]"                         PASS (67)
./build/cpp_harness_tests "[harness][session]"            PASS (55)
./build/cpp_harness_tests "[coding-agent][runtime][session]" PASS (8)
./build/cpp_harness_tests "[coding-agent][json-events]"   PASS (5)
./build/cpp_harness_tests "[coding-agent][runtime][rpc]"  PASS (13)
./build/cpp_harness_tests "[architecture]"                PASS (22)
ctest --preset system --output-on-failure                  PASS (1/1)
```

Live-provider and network tests were intentionally skipped. Fake providers, process transcripts, and local session files cover this PRD's contract, failure, persistence, and protocol scope without credentials or network access.
