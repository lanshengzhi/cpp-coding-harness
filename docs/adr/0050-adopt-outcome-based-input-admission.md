---
status: accepted
---

# Adopt outcome-based Decoded Input Event admission

The shared `cch_tui::InputHandler` interface replaces its `void handle_input` method and `accepts_key_releases` preflight query with one explicit two-state admission outcome: `InputAdmissionOutcome::Unhandled` or `InputAdmissionOutcome::Consumed`. Every component, wrapper, forwarding layer, compositor dispatch, and test double across `cch_tui` and the Native TUI uses this contract.

This decision supersedes only ADR 0035's named `accepts_key_releases` preflight method shape. TUI Toolkit ownership of terminal input, Decoded Input Event semantics, and the observable press, repeat, and release behavior remain authoritative.

## Considered options

- Keep `accepts_key_releases` beside an outcome enum: rejected because parallel admission conventions leave component-private capabilities leaked into callers and allow event precedence to drift.
- Return a compound outcome containing repaint or state-change flags: rejected because input admission and asynchronous presentation scheduling are orthogonal concepts.
- Retain the compositor's boolean dispatch vocabulary: rejected because first-consumed-wins admission applies uniformly to components, overlays, and selectors.

## Consequences

- The preflight `accepts_key_releases` method and its implementations are removed across all components.
- Key-release events travel through normal admission routing. Components that do not claim a release return `Unhandled` without applying press behavior, allowing lower eligible consumers to receive the event.
- Recognized actions consumed at a boundary (such as navigation at an edge or undo on an empty stack) return `Consumed` without leaking to lower precedence layers.
- An `Unhandled` result guarantees that no action was emitted, no callback was invoked, and no visible state change occurred before forwarding.
- Overlay composition follows first-consumed-wins and eliminates the separate boolean dispatch vocabulary.
- The clean cutover removes compatibility overloads, boolean aliases, and dual-protocol paths in one pass.
