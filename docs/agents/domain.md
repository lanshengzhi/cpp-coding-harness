# Domain Docs

This is a single-context repository: [GLOSSARY.md](../../GLOSSARY.md) defines its language, and [ADRs](../adr/) record decisions and their rationale.

## Consume the language

Before exploring a domain change, read the glossary and the contract references below for the affected topic. Use the glossary's canonical terms in issues, hypotheses, tests, and explanations; `_Avoid_` distinguishes the competing meanings.

The glossary defines what a concept is. Behavior, algorithms, wire fields, defaults, and validation procedures belong in the topic documents or accepted ADRs, not in glossary entries. A matching term or filename alone does not establish conformance to its contract.

## Find the contract

Read the row for the task, not the whole index. Follow an ADR's superseding records and amendments before treating its historical text as current policy.

| When working on | Read |
| --- | --- |
| Product scope, package ownership, Owner Interfaces, or the Configured Package Graph | [Architecture](architecture.md), including its ADR index; [Product Architecture Contract](pi-parity.md) for the gate and compatibility boundary |
| Platform or runtime-only release obligations | [ADR 0039](../adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md#supported-platform-and-toolchain), [ADR 0045](../adr/0045-name-the-released-runtime-pike-and-preserve-owner-package-names.md) |
| Runtime Root, Serialized Execution Domain, Session Event Commitment, Abort, or Close | [ADR 0040](../adr/0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md), [measured capacities](../runtime-capacities.md) |
| Provider and Agent Messages, Model identity, Provider assembly, or Models Runtime | [ADR 0005](../adr/0005-keep-provider-and-product-messages-in-their-owning-modules.md), [ADR 0029](../adr/0029-align-models-provider-and-authentication-ownership-with-pi.md), [ADR 0047](../adr/0047-own-provider-assembly-inside-cch-ai-and-hide-the-provider-capability.md), [ADR 0055](../adr/0055-deepen-models-runtime-owner-interface.md) |
| Provider protocols: Adapter, Transport, Compat Field, or Session Affinity | [ADR 0033](../adr/0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md), refined by [ADR 0059](../adr/0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md) and [ADR 0063](../adr/0063-source-wire-protocol-specialization-from-the-model-provider-identity.md); [pi-ai fixtures](../../fixtures/pi-ai/README.md) for current field-level evidence |
| Request Authentication, Credentials, OAuth Callback Server, login, refresh, or cancellation | [ADR 0031](../adr/0031-align-settings-shared-file-cli-and-resume-configuration-with-pi.md), [ADR 0032](../adr/0032-own-oauth-lifecycle-and-frontend-interaction-division-with-pi.md), and [ADR 0059 amendments](../adr/0059-align-the-cch-ai-capability-subset-with-pi-ai-and-diverge-on-kimi-code.md#amendment-and-766-close-out) for OpenRouter and Kimi |
| Prompt Run, Agent Run, Agent Turn, tools, stream outcomes, Compaction, or Auto-Retry | [Agent lifecycle](../agent-lifecycle.md), [ADR 0007](../adr/0007-make-json-schema-the-executable-tool-argument-contract.md), [ADR 0034](../adr/0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md); [ADR 0048](../adr/0048-compose-the-execution-environment-without-a-union-interface.md) for Execution Environment assembly |
| Session Assembly, Publication, history, or Resume | [ADR 0001](../adr/0001-centralize-session-assembly-policy.md), [ADR 0009](../adr/0009-treat-pi-v3-sessions-as-an-interoperable-wire-contract.md), [ADR 0031](../adr/0031-align-settings-shared-file-cli-and-resume-configuration-with-pi.md), [ADR 0060](../adr/0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md), [ADR 0064](../adr/0064-defer-the-session-file-to-the-first-user-or-assistant-message.md) |
| Agent Config Directory, Settings Scope, or one-time import | [Usage](../usage.md#agent-configuration), [ADR 0031](../adr/0031-align-settings-shared-file-cli-and-resume-configuration-with-pi.md), [ADR 0058](../adr/0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md) |
| System Prompt, Thinking Level, Project Trust, Skills, Prompt Templates, or Resource Reload | [ADR 0036](../adr/0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md#decisions-the-phase-surface), refined by [ADR 0060](../adr/0060-advance-the-cch-coding-agent-baseline-to-pi-v0-87-1-and-adopt-transcript-system-messages.md); [application fixtures](../../fixtures/pi-coding-agent/README.md) for resource parsing, invocation, reload, and prompt evidence |
| Interactive Session Run, Print Mode, User Bash, Slash Commands, selectors, or themes | [Usage](../usage.md), [interactive run boundary](architecture.md#interactive-run-boundary), [ADR 0036](../adr/0036-own-the-scoped-pi-coding-agent-application-layer-capabilities-for-the-three-provider-paths.md), [ADR 0043](../adr/0043-own-native-tui-slash-routing-and-host-effects.md) |
| Decoded Input Event, Resolved Keybinding Registry, or Interrupt Admission | [Keybindings](../keybindings.md), [ADR 0035](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md), [ADR 0027](../adr/0027-keep-prompt-cancellation-with-the-admission-owner.md), [ADR 0050](../adr/0050-adopt-outcome-based-input-admission.md) |
| Tool Renderer, terminal images, or Main-Screen Scrollback Flow | [ADR 0061](../adr/0061-align-tool-execution-rendering-with-pi-v0-87-1.md), [ADR 0035](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md), [ADR 0037](../adr/0037-own-the-pi-aligned-main-screen-scrollback-flow-for-the-tui-toolkit-renderer.md), [ADR 0041](../adr/0041-own-the-anchored-absolute-flow-model-for-the-tui-main-screen-renderer.md) |
| Headless Core, Projection Stream, Base/Patch convergence, Subscription Mailbox, frame pacing, Block Frozen Protocol, or Client-Side Prediction | [ADR 0051](../adr/0051-decouple-headless-core-and-projections-with-zero-mutex-sampling.md), including its preview-frame deviation; [ADR 0052](../adr/0052-replace-projection-sampling-with-one-patch-stream-and-bounded-mailboxes.md) replaces sampling with mailbox delivery |
| Focused, Full, or Fresh Validation | [Validation tiers](validation.md#validation-tiers), including the documentation-only exception |

## Maintain the boundary

Use `/domain-modeling` when resolving or changing terms or recording a decision. Keep glossary definitions project-specific, one or two sentences each, with `_Avoid_` alternatives; general engineering mechanisms and procedures stay at the contract references above. Add a term when it is resolved, rather than reserving speculative entries.

When moving detail out of the glossary, first locate its authoritative contract. Link that source rather than copying its policy; put genuinely unrecorded detail in the relevant topic document. Preserve the accepted behavior and identify superseded claims explicitly.

Offer an ADR only for a hard-to-reverse, surprising decision involving a real trade-off. A glossary cleanup or reference repair alone needs no ADR. If a proposal contradicts an accepted ADR, name the conflict and seek a decision rather than silently overriding it.
