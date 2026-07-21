---
status: accepted
---

# Use one runtime terminal channel for provider calls

Once a model call has been accepted, request, model, transport, protocol, and cancellation failures terminate through the provider-neutral assistant event stream as an `error` or `aborted` final message, matching pi semantics. They must not also escape through a second `std::expected` runtime-error channel: construction failures, pre-call static validation failures, and programmer contract violations remain outside the accepted call lifecycle because combining them with runtime termination would force every C++ caller to reconcile two competing outcomes.

## Considered options

- Keep event errors and `std::expected` errors in parallel: rejected because explicit errors do not improve safety when they duplicate an already observable terminal state and permit contradictory outcomes.
- Put every failure into the event stream: rejected because no lifecycle exists when provider/session construction or pre-call validation fails.

## Consequences

- An accepted model call has exactly one terminal outcome and callers observe it through the same event/final-message contract as pi.
- Provider configuration should fail as early as practical, before a model call is accepted.
- Contract tests must cover preflight rejection separately from `error` and `aborted` runtime termination.
