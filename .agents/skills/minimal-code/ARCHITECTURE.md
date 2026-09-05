# Architecture existence test

An architecture change earns design work only after current evidence justifies its existence. Run the **existence test** before using `codebase-design` to shape a module.

For each possible change, stop at the first structural move that holds:

1. **Leave it.** No observed friction or named upcoming change means no architecture work.
2. **Remove or collapse it.** Delete shallow modules and fold pass-through behaviour into the place that already owns it.
3. **Reuse or deepen what exists.** Put the behaviour behind an existing module's interface when that concentrates knowledge and verification.
4. **Introduce a module.** Add a new module only when the earlier moves cannot concentrate the existing complexity.

Every module has a conceptual interface, even with one implementation. At whichever move holds, add a polymorphic seam only when two real adapters vary there. One adapter does not justify the seam; it does not invalidate the module's interface.

## Done when

A candidate earns a card only when it names:

- **Evidence:** the observed friction or named upcoming change.
- **Earliest move:** the first move above that holds.
- **Reduction:** the interface, module, duplication, or spread of knowledge that becomes smaller or disappears.
- **Seam evidence:** the two real adapters, when the move introduces a seam.

No evidence means no card. If no candidate passes, the result is **no actionable deepening opportunities**.
