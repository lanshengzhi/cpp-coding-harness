---
status: accepted
---

# Own a modular native TUI

A Native TUI belongs in this product's boundary rather than being left to JSON/RPC or SDK clients. It follows pi's modular shape: a public, reusable source-level C++ TUI module that does not depend on coding-agent types, plus a separate interactive-mode module that assembles TUI and Agent Session capabilities; print, JSON/RPC, and SDK paths remain independent of the TUI. The earlier requirement to preserve an explicit line-oriented frontend was superseded by the Native TUI promotion decision in #34 and #64.

> Refined by [ADR 0035](0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md): the toolkit's Supported renderer is pi's default regular main-screen mode at parity baseline `83114817`; the opt-in fullscreen alt-screen/viewport half (layout engine, scroll/mouse/selection, OSC 133 navigation) is Deferred with no placeholder surface. The toolkit keeps the decoded input-event model and terminal-owned image placement as recorded Intentional Divergences under the Semantic Parity definition below, and the reusable module surface is re-pinned from `864b35c` to `83114817` (app-layer pins advance with the pi-coding-agent phase audit).

> Platform, release, and frontend clauses superseded by [ADR 0036](0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md) and [ADR 0039](0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md): `cch_tui` is a repository-internal Capability Owner Package, not an installed public SDK; JSON/RPC/SDK paths do not remain; and Linux/macOS plus best-effort non-TUI platform support is replaced by native Linux x86-64 with glibc only. The modular split between reusable TUI Toolkit behavior and coding-agent application composition, the Semantic Parity definition, terminal RAII, and deterministic terminal seam remain authoritative.

## Considered options

- Keep the product boundary at the line-oriented CLI, JSON/RPC, and SDK: rejected because the intended product is an idiomatic C++ counterpart to pi's complete interactive terminal experience, not only an integration backend.
- Embed terminal behavior directly in the coding-agent runtime: rejected because it would couple reusable rendering and input behavior to agent/session policy and force non-interactive consumers to carry TUI dependencies.
- Own a reusable TUI module and assemble it in a separate interactive frontend: accepted because it preserves pi's capability boundary while allowing idiomatic C++ contracts and independent reuse.

## Consequences

- The Native TUI, themes, keybindings, and assembled interactive components are Supported Capabilities on Linux/macOS at the recorded baseline. Capabilities not implemented end to end remain absent rather than appearing as placeholder APIs, flags, menus, or actions.
- TUI Semantic Parity covers user-observable interaction outcomes, state transitions, lifecycle, and explicitly supported configuration formats at the recorded pi baseline. It does not require byte-identical ANSI output, pixel-identical layout, or TypeScript API shape.
- Theme and keybinding files use compatible baseline formats, but discovery remains rooted in this product's Agent Config Directory and trusted `.cpp-harness` resources rather than implicitly reading pi's state directories.
- Supported TUI platforms may be promoted in stages, beginning with Linux and macOS; unsupported platforms retain the non-TUI surfaces without acquiring a TUI parity obligation.
- On supported Linux/macOS, the Native TUI is the default for interactive stdin/stdout. `--mode rpc` and `--mode json` take precedence; otherwise `--print` or either non-TTY stream selects one-shot text output, while `--mode text` leaves selection unchanged. The line-oriented frontend and `--repl` are not retained, and non-TTY execution never gains implicit ANSI output.
- The public C++ TUI contract does not promise ABI stability and does not expose a chosen third-party terminal library. Terminal resources and restoration follow RAII, and a replaceable terminal seam supports deterministic virtual-terminal tests.
- The interactive frontend exposes only capabilities actually assembled as Supported Capabilities. Deferred session, model, extension-UI, authentication, or other features do not appear as inert TUI affordances.
