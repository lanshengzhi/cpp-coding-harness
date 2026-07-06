# Architecture Review Follow-up TODO

Source report: `/tmp/architecture-review-20260705-175740.html`

Rule of thumb: one architecture candidate becomes one PRD, then one issue set.
Do not combine multiple candidates into a single PRD. After `to-issues`, start a
fresh session for each implementation issue and run `implement` with the parent
PRD plus exactly one issue.

## Done

- [x] `session-resume`: Make `SessionTree` the resume module.
  - PRD: `.scratch/session-resume-module/PRD.md`
  - Issues: `.scratch/session-resume-module/issues/`
  - Status: implemented

## Next Candidates

### 1. `session-wire`

- [x] Create PRD for "Stop leaking session wire payloads".
  - Suggested path: `.scratch/session-wire-payloads/PRD.md`
  - Flow: `to-prd`
  - Scope: keep raw JSON/wire field names inside serialization; expose typed
    session entry values to `SessionTree` and tests.
- [x] Split the PRD into implementation issues.
  - Suggested path: `.scratch/session-wire-payloads/issues/`
  - Flow: `to-issues`
- [ ] Implement each issue in a fresh session.
  - Flow: `implement`
  - [x] Issue 01: introduce typed session entry values.
  - [ ] Issue 02: move `SessionTree` context reconstruction to typed values.
  - [ ] Issue 03: move leaf resume and append continuation off wire payloads.
  - [ ] Issue 04: harden tests around the serializer boundary.

Suggested PRD prompt:

```text
$mattpocock-skills:to-prd

Based on /tmp/architecture-review-20260705-175740.html, create a PRD only for
the #session-wire candidate, "Stop leaking session wire payloads".
Do not include any other architecture candidate.
Output to .scratch/session-wire-payloads/PRD.md.
```

### 2. `resource-loading`

- [ ] Create PRD for "Deepen project resource loading".
  - Suggested path: `.scratch/project-resource-loading/PRD.md`
  - Flow: `to-prd`
  - Scope: make project resource loading own trust, load decisions, resource
    adapters, precedence, and diagnostics.
- [ ] Split the PRD into implementation issues.
  - Suggested path: `.scratch/project-resource-loading/issues/`
  - Flow: `to-issues`
- [ ] Implement each issue in a fresh session.
  - Flow: `implement`

### 3. `execution-seam`

- [ ] Sharpen the candidate before PRD.
  - Flow: `grill-with-docs`
  - Reason: report marks this as "Worth exploring", not an immediate build.
- [ ] If accepted, create PRD for "Retire the legacy execution surface".
  - Suggested path: `.scratch/execution-capability-seam/PRD.md`
  - Flow: `to-prd`
- [ ] Split into implementation issues.
  - Suggested path: `.scratch/execution-capability-seam/issues/`
  - Flow: `to-issues`
- [ ] Implement each issue in a fresh session.
  - Flow: `implement`

### 4. `prompt-processing`

- [ ] Re-check current prompt-processing implementation and old plans.
  - Flow: `grill-with-docs`
  - Reason: report asks for a deeper module, while an earlier prompt-processing
    feature already exists.
- [ ] If accepted, create PRD for "Deepen prompt input processing".
  - Suggested path: `.scratch/prompt-input-module/PRD.md`
  - Flow: `to-prd`
- [ ] Split into implementation issues.
  - Suggested path: `.scratch/prompt-input-module/issues/`
  - Flow: `to-issues`
- [ ] Implement each issue in a fresh session.
  - Flow: `implement`

### 5. `runtime-messages`

- [ ] Do design clarification before PRD.
  - Flow: `grill-with-docs`
  - Reason: report marks this as speculative and it touches public AI contracts.
- [ ] If accepted, create PRD for "Move runtime messages out of AI contracts".
  - Suggested path: `.scratch/runtime-message-boundary/PRD.md`
  - Flow: `to-prd`
- [ ] Split into implementation issues.
  - Suggested path: `.scratch/runtime-message-boundary/issues/`
  - Flow: `to-issues`
- [ ] Implement each issue in a fresh session.
  - Flow: `implement`

## Reusable Issue Prompt

```text
$mattpocock-skills:to-issues

Split .scratch/<slug>/PRD.md into independently implementable issues.
Output to .scratch/<slug>/issues/.
Each issue should be suitable for a fresh session and should include validation.
```

## Reusable Implementation Prompt

```text
$mattpocock-skills:implement

Implement .scratch/<slug>/issues/<NN>-<issue>.md.
Parent PRD: .scratch/<slug>/PRD.md.
Work only on this issue.
```
