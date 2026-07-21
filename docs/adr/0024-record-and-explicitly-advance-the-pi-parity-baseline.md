---
status: accepted
---

# Record and explicitly advance the pi parity baseline

Semantic Parity is evaluated against a recorded pi commit and package version rather than an unpinned upstream HEAD. Advancing that baseline is an explicit audit that classifies upstream additions, changed semantics, local drift, and still-deferred capabilities; implementation specs may inspect newer pi source, but cannot silently redefine the repository's current compatibility claim.

## Considered options

- Always claim compatibility with latest pi: rejected because upstream movement could invalidate the claim without any local code change or review.
- Pin one pi version permanently: rejected because the project is intended to learn from and selectively follow current pi semantics.
- Record a baseline and advance it deliberately: accepted because claims and tests stay reproducible while evolution remains possible.

## Consequences

- The parity map records the current pi commit and relevant package versions.
- Specs, audits, and cross-project fixtures cite their baseline.
- A baseline update receives its own review and does not automatically approve newly added pi capabilities.
- README claims refer to the recorded baseline rather than “latest” pi.
- This audit establishes pi commit `864b35c` and `@earendil-works/pi-agent-core` version `0.80.10` as the initial recorded baseline.
