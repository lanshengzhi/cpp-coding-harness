# Spec: Deepen project resource loading

Category: enhancement
Status: implemented

## Problem Statement

Project resource loading is now behaviorally useful but architecturally shallow.
Trust resolution, marker detection, load decisions, skill and prompt-template
loader wiring, host-provided resource precedence, and diagnostics are split
across session creation, runtime-service construction, standalone resource
helpers, and SDK-specific code paths.

That split makes every new project-authored resource risky. An implementation
agent adding project settings, system prompt files, packages, or extension
resources has to rediscover the trust rules, decide where diagnostics belong,
and duplicate precedence rules between CLI and SDK flows. This is exactly the
kind of cross-module policy that should be owned by one deep module before more
T6 resource work lands.

## Solution

Create a project resource-loading seam that owns the whole startup input-loading
decision: resource marker detection, trust resolution, resource enablement,
adapter selection, ordered resource loading, duplicate precedence, and
structured diagnostics. CLI, RPC, and SDK session creation should ask this seam
for a loaded resource bundle instead of assembling project skill and prompt
template paths themselves.

The seam should preserve current behavior for implemented resource kinds:
project-local skills and prompt templates remain trust-gated; explicit prompt
template paths remain controlled by the existing CLI flag; host-provided SDK
skills and templates continue to win over project-discovered duplicates; and
JSON/RPC stdout remains protocol-clean.

The design should be idiomatic C++ parity, not a direct TypeScript port of pi's
`DefaultResourceLoader`. The C++ module should expose passive value contracts
and a small loading function or object with adapter-based internals.

## User Stories

1. As a CLI user, I want trusted project skills to load as before, so that my existing workflows do not break.
2. As a CLI user, I want trusted project prompt templates to load as before, so that slash-template invocation still works.
3. As a CLI user, I want untrusted project resources skipped before their contents are parsed, so that repositories cannot silently alter startup behavior.
4. As a CLI user, I want `--approve` to allow supported project resources for one run, so that I can intentionally use a project's local agent resources.
5. As a CLI user, I want `--no-approve` to skip supported project resources for one run, so that I can inspect a project without loading its resources.
6. As a CLI user, I want `--no-skills` to keep disabling project-local skills and prompts, so that the existing coarse resource-off control remains predictable.
7. As a CLI user, I want `--no-prompt-templates` to keep disabling prompt template loading, so that explicit template loading can be suppressed for protocol runs.
8. As a JSON-mode user, I want resource diagnostics to stay off stdout, so that the output remains parseable JSONL.
9. As an RPC-mode user, I want startup diagnostics to be reported outside the command stream, so that clients do not have to filter human warnings from responses.
10. As an SDK host, I want host-provided skills to take precedence over project-discovered skills, so that host policy cannot be overridden by repository files.
11. As an SDK host, I want host-provided prompt templates to take precedence over project-discovered templates, so that host-defined commands stay stable.
12. As an SDK host, I want project resource loading to return diagnostics as values, so that I can display, log, or ignore them using my own UI.
13. As an SDK host, I want the SDK and CLI to use the same load-plan logic, so that security behavior does not diverge by entry point.
14. As an implementation agent, I want one resource-loading entry point, so that adding a future resource kind does not require editing every session creation path.
15. As an implementation agent, I want resource adapters for skills and prompt templates, so that parsing and validation remain in their existing loaders while policy stays centralized.
16. As an implementation agent, I want a single precedence rule for duplicate resources, so that collision behavior can be reasoned about and tested.
17. As an implementation agent, I want structured diagnostics for trust, load-plan, and adapter failures, so that CLI formatting and SDK diagnostics do not invent different taxonomies.
18. As an implementation agent, I want unsupported future markers to be visible but not loadable, so that project settings, extensions, packages, and system prompt files can be planned without accidental loading.
19. As a maintainer, I want tests at the public session creation seams, so that refactors preserve user-visible resource behavior.
20. As a maintainer, I want tests at the resource-loading seam, so that trust, policy, precedence, and diagnostics can be verified without running a provider.
21. As a maintainer, I want the README and routing docs to remain accurate if responsibilities move, so that future agents start in the right module.

