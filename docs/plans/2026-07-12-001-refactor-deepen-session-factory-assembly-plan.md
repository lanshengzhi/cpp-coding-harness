---
title: "refactor: Deepen SessionFactory assembly"
type: refactor
status: completed
date: "2026-07-12"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
origin: "/tmp/architecture-review-20260712T114722Z.html"
adr: "docs/adr/0001-centralize-session-assembly-policy.md"
---

# Refactor: Deepen SessionFactory assembly

## Summary

Deepen the private `SessionFactory` module so CLI/RPC and SDK creation retain their intentional product differences while sharing one normalization, validation, and assembly implementation. Concentrate provider, resume, trust, resource, tool, capability-ownership, diagnostic, and runtime-construction policy behind the existing factory seam.

This refactor also fixes security and lifecycle divergences found by the architecture review: workspace-local SDK trust state, unconditional cleanup of host-owned execution environments, disabled bash being exposed to CLI models, inconsistent provider restoration, and failed creation leaving session artifacts.

## Problem

`SessionFactory` is a nominally shared entry point with two independent implementations:

- `create(AgentSessionCreationRequest)` resolves CLI configuration, opens the session, loads project resources, and delegates provider/env/tool creation to `make_runtime_services()`.
- `create(CreateAgentSessionOptions)` independently validates SDK targets, builds providers, opens sessions, rejects topology, creates environments and tools, merges resources, and constructs the runtime.

The duplication has produced observable policy drift:

- CLI uses the user-level trust store while SDK reads trust state from the workspace being evaluated.
- CLI and SDK use different provider/model restoration and override precedence.
- CLI always registers bash even when its execution capability is disabled; SDK omits it.
- Runtime close cleans up host-injected shared environments as if the session owned them.
- Some validation and assembly failures occur after the new JSONL session has already been created.
- `RuntimeServices` is an assembly helper for one path rather than a deep module shared by both.

## Goals

1. Provide one private assembly pipeline for CLI/RPC and SDK session creation.
2. Preserve intentional CLI and SDK contracts without duplicating their implementations.
3. Make security-sensitive policy local and testable through the factory seam.
4. Make failed creation free of visible persistent artifacts.
5. Preserve the public/private include boundary and SDK v1 deferrals.

## Non-goals

- In-memory sessions.
- Session switching, fork, import, or runtime replacement APIs.
- Concurrent prompts, abort, steering, follow-up, or queueing.
- Compaction support in SDK v1.
- Extensions, packages, OAuth, model catalogs, or new provider features.
- Public exposure of internal assembly phases or the resolved plan.
- Full mechanical parity with pi's TypeScript implementation.

## Accepted decisions

The decisions below are governed by [ADR 0001](../adr/0001-centralize-session-assembly-policy.md).

| Area | Decision |
| --- | --- |
| Factory ownership | CLI and SDK adapters translate source-specific syntax; `SessionFactory` owns shared defaults, precedence, validation, and security policy. |
| Pipeline | Both inputs run through private `normalize -> validate -> assemble` stages. |
| Internal plan | The resolved assembly plan is a private passive value and cannot be constructed or consumed by callers. |
| Session target | The resolved target is a closed `NewSessionTarget | ResumeSessionTarget` variant, not two optional paths. |
| Identity | The factory generates one opaque session ID and an independent UTC creation timestamp for every new session. Resume preserves stored identity. |
| Atomic visibility | Failed new-session assembly leaves no visible session file. Failed resume assembly does not modify the existing file. |
| Provider precedence | Explicit request values, then stored resume metadata, then user configuration, then provider defaults. Overrides of stored metadata produce diagnostics. |
| Host client metadata | A host client controls request execution. Explicit metadata is retained; resume otherwise retains stored metadata; only metadata-less new sessions use the host sentinel. |
| Trust authority | Default to the user-level trust store. SDK may supply a trust-store path outside the workspace. Workspace-local trust stores are rejected. |
| Resource provenance | Host/explicit resources are caller-authorized and take precedence. Only auto-discovered project resources are trust-gated. |
| Resource failure | Explicit resource load failure is fatal. Invalid auto-discovered resources are skipped with diagnostics. Trust-store failure denies project resources but need not prevent a resource-free session. |
| Tool availability | Register only enabled tools. Execution-environment permission checks remain defense in depth. |
| Capability ownership | Factory-created environments are session-owned and cleaned up. Host-injected environments are host-owned and never cleaned up by session close. Transferred clients and custom tools remain session-owned. |
| Diagnostics | Factory and runtime are silent. Recoverable issues return as diagnostics; fatal failures use `util::Expected`. Adapters choose presentation. |
| Resume capability | CLI keeps currently supported active-path resume behavior. SDK v1 remains linear-only, enforced by shared topology inspection. |
| Runtime services | `RuntimeServices` remains an internal passive bundle. Remove `RuntimeServicesConfig` and `make_runtime_services()` as an independent assembly interface. |
| Test surface | Test observable creation behavior through source-facing factory entry points. Do not expose or directly test private assembly stages. |

