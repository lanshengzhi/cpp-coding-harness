# #1113 Agent queue admission and frozen drain contract

**Status:** implemented and focused/full-verified against the frozen baseline. No capability row is
promoted here: whole-row Agent/application receipts are owned by #1176 (issue reassessment of
2026-10-30), and this report is the comparison evidence those receipts consume.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.13.0, tsx 4.22.1
- **Frozen checkout:** `/tmp/pi-1113-frozen` (clean `HEAD` at the baseline; installed
  dependencies only)
- **Integration base:** `4790e9e3c2d61f31ad31b8a742352389018ad4b6`
- **Implementation commit:** `44784909befc10b5ae181f6a19bf17f7190d37c2`
- **Merge commit:** `05850774f` (merges `origin/main` `f840fc445`, the #1171 continuation slice)
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1113/` — one artifact, two scenarios
- **Registered reuse:** `fixtures/pi-v1.3.0/audit-1047/queue-admission.json` (manifest
  `0d146159a27ceaf1e657cd22997c1ebdc972818215054a860d88226e7d58d59a`) for the default modes,
  drain ordering, peek/clear and continuation scenarios
- **Batch:** B55 / F22 (parent #1044, ADR 0075, accepted #1058 consolidation)

## Frozen evidence

`queue-admission.json` already records the frozen queue behavior this slice aligns with:

- `steer`/`followUp` enqueue without capacity checks and return `undefined`;
- both queues default to `one-at-a-time`;
- one-at-a-time steering injects the oldest message after the tool turn;
- all-mode steering injects every queued message in FIFO order in one turn;
- follow-up runs only after an otherwise stopping assistant turn;
- `peekQueuedMessages` prefers steering and `clearAllQueues` removes both queues;
- `continue` from an assistant tail drains steering before follow-up.

That registered family proves acceptance and ordering, but its 300+300 scenario records only the
enqueue count it supplied, not that all 300 remained queued, and it has no message larger than the
retired 16 MiB byte bound. `audit-1113` adds exactly those missing observations rather than deriving
them from C++:

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `queue-admission-retention.json` | 2 | `3283a503149f6c1f7d384065ba7c5eddefda00a49ede452f02d81e0ebd8bebdc` |

`manifest.json` SHA-256: `f0906d99c431849b1e080eaa074de4872c9bd186428267421e07e999b94d61dd`.
Capture generator SHA-256: `0e9c1f6ae0c35636a10446c067e27b0f2879ed07cc60a88edfb12d68c42a9452`.
The bundle is a candidate, not a registered family: serial registration in
`fixtures/pi-v1.3.0/baselines.json` is owned by the merger (#1176), matching the lane precedent of
#1047/#1108. `python3 scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1113/manifest.json` accepted it and returned the registry record:

```json
{
  "audit-1113": {
    "manifest": "audit-1113/manifest.json",
    "manifestSha256": "f0906d99c431849b1e080eaa074de4872c9bd186428267421e07e999b94d61dd"
  }
}
```

```bash
PI_CHECKOUT=/tmp/pi-1113-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/pi-1113-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1113/capture-1113.mts
# captured audit-1113 (1 artifacts, 2 scenarios) for pi-v1.3.0 (71023aec…)
```

The new scenarios drive the real public `Agent` seam:

1. **Retention and FIFO.** Enqueue 300 steering and 300 follow-up messages; the capture then sets
   each mode to `all` and peeks the full queue. It records default `one-at-a-time` modes, one-item
   default peek (`s0`), 300 retained steering messages (`s0`…`s299`), 300 retained follow-up
   messages (`f0`…`f299`), and `enqueueReturnsVoid == true`.
2. **Retired byte bound.** Enqueue a 16,777,217-byte steering message and a 16,777,218-byte
   follow-up message (the retired cap was 16,777,216 bytes). Both peek back at their exact length
   and SHA-256, so the observation is retention, not just a non-throwing call.

## Implementation

- **Capacity behavior removed.** `AsyncAgentOptions::max_queued_messages` /
  `max_queued_bytes`, `AgentInputQueues::max_messages` / `max_bytes`, the
  `kDefaultMaxQueued*` constants, the approximate-size helpers, `admit_queued_message`, and the
  `AgentSessionCreationRequest` / `AgentSessionConfig` / `SessionFactory` plumbing are gone. No
  public option, snapshot field, or hidden constant can reject a queued message for count or bytes.
  This is the ADR 0075 supersession of ADR 0022's capacity divergence.
- **Frozen enqueue shape.** `Agent::steer` and `Agent::follow_up` are `void` and append directly,
  matching `Agent.steer` / `Agent.followUp`. A moved-from Agent has no live state and ignores the
  call, the same no-op shape as the other void Agent operations.
- **Peek and presence.** `Agent::peek_queued_messages()` mirrors `Agent.peekQueuedMessages()`:
  the mode-selected steering batch when steering has items, otherwise the mode-selected follow-up
  batch, without consuming. `Agent::has_queued_messages()` mirrors `hasQueuedMessages()`.
- **One selection authority.** `queued_selection_count` drives both `peek_queued_messages` and the
  turn machine's `drain`, so one-at-a-time (`1`) and all (`size`) cannot drift between the preview
  and consumption paths. `clear_steering_queue` / `clear_follow_up_queue` / `clear_input_queues`
  are the C++ spellings of pi's `clearSteeringQueue` / `clearFollowUpQueue` / `clearAllQueues`.
- **Session adapter.** `AgentSession::steer` / `follow_up` still return `ExpectedVoid`, but the only
  remaining rejection is the closed-session check; capacity is not an error shape. The TUI keeps
  its existing closed-session diagnostic path and no longer has a capacity-rejection path to
  exercise.
- **Tests.** The Agent queue tests now compare the frozen request roles, last-user FIFO order,
  retained invocation message counts, peek/clear and continuation. The Session and Native TUI
  tests exercise unbounded admission through the highest public caller, including input that the
  retired count/byte caps rejected.

## Acceptance and separating cases

| Acceptance | Normal case | Separating case |
|---|---|---|
| 300+300 messages are accepted and no hidden count/byte rejection remains | `stateful Agent queue admission retains 300 steering and 300 follow-up messages` and `AgentSession queue admission retains 300 steering and 300 follow-up messages` check 300 retained per queue, default one-item peek, all-mode 300-item FIFO peeks, and the unchanged counts after peek. | `stateful Agent queue admission keeps a message larger than the retired byte bound` enqueues 16 MiB + 1 and + 2 byte messages and reads their exact lengths back. The previously exercised TUI case queues `x` after `é`, `é` and then a second batch after the old 4-byte/2-message limits; a retained cap fails those cases while a small-input test still passes. |
| One-at-a-time and all steering modes, follow-up, peek/clear and continue ordering match the capture | `one-at-a-time steering injects the oldest message after the tool turn` compares request roles `[system,user]`, `[system,user,assistant,toolResult,user]`, `[system,user,assistant,toolResult,user,assistant,user]` and last users `go`, `steer1`, `steer2`; `all-mode steering injects every queued message in one turn` compares the single two-message batch; `follow-up runs only after an otherwise stopping assistant turn`; `continue from an assistant tail drains steering before follow-up`; `queue peek prefers steering and clear removes both queues`. | The one-at-a-time and all-mode cases consume the same two steers through different drain policies; a single policy cannot satisfy both request-role sequences. The continuation case keeps both queues populated and proves steering is selected first; the peek case proves preview does not consume and the follow-up queue remains visible after steering is cleared. |
| The slice is verified through the highest public caller | `AgentSession queue admission retains 300 steering and 300 follow-up messages` drives the real Session admission path; `Native TUI queues unbounded steering input and dequeues pending text` and `Native TUI queues multiple follow-ups without capacity rejection` drive the real interactive frontend and observe both queues and delivered user messages. | The TUI cases previously produced `Unable to queue …: too many queued messages` / `queued messages too large`; the new assertions require the same input to be accepted, displayed and delivered in order. A capacity check that merely moved into the Session would fail these cases. |
| Full command validation and repository checks are recorded | Focused, architecture and full-suite counts below; formatting clean; consolidation and named-evidence checks pass. | The full-suite flake record below names the cases that passed in isolation, so a green focused run is not presented as a full-suite pass. |

## Review

The two review axes ran against the baseline `4790e9e3c` and found no blocking spec issue. The
standards findings were fixed rather than waived:

- **Checked narrowing.** The drain path no longer casts a `size_t` count to `ptrdiff_t`; it moves
  the selected range through iterator increments and erases with the resulting iterator
  (`CODING_STANDARDS.md` §9.14), and binds the configured queue once instead of repeating the
  queue-kind ternary.
- **Coroutine captures.** The two tool-triggered queue tests capture a `shared_ptr` holder by value
  instead of a local reference across the coroutine boundary (§6.2/§6.7).
- **Variant dispatch and indices.** The request-role helper dispatches with `std::visit` +
  `if constexpr` (§4.2), and the 300-message loops use `std::size_t` in-memory indices (§9.7).
- **Adjudicated spec findings.** Session-level admission/clear and the Native TUI one-at-a-time
  drain are exercised through the public frontend; the exhaustive all-mode/continuation orderings
  are Agent-level because that is the owning seam and `AgentSession` has no public continue
  operation. `AgentSession::steer`/`follow_up` retain `ExpectedVoid` for the closed-session
  rejection only; the audit requirement explicitly permits a non-capacity error shape and the
  frozen application `AgentSession.steer` is itself async/fallible. Removing the public capacity
  options, rather than keeping inert 256/16 MiB fields, is required to leave no capacity surface.

The review diff was taken against the named baseline `4790e9e3c`; `origin/main` advanced to a
newer #1171 merge during the work, so a review against current `origin/main` shows unrelated
durable changes that are not part of this branch.

## Validation

Focused Validation:

```bash
ctest --preset vcpkg -L issue1113 --output-on-failure
# 15 selected, 15 passed
```

The 15 cases are the twelve `[agent][issue1113]` cases (including the updated defaults test), the
Session admission case, and the two rewritten Native TUI queue cases.

Architecture selection:

```bash
ctest --preset vcpkg -L architecture -j2 --output-on-failure
# 43 selected, 43 passed
```

Final-tree selections after merging `origin/main` `f840fc445`:

```bash
TERM_PROGRAM=herdr ctest --preset vcpkg -L issue1113 --output-on-failure
# 15 selected, 15 passed
TERM_PROGRAM=herdr ctest --preset vcpkg -L architecture -j2 --output-on-failure
# 43 selected, 43 passed
TERM_PROGRAM=herdr ctest --preset vcpkg -L '^agent$' --output-on-failure
# 242 selected, 241 passed, 1 failed
```

The `^agent$` failure is the pre-existing timing-sensitive
`bounded parallel cancellation leaves queued tool capabilities unstarted` (`agent issue40
issue497 tool-executor`): it waits 30 ms for two concurrent tool invocations, so a loaded host can
make the second miss the window. Immediate isolated reruns on the merged tree were mixed (2/3
passed); the same case had passed 10/10 isolated reruns earlier and passed in one earlier full
run. It does not touch Agent queue admission.

The requested isolated #632 quarantine rerun on the merged tree remains the known cost-bound
failure, with all functional assertions passing:

```text
100-chunk projection publication loop: 2299000ns processor, 2348609ns wall
CHECK( processor < std::chrono::milliseconds{2} ) failed
173 assertions: 172 passed, 1 failed
```

Full Validation history on the pre-merge reviewed tree (the merge with `origin/main` is purely
additive for this slice):

- Run A: 4155 selected, 4146 passed, 1 failed, 8 skipped. The failure was
  `bounded parallel cancellation leaves queued tool capabilities unstarted`, which passed 10/10
  isolated reruns.
- Run B: 4155 selected, 4141 passed, 6 failed, 8 skipped (364.98 s), while the host was also
  building another worktree (load average 11–21 on 8 cores). The six failures were
  `generation publishes the attempt before the model stream and usage with the final answer`
  (passed alone), `a timed-out query resolves partial results and later replies advance state`
  (passed 3/3 alone), `pi-shaped exec returns shell_unavailable when bash is disabled` (passed
  alone), `Streaming assistant incremental block freeze maintains flat processing time across
  chunks` (#603 quarantine, passed alone), `Native TUI fullscreen scroll-to-end indicator paints
  on a direct-fullscreen launch while the transcript is scrolled` (passed alone), and the #632
  projection cost bound (the only repeated isolated failure). None touches Agent queue admission.

Other checks:

```bash
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1113/manifest.json   # accepted (record above)
python3 scripts/evidence/audit_evidence.py verify           # 47 bundles, 103 artifacts, 571 scenarios
python3 tests/evidence/NamedEvidenceTest.py                 # 24 tests, 22 passed, 2 skipped
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check  # PASS
scripts/format-check.sh origin/main                         # clean
```

The delivery inputs for #1104 and #1118 include the touched Session files. Their records now carry
`revalidation1113` and fresh JUnit artifacts
(`deliveries/1104-revalidation-1113.xml`, 36 selected;
`deliveries/1118-revalidation-1113.xml`, 19 selected), with the prior validation retained as
`previousValidation`. The delivered rows, scenarios and routing digests are unchanged; this makes
no new verification claim about those rows.

## Capability status

No capability row advances in this slice. `cch_agent_core:InputQueue:admission` remains
`partial` / `upstream-captured` in the source audit until the #1176 receipt reconciles the audit
notes and updates the whole-row status. The implementation obligation that caused the divergence
is removed; the comparison and discriminating cases above are the evidence #1176 consumes.

## Remaining boundary

- The source audit's `InputQueue:peek-clear` target list still names historical
  `peekSteering`/`peekFollowUp`/`clearAllQueues` spellings; the C++ surface is
  `peek_queued_messages` / `clear_steering_queue` / `clear_input_queues`. Reconciliation of that
  stale audit text belongs to #1176, not this implementation slice.
- `Agent::clear_*`, `set_steering_mode` and `set_follow_up_mode` retain an `ExpectedVoid` result for
  a moved-from Agent; `steer`/`followUp` are the frozen void enqueue operations. The Session keeps
  its closed-session error channel, which is not a capacity result.
- The Native TUI pending-input display still reads the raw queue contents from the Session
  projection, matching pi's `_steeringMessages` / `_followUpMessages` display lists; it does not
  substitute `peekQueuedMessages`. No queue-display behavior changed here.
- Full-suite runner sensitivity and the #632 projection quarantine remain open under their existing
  owners; this slice neither fixes nor suppresses them.
