---
status: accepted
---

# Deepen the Models Runtime Owner Interface with a private Models lifetime and stream factory

The Models Runtime owns the coding-agent composition of provider definitions, availability, and
Agent Config Directory state, while `cch::ai::Models` owns the live Provider graph, Request
Authentication, and model-stream dispatch. Before this decision, the Runtime's `ai_models()`
accessor returned a shared `ai::Models` handle. An Agent or host holding that handle could mutate
Provider composition or reach trusted authentication operations, defeating the Owner boundary and
leaving a secret-bearing seam in front of the future Web/GUI/Spectator projections.

This decision implements issue [#641](https://github.com/lanshengzhi/cpp-coding-harness/issues/641)
as part of [#640](https://github.com/lanshengzhi/cpp-coding-harness/issues/640). It refines [ADR
0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), [ADR
0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md), and [ADR
0047](0047-own-provider-assembly-inside-cch-ai-and-hide-the-provider-capability.md), and applies
CODING_STANDARDS §13.3: the capability is exposed through the narrow operation needed by the
caller, not through the implementation graph it owns.

## Decisions

- `ModelRuntime` privately owns its `std::shared_ptr<ai::Models>` in `ModelRuntime::Impl`. Its
  Owner Interface exposes passive `Model`/`ProviderInfo` catalog values, non-secret availability
  and authentication-status queries, and the existing credential metadata and runtime-key controls;
  it does not expose a Models handle or Provider capability. The former `ai_models()` accessor is
  removed rather than deprecated or wrapped.
- `ModelRuntime::stream_factory()` returns the AI-owned `ai::ModelStreamFactory` value. Each
  factory closure captures the private `ai::Models` lifetime and delegates one turn to
  `Models::stream(Model, AiContext, SimpleStreamOptions)`. Provider lookup, Request
  Authentication, and stream dispatch therefore remain inside `ai::Models`; no Provider pointer,
  `AuthResult`, `Credential`, or key value crosses this factory seam. The shared capture preserves
  the Models lifetime required by ADR 0040 while the consuming `ModelStream` remains move-only.
- Agent Session assembly obtains the factory from Models Runtime. Its existing auth-guidance
  wrapping remains at the coding-agent factory/stream path and uses only the non-secret
  `is_using_oauth` predicate; it does not inspect or forward request-time authentication results.
- `ai::Models` remains the sole Provider composition seam from ADR 0047. Models Runtime keeps
  configuration and recomposition policy and submits complete passive `ProviderDefinition` values;
  this decision does not add a Runtime Provider registry, registration hook, or second graph.
- Scripted Provider Definition and transport injection stays in the existing test support. The
  friend test factories use a private construction hook to configure the privately held Models
  graph during test setup; no production caller receives a Models accessor or Provider-registration
  surface merely for testing.
- This boundary does not delete trusted AI-owner operations. In-process `ai::Models` continues to
  resolve `get_auth` and return the Credential produced by its `login` operation for its own
  Request Authentication and persistence path. The coding-agent Runtime/Agent/projection seam is
  not an authentication-resolution seam: `ModelRuntime` publishes no request-time `get_auth`, and
  its `login` returns `AsyncResult<void>` after Models persists the Credential and a successful
  login refreshes composition. Login failures carry only the existing string-based `support::Error`;
  `logout`, `check_auth`, provider status, credential metadata, and runtime API-key controls remain
  available as non-secret host surfaces. `support::Error` is `{code, message, detail, context}` and
  has no Credential payload or credential-carrying synchronization error to remove.
- Remote Models/projection services are not introduced here. ADR 0051's projection work starts
  from this already-narrow in-process boundary rather than from a leaked Models handle.

## Considered options

- **Keep `ai_models()` and rely on caller discipline**: rejected because the shared handle permits
  Provider graph mutation and trusted auth calls from Agent or host code; a comment cannot enforce
  the Owner boundary.
- **Add a second coding-agent stream abstraction**: rejected because `ai::ModelStreamFactory`
  already expresses the one-turn operation, its move-only lifetime, and its sink/error contract.
  The Runtime should bind that existing AI-owned value, not duplicate it.
- **Move Provider composition or Request Authentication into ModelRuntime**: rejected because
  ADRs 0029 and 0047 assign the live Provider graph and request-time auth to `ai::Models`; moving
  those responsibilities would split the owner and create a second Provider seam.
- **Expose a public test accessor or registration API**: rejected because tests do not justify a
  production capability surface. The private friend/test-support construction hook preserves
  deterministic scripted-provider and transport tests without returning the Models handle.
- **Wrap auth guidance inside `ai::Models`**: rejected because guidance is coding-agent policy and
  presentation text. Agent Session keeps that wrapper while consuming only a non-secret OAuth
  status predicate from Models Runtime.

## Consequences

- Agent Session and future hosts can stream one turn without holding the full Models graph. They
  receive an AI-owned operation and passive values rather than Provider composition or credentials.
- A factory keeps Models alive for the lifetime of each stream operation without exposing the
  lifetime owner. Runtime sharing and the serialized execution contract from ADR 0040 remain
  unchanged.
- ModelRuntime tests exercise the same factory used by Agent assembly. Scripted provider and
  transport setup remains available through test support, while production and non-test-support
  code have no Models Runtime accessor call path.
- Trusted `ai::Models` authentication tests remain valid and continue to cover refresh, persistence,
  and request precedence at the AI Owner seam. Host-facing auth assertions use status, availability,
  metadata, or observed request behavior rather than Credential values from Runtime.
- The Owner Interface documentation and package-seam description now state the private ownership
  explicitly. Include/architecture evidence continues to be checked by the Parity Architecture
  Gate; no package dependency or third-party dependency changes.
- The narrowed seam is a prerequisite for projection work, not a remote service contract. Future
  projections need a separately accepted ADR for transport, authorization, and non-secret status
  values.

## References

- Issue [#641](https://github.com/lanshengzhi/cpp-coding-harness/issues/641) and parent spec
  [#640](https://github.com/lanshengzhi/cpp-coding-harness/issues/640).
- Runtime secret-surface narrowing in [#642](https://github.com/lanshengzhi/cpp-coding-harness/issues/642).
- [ADR 0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), [ADR
  0032](0032-own-oauth-lifecycle-and-frontend-interaction-division-with-pi.md), [ADR
  0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md), [ADR
  0047](0047-own-provider-assembly-inside-cch-ai-and-hide-the-provider-capability.md), and [ADR
  0051](0051-decouple-headless-core-and-projections-with-zero-mutex-sampling.md).