## Intended module shape

```text
CLI/RPC adapter                SDK adapter
      |                            |
      | source-specific input      |
      +-------------+--------------+
                    v
             SessionFactory
          normalize source input
                    |
                    v
       private resolved assembly plan
                    |
          validate shared invariants
                    |
                    v
          one assembly implementation
     provider / trust / resources / tools
       session commit / runtime creation
                    |
                    v
       runtime + metadata + diagnostics
```

The source-facing creation entry points may remain overloads. The depth comes from hiding the complete policy and phase ordering behind them, not from forcing CLI and SDK to expose identical option types.

## Ordering and failure invariants

The implementation must ensure the following observable ordering without making the stages public:

1. Validate source-level combinations and normalize a valid `SessionTarget`.
2. Inspect resume metadata and topology without modifying the session.
3. Resolve provider/model metadata and all source-specific defaults.
4. Validate tool/command names and explicit resources.
5. Resolve trust and load allowed resources.
6. Construct or adopt provider, execution environment, and tools.
7. Publish/open the persistent new session only after all prior fallible prerequisites succeed.
8. Construct `AgentSessionRuntime` and return metadata plus diagnostics.

If a failure occurs after factory-owned capabilities are created, clean them up without touching host-owned capabilities. Session persistence may use staging plus atomic publication or another mechanism, but the tests must prove the visibility invariants rather than implementation details.

## Public contract impact

The public SDK remains source-compatible except where an additive option is needed for an external trust-store path. Its rules are:

- `load_project_resources` stays opt-in.
- An omitted trust-store path uses the user-level default.
- A supplied trust-store path must be absolute. Resolve it and the workspace with `weakly_canonical()` so a not-yet-created trust file uses its longest existing parent; reject canonicalization errors, equality with the workspace, or a resolved descendant of the workspace. Existing symlinked files and symlinked parents must not bypass containment. Relative or indeterminate paths fail closed.
- Host-provided execution environments remain shared host capabilities.
- SDK v1 still rejects non-linear resume topology.

No private runtime type or resolved-plan type moves into `include/cch/`.

## Implementation units

### U1. Characterize current behavior and add failing regression tests

**Goal:** Establish the changed invariants before restructuring.

**Files:**
- `tests/coding_agent/SdkSessionTest.cpp`
- `tests/coding_agent/runtime/SessionLifecycleTest.cpp`
- `tests/coding_agent/ProviderConfigResolutionTest.cpp`
- `tests/cli/CliSmokeTest.cpp`

**Scenarios:**
- A duplicate custom tool, invalid explicit template, provider failure, or other precondition failure leaves no new session file.
- Failed resume does not change file contents.
- SDK rejects relative trust-store paths, paths equal to or inside the workspace, symlinked-parent escapes into the workspace, and paths whose containment cannot be determined; it accepts a not-yet-created file under a canonical external parent.
- CLI and SDK share provider/model precedence and report stored-metadata overrides.
- Closing a session does not call cleanup on a host-injected environment.
- Factory-owned local environments are cleaned up exactly once.
- Disabled CLI bash is absent from the model-visible registry.
- Factory/runtime diagnostics do not write stdout or stderr.

### U2. Introduce the private normalization model

**Goal:** Make invalid assembly states unrepresentable inside the factory.

**Files:**
- `src/coding_agent/runtime/SessionFactory.hpp`
- `src/coding_agent/runtime/SessionFactory.cpp`
- `src/coding_agent/runtime/AsyncCliRuntime.cpp`
- `src/coding_agent/Sdk.cpp`

**Approach:**
- Replace the pair of optional target paths inside the resolved flow with a new/resume variant.
- Move session ID and creation-time generation into the factory implementation.
- Normalize CLI and SDK inputs into one private move-only plan containing resolved intent, injected capabilities, resource provenance, ownership, and allowed resume capability.
- Keep normalization functions and plan types private to the implementation.
- Preserve source-facing option types so adapters remain small and do not inherit factory invariants.

### U3. Unify provider, trust, resource, and tool policy

**Goal:** Resolve every shared policy once.

**Files:**
- `src/coding_agent/runtime/SessionFactory.cpp`
- `src/coding_agent/ProviderConfigResolution.cpp`
- `src/coding_agent/ProjectResourceLoader.cpp`
- `include/cch/coding_agent/Sdk.hpp`
- `tests/coding_agent/ProviderConfigResolutionTest.cpp`
- `tests/coding_agent/ProjectResourceLoaderTest.cpp`
- `tests/coding_agent/SdkSessionTest.cpp`