## Implementation Decisions

- Build or deepen one project resource-loading module under the coding-agent runtime area. It owns startup resource policy and returns passive loaded-resource values.
- Keep resource-specific parsing in existing adapters. The skill loader still parses and validates skills. The prompt template loader still parses and validates templates. The new module decides whether and in what order those loaders run.
- Model supported resources as adapter registrations or equivalent small internal units. Each adapter declares its resource kind, marker path, policy gate, load operation, and diagnostic conversion.
- Keep project trust as an input-loading guard. The module must not parse protected project files before the load plan allows their resource kind.
- Preserve the existing no-UI behavior: `ask` without an applicable saved or explicit decision means untrusted.
- Preserve the current coarse resource policy for this slice. The existing project skills enablement continues to gate project skills and project prompt templates unless a later spec introduces per-resource toggles.
- Preserve explicit prompt template path behavior. Explicit paths are user-supplied startup inputs, not project marker discovery, and remain governed by the existing prompt-template disable flag.
- Preserve host-provided SDK precedence. Host skills and templates are loaded first; project-discovered duplicates are skipped with diagnostics.
- Centralize duplicate handling and diagnostics so CLI and SDK no longer carry separate collision logic.
- Normalize diagnostics into stable categories for trust resolution, resource load decisions, adapter warnings, and duplicate/collision decisions.
- `RuntimeServices` should receive already-loaded resources. It should not scan project directories or print resource diagnostics on its own after this refactor.
- Public domain contracts remain passive values. Do not introduce Glaze DTOs, filesystem implementation details, or serialization helpers into public resource contracts.
- Future resource markers such as project settings, packages, extensions, and project system prompt files remain unsupported in this spec. They should flow through the plan as unsupported decisions, not through ad hoc checks.
- Keep config and trust-store persistence behavior compatible unless an issue explicitly calls out a correction. If a divergence between CLI and SDK trust-store behavior is found, resolve it through the shared resource-loading input model rather than duplicating special cases.

## Testing Decisions

- Tests should verify behavior through public seams where practical. The highest-value seams are CLI session creation, public SDK session creation, and the new project resource-loading seam.
- Resource-loading seam tests should cover marker detection, trust required/not required, disabled resources, unsupported future markers, adapter diagnostics, duplicate precedence, and loaded resource contents.
- CLI tests should use the fake provider and temporary workspaces. They should prove text mode diagnostics go to stderr and JSON/RPC stdout remains protocol-clean.
- SDK tests should use host-provided fake clients and temporary workspaces. They should prove diagnostics are returned as values and host-provided resources win over project resources.
- Existing focused tests for skill loading, prompt template loading, project trust, project resources, prompt expansion, and skill expansion should continue to pass.
- Public-boundary or CMake dependency changes require the architecture test slice.
- No live provider or network validation is required.

## Out of Scope

- Implementing project settings parsing.
- Implementing project system prompt or append-system-prompt loading.
- Implementing extensions, extension execution, themes, package installation, or package discovery.
- Adding global `~/.cpp-harness/skills` or global prompt template loading.
- Adding interactive project trust prompts or persistent trust commands.
- Adding live resource reload.
- Adding per-resource config toggles beyond the current project skills and prompt-template controls.
- Changing the session JSONL format.
- Changing the built-in tool sandbox/security model.

## Further Notes

- This completed spec is the tracker authority for the project resource-loading slice.
- Relevant local route rows are project trust/resource controls, skills/resource loading, prompt processing, CLI/runtime, SDK, and public boundary.
- Current parity decisions belong in `.scratch/pi-cpp-parity/map.md`; completed resource-loading evidence belongs in this spec and its tickets.
- Relevant pi references inspected for parity direction include the resource loader, project trust flow, trust manager, settings/security docs, package docs, diagnostics, and source-info contracts.
