# Persist branch continuation after resume

Status: implemented

## Parent

.scratch/session-resume-module/PRD.md

## What to build

After a session resumes from an active branch, continuing the conversation should make the newly appended messages the next resume point. A later resume should continue from that new conversation path instead of snapping back to an older persisted `Leaf` target.

The runtime should preserve the narrow `AgentSessionRuntime` seam: it should not hold or directly navigate `SessionTree`. Any required persistence behavior should be exposed through the session or store seam at an appropriate level.

## Acceptance criteria

- [x] A session can resume from a valid active `Leaf` target and then accept a new prompt.
- [x] After that prompt is persisted, a second resume includes the continuation messages.
- [x] The second resume does not snap back to the old `Leaf` target and drop the continuation.
- [x] The implementation does not make `AgentSessionRuntime` directly hold, inspect, or navigate `SessionTree`.
- [x] Branch continuation behavior is covered at an `AgentSession` prompt-after-resume seam.
- [x] Existing linear resume and compaction resume behavior still passes.

## Blocked by

- .scratch/session-resume-module/issues/01-deepen-session-resume-open-session.md

## Comments

- Implemented by `c0d25d5 Persist branch continuation after resume`.
- Verified in the final module validation with `./build/cpp_harness_tests "[coding-agent][runtime][session]"`, `./build/cpp_harness_tests "[harness][session][tree]"`, and `ctest --test-dir build --output-on-failure`.
