---
status: accepted
---

# Move pi-neutral mechanics from cch_ai to cch_support

`AsyncResultBridge.hpp` (the Boost.Asio ↔ `AsyncResult` completion bridge), `BoostExceptionHandler.cpp` (the application-owned `boost::throw_exception` termination hook the bridge's consumers must link), `BoundedText.hpp` (bounded/redacted diagnostic text), and `Redactor.hpp` (secret redaction) are pi-neutral C++ mechanics, but they lived inside the `cch_ai` Owner Package while `cch_agent_core`, `cch_coding_agent`, and the CLI consumed them through 30+ quoted private includes. That made cch_ai's private area a second de-facto seam beyond its Owner Interface and contradicted the spirit of [ADR 0042](0042-adopt-a-strict-no-exception-core-with-private-async-bridges.md)'s "bridge machinery does not cross an Owner boundary" clause. [ADR 0039](0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) limits `cch_support` to pi-neutral mechanics "genuinely required across Owners" — with an explicit escape hatch: redaction and output limiting remain with their Capability Owner *unless multiple Owners genuinely require the same pi-neutral mechanic*. That is exactly this case. This decision records issue [#539](https://github.com/lanshengzhi/cpp-coding-harness/issues/539), found by the cch_ai architecture review.

The three headers move to `src/support/` as private quoted-spelling headers (`"support/AsyncResultBridge.hpp"`), and the hook moves to `src/support/BoostExceptionHandler.cpp` with its no-exception compile properties, following the existing `support/JsonGlaze.hpp` precedent; the headers are deliberately not promoted to the cch_support interface root, so Boost.Asio never enters any interface vocabulary. Namespaces move with ownership (CODING_STANDARDS §3.7): `cch::ai` → `cch::support`, `cch::ai::detail` → `cch::support::detail`, with all call sites re-qualified. The manifest's exception-ptr allowlist path moves with the bridge; `src/ai/ModelStreamBridge.hpp` stays in cch_ai (it is ai-domain machinery bridging `ModelStream`) and stays allowlisted.

## Considered options

- Leave the headers in cch_ai: rejected because every edit to pi-neutral mechanics rebuilds and re-tests an unrelated capability Owner, and explorers misread cch_ai's private area as fair-game cross-Owner surface.
- Promote the three headers to the cch_support interface root (`<cch/support/...>`): rejected because the interface root is visible graph-wide and `AsyncResultBridge.hpp` carries ten Boost.Asio headers; ADR 0042 keeps Asio out of every interface. The private quoted spelling is the established pattern for support mechanics with third-party weight.
- Also move `ModelStreamBridge.hpp` and `glaze/AiJson.hpp` reach-throughs in the same change: rejected as out of scope; the ProviderComposer composition root is ADR-0029-sanctioned and the session EntrySerializer's reuse of ai message DTOs is domain-coupled. Both are recorded for separate adjudication in [#540](https://github.com/lanshengzhi/cpp-coding-harness/issues/540).

## Consequences

- cch_ai's surface shrinks to its actual capability; the Owner Interface is again the only seam.
- Support private headers remain outside every Owner contract, so replacing Boost.Asio later still cannot change an Owner Interface (ADR 0042's intent preserved under the support role).
- The `allowed_exception_ptr_sources` allowlist in `cmake/parity/manifest.json` names `src/support/AsyncResultBridge.hpp` and `src/ai/ModelStreamBridge.hpp`; the fixture mirror in `tests/architecture/parity_gate_test.py` matches.
- The three test files move to `tests/support/` per CODING_STANDARDS §11.5; the ai-domain ModelStream-bridge assertion was split into `tests/ai/ModelStreamBridgeTest.cpp` rather than moved.