**Approach:**
- Generalize provider resolution around explicit, stored, configured, and default inputs rather than CLI-only vocabulary.
- Preserve host-client capability injection while resolving metadata independently.
- Use the user-level trust store for both entry points. For an optional SDK path, require an absolute path, weakly canonicalize existing ancestors, compare normalized path components against the canonical workspace, and fail closed on errors or indeterminate containment.
- Update `Sdk.hpp` documentation and tests to state that a supplied `unique_ptr` chat client is transferred to the session; when it accompanies `provider_config`, provider/model remain metadata while connection/auth fields are ignored.
- Keep caller-authorized and auto-discovered resources distinct through loading and duplicate resolution.
- Convert explicit resource failures into creation errors while retaining bounded diagnostics for skipped discovered resources.
- Build one tool registry from the final enabled built-in set plus validated custom tools.

### U4. Consolidate runtime construction and ownership

**Goal:** Remove the second assembly interface and make lifecycle ownership explicit.

**Files:**
- `include/cch/coding_agent/Sdk.hpp`
- `src/coding_agent/runtime/RuntimeServices.hpp`
- `src/coding_agent/runtime/RuntimeServices.cpp`
- `src/coding_agent/runtime/SessionFactory.cpp`
- `src/coding_agent/runtime/AgentSessionRuntime.hpp`
- `src/coding_agent/runtime/AgentSessionRuntime.cpp`
- `CMakeLists.txt`

**Approach:**
- Keep `RuntimeServices` only as an internal passive bundle.
- Remove `RuntimeServicesConfig` and `make_runtime_services()` after both paths use the unified implementation.
- Record execution-environment ownership explicitly rather than inferring it from `shared_ptr` reference counts.
- Clean up only factory-owned environments.
- Update `Sdk.hpp` ownership documentation: `shared_ptr` execution environments remain host-owned, while chat clients and custom tools passed by `unique_ptr` transfer ownership to the session.
- Remove direct runtime/services construction from SDK and CLI adapters.

### U5. Defer persistent publication until assembly succeeds

**Goal:** Enforce artifact-free creation failure and read-only resume failure.

**Files:**
- `src/coding_agent/runtime/SessionFactory.cpp`
- `src/coding_agent/runtime/SessionLifecycle.hpp`
- `src/coding_agent/runtime/SessionLifecycle.cpp`
- `src/harness/session/JsonlSessionStore.cpp` only if staging support is required
- `tests/coding_agent/runtime/SessionLifecycleTest.cpp`
- `tests/coding_agent/SdkSessionTest.cpp`

**Approach:**
- Split read-only target preparation from writable session publication.
- Reuse `SessionResume`/`SessionTree` for topology and active-path meaning; do not duplicate traversal in the factory.
- Publish a new session only after provider, resources, tools, and capabilities are valid.
- Ensure every failure path removes unpublished factory-owned staging artifacts.
- Keep the original resume file byte-for-byte unchanged on failure.

### U6. Make diagnostics presentation adapter-owned

**Goal:** Keep the deep module protocol-neutral and embeddable.

**Files:**
- `src/coding_agent/runtime/AgentSessionRuntime.cpp`
- `src/coding_agent/runtime/AsyncCliRuntime.cpp`
- `src/coding_agent/Sdk.cpp`
- `tests/coding_agent/SdkSessionTest.cpp`
- `tests/cli/CliSmokeTest.cpp`

**Approach:**
- Remove creation/prompt diagnostic printing from runtime logic.
- Return skill/template and other recoverable diagnostics as values for every entry point.
- Let text-mode CLI render human diagnostics.
- Preserve JSON/RPC protocol cleanliness and SDK silence.

### U7. Protect seam ownership and update architecture documentation

**Goal:** Prevent assembly logic from leaking back into adapters.

**Files:**
- `tests/architecture/ArchitectureSurfaceScanTest.cpp`
- `tests/architecture/CMakeDependencyTest.cpp`
- `tests/architecture/PublicHeaderBoundaryTest.cpp`
- `docs/agents/module-routing.md`
- `README.md` only if user-visible behavior or SDK options change

**Approach:**
- Guard that `SessionFactory` is the only runtime location that constructs `AgentSessionRuntime` and assembles `RuntimeServices`.
- Do not freeze helper names, private stage types, or internal ordering through source-text tests.
- Preserve the no-`src` public include rule and passive public SDK contracts.
- Update route/docs to describe the single assembly seam and trust-store behavior after implementation passes.

## Test strategy

### Shared policy conformance

Run equivalent CLI and SDK scenarios for:

- provider/model create and resume precedence;
- trust-store authority and fail-closed behavior;
- resource provenance and duplicate precedence;
- enabled tool visibility;
- creation/resume persistence invariants;
- diagnostics-as-values and protocol silence.

