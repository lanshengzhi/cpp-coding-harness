# Product Architecture Contract

Read this when changing a module boundary, an Owner Package, a frontend seam, or a
rule enforced by the architecture gate. ADR 0053 replaced pi parity as this
repository's architecture authority: this product now makes its own decisions.

## Authority

- The machine-readable contract is `cmake/parity/manifest.json`.
- The validator is `cmake/parity/parity_gate.py`; the `parity` path and stable
  `PARITY-*` diagnostic IDs are implementation names retained to avoid churn.
  They do **not** mean that pi compatibility is required.
- Product decisions are recorded in accepted ADRs. Start with [ADR
  0053](../adr/0053-replace-pi-parity-authority-with-the-product-architecture-contract.md)
  and [ADR 0039](../adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md).
- Run the architecture selection with `ctest --preset vcpkg -L architecture`.
  The required gate checks the Product Architecture Contract only; it does not
  compare the repository with pi or consult a pi checkout.

## Current contract

The first contract rules establish these boundaries:

- Headless Session and Runtime sources cannot include frontend headers from the
  TUI, terminal, or CLI surfaces.
- Session-module sources under `src/agent/` cannot include `cch_ai` private
  headers; that module reaches the AI Owner only through the canonical
  `<cch/ai/...>` Owner Interface spelling (rule `agent-no-ai-private-includes`,
  diagnostic `PARITY-8003`).
- Owner Interface headers keep their canonical `<cch/...>` spelling, and
  cross-Owner include edges must follow the declared Owner graph through
  authoritative Owner targets; the gate enforces those checks rather than a
  blanket ban on private includes.
- Strict builds keep their no-exception and evidence requirements.

The contract is intentionally machine-readable and fail-closed. A rule change
must update the manifest, its validator, and the architecture tests in one
change. A temporary exception is acceptable only when it names the exact source
and rule, an accountable owner, a removal issue, an ISO expiry date, and the
reason. Exceptions are migration records, not a second policy source; remove
them when the migration lands.

## pi compatibility

Upstream pi is optional reference material, not a specification. A pi session or
configuration format is supported only at an explicit product compatibility
edge, such as `compat/pi`, and only when an issue or ADR says so. Compatibility
code is not part of the required architecture gate. The de-pi migration uses
one-time import rather than runtime fallback reads or dual-format loading.

When a user-visible behavior is intentionally retained because it is useful —
for example familiar TUI keybindings or slash commands — record it as a product
choice. Do not turn that retained behavior into an architecture dependency.

## Change workflow

1. Describe the boundary change and its non-goals in an issue or ADR.
2. Update the manifest contract and focused architecture tests.
3. Run the owning build and `ctest --preset vcpkg -L architecture`.
4. Remove any migration exception in the same change that eliminates its
   violation.

The Owner Package graph remains a repository implementation boundary, not a
promise that package shapes must mirror another project. See
`docs/agents/architecture.md` for capability, ownership, and security rules.
