---
status: accepted
---

# Isolate tool-call failures

Argument, lookup, before-hook, tool-execution, and after-hook failures finalize as error Tool Call Outcomes for their own calls rather than aborting the agent run. This matches pi's failure boundary: a before-hook failure prevents that tool from executing, while an after hook runs for every executed outcome, including tool errors, and may refine the final result.

## Considered options

- Propagate every hook `std::expected` error as an agent failure: rejected because a C++ error representation does not justify widening a per-call failure into a session-wide terminal state.
- Abort the whole batch on policy-hook failure: rejected because failing closed for the affected call is sufficient, while unrelated calls retain deterministic outcomes.
- Isolate failures per call and reserve run failure for agent-global infrastructure: accepted because it gives callers one stable containment boundary.

## Consequences

- A failed before hook never invokes its tool but produces that call's error result.
- After hooks run for success and error outcomes and can replace the final content/error/termination fields.
- Batch termination is based on the finalized explicit termination decisions, not an implicit ban on error results.
- Agent-global context conversion, event commitment, and invariant failures may still terminate the run.
- Sequential and bounded-parallel execution preserve the same per-call failure semantics and deterministic transcript ordering.
