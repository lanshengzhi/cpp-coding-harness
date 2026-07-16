# Spec: Centralize agent-session assembly policy

Category: enhancement
Status: implemented

## Problem Statement

CLI, RPC, and SDK session creation had separate assembly paths for provider selection, resume behavior, project trust, resources, tools, capability ownership, diagnostics, and persistence. The duplicated security-sensitive policy allowed the product surfaces to drift and made failed creation capable of leaving partial state.

## Solution

Make one private session factory own normalization, validation, and assembly while preserving intentional differences between CLI and SDK contracts. Source adapters translate their inputs; the factory owns shared precedence, authorization, ownership, failure, and publication policy and returns diagnostics as values.

## User Stories

1. As a CLI user, I want session creation and resume to apply one consistent provider and trust policy, so that startup behavior is predictable.
2. As an SDK host, I want injected clients, tools, and execution environments to have explicit ownership, so that closing one session does not destroy host-owned capabilities.
3. As a user, I want a failed new-session assembly to leave no visible session artifact, so that failed attempts do not look resumable.
4. As a user resuming a session, I want failed assembly to leave the durable session unchanged, so that recovery is safe.
5. As a security-conscious user, I want project trust to come from authority outside the project being evaluated, so that a project cannot approve itself.
6. As an adapter author, I want resource and provider diagnostics returned as values, so that each product surface controls presentation without duplicating policy.
7. As a model caller, I want only enabled tools exposed, so that the advertised tool surface matches actual capability.
8. As a maintainer, I want one high-level assembly seam, so that policy changes cannot silently diverge between CLI and SDK paths.

## Implementation Decisions

- Both source profiles use one private normalization, validation, and assembly pipeline.
- New-session and resume intent form a closed choice; private assembly stages are not public contracts.
- Explicit provider metadata takes precedence over stored resume metadata, then user configuration, then provider defaults.
- Project trust defaults to user-controlled authority outside the workspace; project-controlled trust state fails closed.
- Host-provided resources are already authorized and take precedence over auto-discovered project resources.
- Explicit resource failures are fatal; invalid auto-discovered resources are skipped with diagnostics.
- Only enabled tools are registered.
- Factory-created capabilities are session-owned; host-injected execution environments remain host-owned.
- New sessions become visible only after successful assembly; failed resume assembly does not modify durable state.
- Factory and runtime return diagnostics as values; adapters own presentation.
- SDK v1 continues to accept only the resume topologies it can represent safely.

## Testing Decisions

- Test behavior through the CLI and SDK creation seams rather than exposing private assembly stages.
- Cover provider precedence, trust containment, resource provenance, tool visibility, capability ownership, atomic visibility, diagnostics, and resume topology.
- Run focused SDK, CLI, session, provider/resource, and architecture slices, followed by the complete system preset.
- Use fake providers and local sessions; live-provider validation is not required.

## Out of Scope

- In-memory sessions, concurrent prompts, abort or queue APIs, runtime replacement, extensions, OAuth, model catalogs, compaction support in SDK v1, and public exposure of assembly stages.

## Further Notes

Implementation and full validation were completed before this spec was normalized into the local Matt tracker format. See the [validation ledger](../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md) and the durable [session-assembly decision](../../docs/adr/0001-centralize-session-assembly-policy.md).
