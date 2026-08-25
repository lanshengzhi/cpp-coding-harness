---
status: accepted
---

# Adopt a strict no-exception core with private asynchronous bridges

> Refined by [ADR 0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md): the `AsyncResult` completion bridge moved from `src/ai/` to `src/support/` as a private support header, so bridge machinery no longer lives inside a capability Owner while being consumed across Owner boundaries. The Owner-Interface clause below is unchanged: no Owner contract exposes exception or Boost.Asio types.

The project-owned production and test targets are moving to a strict no-exception core. Ordinary failure is represented by `cch::support::Expected<T>`, `ExpectedVoid`, and `std::error_code`; an exception is not a second error channel. Runtime invariant violations remain `std::terminate` contracts in every build mode.

This decision records issue [#480](https://github.com/lanshengzhi/cpp-coding-harness/issues/480), under parent issue [#479](https://github.com/lanshengzhi/cpp-coding-harness/issues/479). It changes the failure mechanism, not the pi-observable lifecycle, cancellation, ordering, persistence, or observer semantics frozen by [ADR 0017](0017-keep-event-subscribers-as-weak-observers.md) and [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md). The strict policy is now the default build ([#487](https://github.com/lanshengzhi/cpp-coding-harness/issues/487)); turning `CCH_STRICT_NO_EXCEPTIONS` off is a deliberate, warned local deviation.

## Policy boundary

All project-owned production and test C++ targets compile with `-fno-exceptions` by default. The `CCH_STRICT_NO_EXCEPTIONS` option (default ON) and the Parity Architecture Gate make this requirement enforceable: every supported configure runs the Gate with strict evidence, so an exception-enabled compile command, an unauthorized exception pointer, or a rethrow fails the build closed. Explicit exception-enable flags are forbidden. Strict validation audits the private exception allowlist and fails on rethrow operations. The migration tickets (#481, #483–#486) removed the ordinary exception paths; the exception allowlist is now closed to the private completion-bridge headers only.

Owner Interfaces contain only value-based expected/error contracts. They do not expose `std::exception_ptr`, `std::exception`, Boost.Asio awaitables, executors, completion signatures, or exception-based callbacks. Capability Owners convert setup and completion failures at their private boundary and return the declared expected error. Weak observers remain weak observers: callback failures are caught, diagnosed within the documented bound, and deactivate the observer rather than vetoing committed progress.

## Private Boost.Asio bridge

Boost.Asio remains an implementation dependency of the private asynchronous completion bridge. Its awaitables, executors, and completion machinery do not cross an Owner boundary. The bridge may receive `std::exception_ptr` from `co_spawn`'s completion signature because that is Boost.Asio's private completion representation. In the staged exception-enabled build it maps a non-null pointer directly to the operation's generic `Expected` error without rethrowing; when `BOOST_ASIO_NO_EXCEPTIONS` is active, a non-null pointer is impossible and terminates as a Runtime invariant violation. It must never call `std::rethrow_exception`, expose the pointer, or use it as an application-level error channel. The exact temporary source allowlist is recorded in `cmake/parity/manifest.json` and enforced by strict Gate validation.

The pinned Boost.Asio headers support the no-exception configuration through Boost configuration and `BOOST_ASIO_NO_EXCEPTIONS`; the private `cch_ai` hook for Boost's no-exception `throw_exception` entry point terminates if that invariant is reached. A minimal `co_spawn` translation unit and the bridge tests have been validated with `-fno-exceptions`. The bridge remains private so replacing Asio later cannot change an Owner contract.

## CLI parsing

CLI11 is not part of the eventual runtime compile graph. Its pinned headers contain direct throws and have no supported no-exception switch. The existing `parse_args -> Expected<CliConfig>` seam remains the Owner-facing boundary; the CLI migration replaces the private CLI11 implementation with an exception-free parser without changing the parsed configuration contract or CLI behavior.

## Error and invariant consequences

- Expected failures use `std::unexpected`, `cch::support::Error`, or the more specific Owner error contract already required by the operation.
- Filesystem and POSIX APIs use their `std::error_code`/status-returning forms.
- Setup failures are converted before asynchronous initiation; producer initiation and terminal completion are `noexcept` after initiation.
- Duplicate consumption/completion, illegal lifecycle transitions, and controlled callback contract violations terminate. They are programming or Runtime invariant failures, not recoverable domain errors.
- No catch-and-rethrow adapter is added to preserve an old implementation detail through an Owner Interface.

## Considered options

- Keep exceptions as the shared failure channel: rejected because it leaks implementation policy through Owner Interfaces and gives asynchronous setup, completion, and Close paths incompatible ownership semantics.
- Expose Boost.Asio's exception and awaitable types publicly: rejected because the event-loop implementation would become an Owner contract.
- Disable exceptions globally immediately: rejected for this frontier ticket because the existing runtime and CLI11 paths must first be migrated in bounded, reviewable slices.
- Keep CLI11 and search for an undocumented switch: rejected because its direct throws make the dependency unsuitable for the strict runtime graph.

## Consequences

- Error paths become explicit, inspectable, and compatible with `-fno-exceptions`.
- Asio remains usable as private machinery without imposing its completion representation on the architecture.
- The staged Gate detects exception-enabled compile commands and unauthorized exception bridge usage instead of silently accepting them.
- Migration tickets must preserve the existing lifecycle and observer semantics while removing exception propagation.

## References

- No-exception architecture specification and parent issue [#479](https://github.com/lanshengzhi/cpp-coding-harness/issues/479).
- [ADR 0017](0017-keep-event-subscribers-as-weak-observers.md) for weak observer failure isolation.
- [ADR 0020](0020-make-cancellation-a-supported-end-to-end-capability.md) for cancellation ownership.
- [ADR 0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md) for `AsyncResult`, initiation, completion, and invariant semantics.
- The pinned Boost.Asio and CLI11 dependency research recorded in the parent issue.
