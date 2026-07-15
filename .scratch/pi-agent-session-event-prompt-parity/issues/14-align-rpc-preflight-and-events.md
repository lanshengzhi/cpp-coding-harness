# 14 — Align RPC preflight and event interleaving

**What to build:** Let RPC clients receive direct AgentSession events interleaved with response records, with prompt acceptance acknowledged only after AgentSession preflight succeeds.

**Blocked by:** 13 — Contract AgentSession to one prompt and subscription path.

**Status:** implemented

- [x] Use PRD stories 31–33 and 36 and the current pi RPC prompt acknowledgement rules as the protocol authority.
- [x] RPC emits direct AgentSession events and response records without a startup session header or runtime-terminal record.
- [x] A valid prompt gets one success response after preflight acceptance and before later execution events; it never gets a second response for execution outcome.
- [x] Closed, busy, empty, or otherwise preflight-rejected prompts produce RPC error responses without claiming acceptance.
- [x] The currently supported command set, malformed-command recovery, shutdown, clean EOF, and response correlation remain adapter-owned and covered.
- [x] Focused `[coding-agent][runtime][rpc]` and `[cli][rpc]` transcript tests pass.

## Comments

### Implementation

- Added a private AgentSession prompt-access adapter so RPC can acknowledge acceptance at the real preflight boundary without adding an RPC callback to the public `PromptOptions` contract.
- RPC now flushes the success response before direct AgentSession events, flushes every event as it streams, and emits no runtime-terminal record.
- Preflight rejection remains a correlated `success:false` response. Failures after acceptance produce no second response and leave the command loop available for later state or shutdown commands.
- Closed, busy, empty, malformed, EOF, shutdown, correlation, and earlier-subscriber-failure paths have transcript coverage.

### Validation

- Red: terminal-removal, accepted-failure recovery, closed-preflight, explicit-preflight, and streaming-flush transcript tests failed before their respective implementation slices.
- Focused: `[coding-agent][runtime][rpc]` (13 tests), `[cli][rpc]` (6 tests), `[coding-agent][json-events]` (5 tests), `[sdk]` (53 tests), and `[architecture]` (20 tests) pass.
- Full: `ctest --preset system --output-on-failure` passes (1/1 CTest target; complete executable suite).
- Two-axis code review completed; the final standards and spec passes reported no remaining blockers.
- Live provider validation was skipped because fake/local transcript tests cover this protocol-only change without network credentials.
