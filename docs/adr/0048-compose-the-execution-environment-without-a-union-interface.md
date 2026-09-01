---
status: accepted
---

# Compose the Execution Environment without a union interface

The Execution Environment remains the complete filesystem-and-Shell capability bundle assembled for an Agent Session, but it is no longer represented by a polymorphic `AsyncExecutionEnv` interface that inherits both capability interfaces. Session Assembly supplies the existing complete `AsyncFileSystem` and `AsyncShell` seams directly to callers that need them; file Tools learn only the filesystem interface, the Bash Tool learns only the Shell interface, and private Runtime ownership retains only the filesystem cleanup obligation after Tool work quiesces. This replaces the physical composition shape selected by ADR 0006 because the standalone filesystem seam made the union interface a shallow pass-through with duplicated Local adapter knowledge, while preserving ADR 0006's completeness and assembly-time availability rules.

## Considered options

- Keep `AsyncExecutionEnv` and hide the duplicated filesystem forwarding internally: rejected because file Tools would still depend on an unrelated Shell interface and the shallow union interface would remain.
- Add a public `ExecutionEnvironment` aggregate containing both capabilities: rejected because no caller needs to pass the pair as one value and the private Runtime composition already owns assembly.
- Reuse the two existing seams directly: accepted because it removes the union interface without adding a module, keeps the interface as the test surface, and concentrates filesystem and Shell behavior in their respective Local adapters.

## Consequences

- `AsyncLocalFileSystem` directly owns the private `WorkspaceFileSystem` containment implementation; `AsyncLocalShell` owns Shell and process behavior; the combined and synchronous Local execution-environment pass-through modules are removed.
- Repository-internal callers migrate in one replacement with no compatibility overload or alias; the Pike Runtime is the only product and exposes no C++ SDK or ABI promise.
- Tests use filesystem and Shell fakes independently, retain concrete Linux containment tests, and delete evidence that only protected the removed inheritance shape.
