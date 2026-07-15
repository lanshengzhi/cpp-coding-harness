# 11 — Render text CLI output from AgentSession subscriptions

**What to build:** Make text mode present agent progress and responses from the same persistent AgentSession event stream used by SDK hosts, with frontend command outcomes handled separately.

**Blocked by:** 04 — Align tool execution events and tool-result messages; 05 — Move text CLI built-ins to the CLI adapter; 08 — Persist completed messages incrementally.

**Status:** implemented

- [x] Use PRD stories 34–36 and 40 as the text adapter authority.
- [x] Text mode subscribes once and renders supported AgentSession events without a per-prompt event sink.
- [x] Assistant text and tool activity are not duplicated by separate prompt-result presentation.
- [x] Frontend command feedback remains text-mode behavior, while unmatched slash input reaches AgentSession.
- [x] Prompt failures are presented without reviving PromptResult status codes as a second lifecycle model.
- [x] Process-level text CLI tests and the focused `[cli]` slice pass with fake providers only.

## Validation

- `cmake --build --preset system -j2`
- `./build/cpp_harness_tests '[cli]'` — 68 tests passed.
- `./build/cpp_harness_tests '[architecture]'` — 19 tests passed.
- Fake-provider one-shot smoke output contains one subscribed assistant line and no duplicate final response.
