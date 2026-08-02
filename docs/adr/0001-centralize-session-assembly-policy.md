---
status: accepted
---

# Centralize session assembly policy in SessionFactory

CLI and SDK session creation must keep their distinct product contracts while sharing one private `SessionFactory` pipeline for normalization, validation, and assembly. `SessionFactory` owns provider and resume precedence, session identity, trust, resources, tools, capability ownership, diagnostics, and runtime construction because duplicating these security-sensitive decisions across adapters has already produced divergent behavior.

> The provider/model precedence and host chat-client consequences below are superseded by [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md): provider execution is injected as a shared `ModelRuntime`, and model/auth ownership follows pi's two-layer shape. Session assembly centralization itself still stands.

## Considered options

- Keep separate CLI and SDK assembly implementations: rejected because defaults, trust-store location, provider restoration, tool availability, and cleanup behavior can drift independently.
- Expose a resolved assembly plan to callers: rejected because callers could bypass normalization or depend on internal ordering constraints, making the module shallow.
- Preserve `RuntimeServices` as a second assembly seam: rejected because it has one implementation, is bypassed by SDK creation, and duplicates the factory's interface without adding leverage.

## Consequences

- CLI and SDK inputs normalize to a private resolved plan and then use one assembly implementation; intentional differences remain explicit policy constraints rather than separate implementations.
- Failed new-session assembly leaves no visible session file, and failed resume assembly does not modify the stored session.
- Trust decisions come from a user-level store or an SDK-supplied path outside the workspace; a project cannot declare itself trusted through a workspace-local store.
- ~~Provider/model precedence is explicit request, stored resume metadata, user configuration, then provider defaults. A transferred host client controls execution while explicit or stored provider/model values remain session metadata.~~ Superseded by [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md): host chat-client injection is removed in favor of a shared injected `ModelRuntime`, and request-time provider/auth precedence follows pi (explicit API key, stored credential, ambient only when nothing is stored).
- Factory-created execution environments are session-owned, while host-injected environments remain host-owned and are not cleaned up when one session closes. Custom tools transferred by `unique_ptr` are session-owned. (The chat-client half of this rule is superseded by [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), which replaces transferred client ownership with shared `ModelRuntime` injection.)
- Only enabled built-in tools are registered; execution-environment checks remain defense in depth.
- Explicit resource failures abort creation, while invalid auto-discovered resources are skipped with diagnostics.
- Public project-resource contracts are passive policy, option, result, and diagnostic values. Filesystem probing, load-plan construction, and resource assembly remain private factory/loader responsibilities rather than a second public assembly seam.
- The factory and runtime return diagnostics as values and never decide how an adapter presents them.
- `RuntimeServices` is a private passive bundle rather than a second assembly seam.
- SDK v1 remains linear-resume-only, and this decision does not add deferred pi runtime features.
