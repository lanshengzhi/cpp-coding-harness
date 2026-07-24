---
status: accepted
---

# Own a modular native TUI

A Native TUI belongs in this product's long-term boundary rather than being left permanently to JSON/RPC or SDK clients. It will follow pi's modular shape: a public, reusable source-level C++ TUI module that does not depend on coding-agent types, plus a separate interactive-mode module that assembles TUI and Agent Session capabilities; line-oriented CLI, JSON/RPC, and SDK paths remain independent of the TUI.

## Considered options

- Keep the product boundary at the line-oriented CLI, JSON/RPC, and SDK: rejected because the intended product is an idiomatic C++ counterpart to pi's complete interactive terminal experience, not only an integration backend.
- Embed terminal behavior directly in the coding-agent runtime: rejected because it would couple reusable rendering and input behavior to agent/session policy and force non-interactive consumers to carry TUI dependencies.
- Own a reusable TUI module and assemble it in a separate interactive frontend: accepted because it preserves pi's capability boundary while allowing idiomatic C++ contracts and independent reuse.

## Consequences

- The Native TUI, themes, keybindings, and interactive components remain Deferred Capabilities until separately specified, implemented, and tested; no placeholder APIs, flags, menus, or actions are exposed in the meantime.
- TUI Semantic Parity covers user-observable interaction outcomes, state transitions, lifecycle, and explicitly supported configuration formats at the recorded pi baseline. It does not require byte-identical ANSI output, pixel-identical layout, or TypeScript API shape.
- Theme and keybinding files use compatible baseline formats, but discovery remains rooted in this product's Agent Config Directory and trusted `.cpp-harness` resources rather than implicitly reading pi's state directories.
- Supported TUI platforms may be promoted in stages, beginning with Linux and macOS; unsupported platforms retain the non-TUI surfaces without acquiring a TUI parity obligation.
- Once the TUI meets its Supported Capability bar, it becomes the default frontend only for supported interactive terminals. Explicit line-oriented, print, JSON, and RPC modes remain available and non-TTY execution never gains implicit ANSI output.
- The public C++ TUI contract does not promise ABI stability and does not expose a chosen third-party terminal library. Terminal resources and restoration follow RAII, and a replaceable terminal seam supports deterministic virtual-terminal tests.
- The interactive frontend exposes only capabilities actually assembled as Supported Capabilities. Deferred session, model, extension-UI, authentication, or other features do not appear as inert TUI affordances.