### Intentional profile differences

Test separately:

- CLI project-resource defaults versus SDK opt-in discovery;
- CLI active-path resume versus SDK linear-only restriction;
- CLI text rendering versus SDK value return and JSON/RPC protocol behavior;
- SDK host capability injection and ownership.

### Replace, do not layer

- Test through the factory's source-facing creation interface and public SDK/CLI adapters.
- Remove tests whose only purpose is to freeze `make_runtime_services()` or private assembly sequencing.
- Keep focused tests for independent domain operations such as provider resolution, project resource loading, and session topology.

## Validation

Smallest-first validation during implementation:

```bash
cmake --build --preset system
./build/cpp_harness_tests "[sdk]"
./build/cpp_harness_tests "[cli]"
./build/cpp_harness_tests "[harness][session]"
./build/cpp_harness_tests "[architecture]"
```

Also run focused provider/resource tests selected by their existing Catch2 tags or test names. Escalate to `ctest --preset system` after the focused slices pass because the change crosses SDK, CLI/runtime, session persistence, tool, and public-boundary seams. Do not run live provider tests by default.

## Completion criteria

- Both source-facing creation paths call one private assembly implementation.
- No SDK or CLI adapter directly constructs runtime services or `AgentSessionRuntime`.
- All accepted decisions in this plan have observable regression coverage.
- Failed new-session creation leaves no visible session file; failed resume leaves its file unchanged.
- Workspace-local trust state cannot authorize project resources.
- Host-owned environments survive session close; factory-owned environments are cleaned up.
- Disabled tools are absent from provider-visible schemas.
- SDK remains silent and linear-resume-only.
- Architecture tests, focused SDK/CLI/session tests, and full `ctest` pass.
- Documentation describes actual behavior without claiming new pi parity features or sandbox guarantees.

## Completion evidence

Validated on 2026-07-15 against this plan, ADR 0001, PRD story 48, and the parity definition of done.

- Both CLI/RPC and SDK inputs normalize to the private `AssemblyPlan` and call the same `run_assembly()` implementation in `src/coding_agent/runtime/SessionFactory.cpp`; architecture coverage rejects direct `AgentSessionRuntime` construction outside the factory.
- New session identity and UTC creation time are generated independently, while resume preserves both values. `[sdk][assembly]` covers fresh identity and resume preservation.
- New-session publication remains after provider, capability, tool, trust, and resource validation. `[sdk][assembly]` proves failed creation leaves no session file and failed resume leaves its file byte-for-byte unchanged.
- Provider precedence and host-client metadata behavior are covered by `[config][resolution]` and SDK provider-resolution tests. Equivalent SDK and process-level CLI scenarios prove project-controlled default trust state fails closed; SDK coverage also protects relative, equal, descendant, symlinked-parent, indeterminate, and external not-yet-created paths.
- Validation found and fixed one closing blocker: when `HOME` resolved to the workspace, the default trust store could be project-controlled. The factory now treats that store as unavailable, fails project-resource authorization closed, and returns diagnostics instead of loading project-authored resources.
- Host/project resource precedence, fatal explicit-resource errors, discovered-resource diagnostics, enabled-tool visibility, SDK silence, and linear-only SDK resume are covered by the focused SDK, resource, CLI, and session slices.
- `RuntimeServices` remains a private passive bundle. Explicit `env_owned` state distinguishes factory-created from host-provided environments; host cleanup count and idempotent session close tests protect the lifecycle policy.
- Documentation continues to describe the centralized factory, external trust-store requirement, diagnostics-as-values behavior, and SDK deferrals without claiming sandboxing or unsupported pi features.

The authoritative command ledger is recorded in [issue 16](../../.scratch/pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md): every focused slice passed, and `ctest --preset system --output-on-failure` passed its complete CTest target. Live-provider and network tests were intentionally skipped because fake providers and local session files satisfy this plan and PRD's validation scope without credentials, quota, or network access.

## References

- Architecture review: `/tmp/architecture-review-20260712T114722Z.html`, “Deepen SessionFactory assembly”
- ADR: `docs/adr/0001-centralize-session-assembly-policy.md`
- Domain glossary: `CONTEXT.md`
- Roadmap: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`
- Contract inventory: `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- SDK v1 plan: `docs/plans/2026-06-21-001-feat-t8-embeddable-sdk-surface-plan.md`
- Trust/resource plan: `docs/plans/2026-06-20-008-feat-project-trust-resource-controls-plan.md`
- pi SDK assembly: `pi:packages/coding-agent/src/core/sdk.ts`
- pi runtime services: `pi:packages/coding-agent/src/core/agent-session-services.ts`
- pi runtime replacement wrapper: `pi:packages/coding-agent/src/core/agent-session-runtime.ts`
