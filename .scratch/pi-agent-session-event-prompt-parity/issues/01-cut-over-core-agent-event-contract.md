# 01 — Cut over the core AgentEvent value contract

**What to build:** Give agent-loop consumers one passive, pi-aligned lifecycle vocabulary for agent, turn, and message events. Remove the C++-specific prompt copy, numeric turn fields, queued-message alternatives, and standalone thinking/tool-call stream alternatives without adding compatibility aliases or dual event paths.

**Blocked by:** None — can start immediately.

**Status:** ready-for-human

- [x] Compare the implementation with PRD stories 9–12, 17, 19, and 43–44 and the current pi `AgentEvent` contract before changing it.
- [x] `agent_start`, `agent_end`, `turn_start`, `turn_end`, and message lifecycle values expose the supported pi field meanings as passive aggregates.
- [x] Legacy queued-message alternatives, raw-prompt fields, numeric turn fields, and standalone thinking/tool-call stream alternatives are absent.
- [x] Existing event consumers are migrated mechanically so the repository builds without compatibility shims; behavioral emission coverage remains scoped to tickets 02–04.
- [x] Move-only event sinks remain supported and the architecture test slice passes.
