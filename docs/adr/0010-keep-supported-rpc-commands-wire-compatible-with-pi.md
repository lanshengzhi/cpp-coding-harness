---
status: accepted
---

# Keep supported RPC commands wire-compatible with pi

The runtime may implement only a documented subset of pi RPC, but every supported pi command preserves pi's request, response, event, acknowledgement, and failure wire semantics. A same-named command cannot return a C++-specific schema; local commands remain explicit extensions whose names and documentation do not imply pi compatibility.

## Considered options

- Treat RPC as an independently designed C++ protocol: rejected because current command names and event vocabulary already claim pi compatibility.
- Require the entire pi RPC surface before exposing any command: rejected because a documented subset is independently useful and deferred commands need no placeholders.
- Keep a compatible subset plus explicit extensions: accepted because it separates missing capability from contract drift.

## Consequences

- `get_state` uses pi's field shapes and requiredness, including its model representation.
- Unsupported pi commands return the documented unsupported-command response and expose no inert API.
- C++-specific commands such as shutdown are identified as extensions and avoid likely pi namespace collisions.
- Shared JSON fixtures verify both request/response shapes and prompt acknowledgement timing.
