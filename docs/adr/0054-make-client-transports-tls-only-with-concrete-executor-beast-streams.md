---
status: accepted
---

# Make client transports TLS-only and key Beast streams on the concrete executor

The cch_ai client transports (the Codex WebSocket transport, the HTTPS stream transport, and the OAuth HTTP client) instantiated Boost.Beast machinery over two stream types — plain `websocket::stream<basic_stream<tcp, any_io_executor, ...>>` and its TLS counterpart — with every operation keyed on the type-erased `any_io_executor`, the `beast::tcp_stream` default. Production traffic is TLS-only, so the plain instantiation was dead weight in the distribution binary, and the erased executor multiplied instantiation keys. This decision implements issue [#638](https://github.com/lanshengzhi/cpp-coding-harness/issues/638) and changes a Supported surface of [ADR 0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md): the client URL surface narrows to TLS schemes only.

## Decisions

- Client transports are TLS-only. The WebSocket transport accepts `wss://` only; the HTTPS stream transport and the OAuth HTTP client already accepted `https://` only. `ws://` and `http://` client URLs fail at the transport boundary with a Validation error, and the plain WebSocket branch is removed so its instantiations are unreachable and reclaimed by the release link's `--gc-sections`/ICF.
- The `OAuthCallbackServer` is exempt: it is a loopback HTTP **server** for the browser login flow, not a client transport, and stays plain.
- Beast stream types in the transport TUs (`BoostBeastWebSocketTransport`, `BoostBeastStreamTransport`, `OAuthHttpClient`, `OAuthCallbackServer`) are spelled with the concrete executor the Runtime actually runs — `boost::asio::io_context::executor_type` — through the private `cch::ai::TransportExecutor` aliases, replacing the `beast::tcp_stream` default. The transport executor contract tightens accordingly: transports must be driven by a single-threaded `io_context` executor. The `cch_agent_core` `any_io_executor` process-pipe plumbing (RuntimeRoot/Process) is unrelated and untouched.
- The WebSocket connect request carries a test-only trust injection (an optional additional CA certificate in PEM form) so tests can run a local `wss://` mock under a committed generated test CA. Production never sets it; verification mode, host-name verification, and the default verify paths are unchanged when it is absent.
- Glaze single-TU explicit instantiation is rejected for now: the measured post-ICF glaze slice (~0.12 MiB across 8 TUs in 4 Owners) sits at the ticket's ~0.1 MB adoption threshold and session-record JSON stays byte-stable under golden tests; hand-written hot-path JSON remains rejected.

## Considered options

- Keep the plain `ws://` client branch for local testing: rejected because production traffic is TLS-only and the branch's Beast instantiations survive into the distribution binary; the local `wss://` mock with a test CA preserves the test coverage instead.
- Type the transport awaitables on the concrete executor (`boost::asio::awaitable<T, io_context::executor_type>`): rejected because the erased `any_io_executor` is the declared type at the AsyncResult/model-stream bridge (ADR 0040), so the change would ripple through every adapter coroutine for no contract gain; deriving the concrete executor from the ambient executor inside the private transport TUs stays contained.
- Leave the OAuth client and callback server on the `beast::tcp_stream` default: rejected because any remaining `any_io_executor`-keyed Beast stream instantiation keeps the erased-executor machinery reachable and forfeits the fold.
- Introduce a runtime executor-type check on the derivation: rejected because the single-threaded `io_context` executor is already the transports' documented contract; violating it is a programming error, not a runtime error channel.

## Consequences

- No plain client path remains in production code; the release binary carries exactly four Beast instantiation contexts: the WSS client, the HTTPS stream client, the OAuth TLS client, and the plain loopback HTTP server.
- Transport executor contracts tighten from "any single-threaded executor" to "single-threaded `io_context` executor"; tests and the Runtime loop already satisfy it.
- The test-only trust injection is a private transport-request field; no Owner Interface changes, so the Parity Architecture Gate evidence is unaffected.
- ADR 0033's adapter surface narrows: non-TLS client base URLs are rejected rather than served over plain HTTP/WebSocket.

## References

- Issue [#638](https://github.com/lanshengzhi/cpp-coding-harness/issues/638) (Beast/Glaze template instantiation convergence) and its size series [#636](https://github.com/lanshengzhi/cpp-coding-harness/issues/636), [#637](https://github.com/lanshengzhi/cpp-coding-harness/issues/637), [#639](https://github.com/lanshengzhi/cpp-coding-harness/issues/639).
- ADRs [0033](0033-own-the-supported-api-adapter-surface-for-the-three-provider-paths.md), [0040](0040-own-asynchronous-operations-and-the-serialized-runtime-lifecycle.md), and [0047](0047-own-provider-assembly-inside-cch-ai-and-hide-the-provider-capability.md).
