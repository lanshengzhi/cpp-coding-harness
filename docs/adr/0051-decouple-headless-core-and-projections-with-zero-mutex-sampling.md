---
status: accepted
---

# Decouple Headless Core and Projections with Zero-Mutex Sampling

The Pike runtime decouples the Authoritative Core and all presentation surfaces (Native TUI, Web, GUI, and remote inspectors) into an asynchronous, read-only projection architecture. The Core runtime operates completely headless on a single-threaded, lock-free serialized domain, publishing immutable, versioned state patches and snapshots. All UI components, terminal renderers, and input buffers eliminate operating system recursive mutexes. The Native TUI becomes a consumer projection pulling state on an independent 30/60 FPS frame ticker, adopting the Block Frozen Protocol (`Active` -> `Finalized` -> `Committed`) for streaming transcript blocks and client-side prediction for user prompt typing.

This decision supersedes the synchronous UI binding and recursive-mutex synchronization introduced in ADR 0025, ADR 0035, and ADR 0040. Core session and agent invariants, the single authoritative session, and serialized execution domain semantics remain authoritative.

## Considered options

- Retain background terminal worker thread and fine-grained mutexes: rejected because recursive mutexes create thread starvation during compute-heavy rendering, blocking keystroke reading and terminal output draining.
- Push-based synchronous rendering on every streaming chunk: rejected because token streaming rates (50–100+ tokens/s) flood the event loop and compound whole-document markdown re-parsing ($O(N^2)$), causing pervasive interactive input lag.
- Layered backward-compatibility wrappers for legacy pi UI components: rejected because retaining `render(): string[]` stack allocations and pass-through adapters preserves heap churn and hides structural latency.

## Consequences

- The Authoritative Core operates completely headless without terminal dimensions, ANSI escape codes, or direct presentation callbacks.
- All recursive mutexes across `cch_tui` and Native TUI components (`Tui::mutex_`, `InteractiveView::mutex_`, `Editor::impl_mutex`) are eliminated.
- Stdin input handling is integrated directly into the non-blocking event loop or delivered via a lock-free SPSC queue, enabling sub-millisecond local echo and prediction for typing.
- The transcript adopts the Block Frozen Protocol: completed blocks freeze immutable line caches in memory, eliminating redundant MD4C re-parsing and re-wrapping across streaming tokens.
- Projections (TUI, Web, GUI) pull state snapshots on independent frame-rate ticks (30/60 FPS), automatically coalescing intermediate tokens and decoupling render cost from inference rate (for the Native TUI's sanctioned preview tier, see the deviation section below).
- Future frontends (Web via WebSocket/SSE, GUI) can attach as peer projections to the versioned patch stream without modifying the Headless Core.

## Sanctioned deviation: Native TUI latency-first immediate previews (#614)

The Native TUI projection composites on two tiers instead of ticker-only pacing:

- The ~33 ms (~30 FPS) frame ticker (`InteractiveEngine::start_frame_ticker`)
  stays the authoritative, counted frame: it pulls the latest projection
  snapshot, consumes the sampled version and dirty state, and re-arms itself.
- Engine-side view mutations additionally produce one coalesced immediate
  preview frame (`InteractiveEngine::post_invalidate`): at most one in-flight
  post, uncounted, never consuming the snapshot version or dirty state, and
  never re-arming the ticker schedule. Streaming bursts therefore still
  coalesce at the event-loop level, and every burst's final state is painted
  by the next counted ticker frame regardless.

This is a deliberate, sanctioned deviation from ticker-only pacing for the
Native TUI, recorded here and in `CODING_STANDARDS.md` §15. Rationale:

- Latency-first: ticker-only pacing would add up to one frame interval of
  latency to every engine-side update (status changes, diagnostics, editor
  echo) for no observable interactive benefit.
- pi parity: pi's TUI pairs throttled coalesced renders
  (`TuiBase.MIN_RENDER_INTERVAL_MS = 16` in `packages/tui/src/tui.ts`) with a
  latency-first immediate render path (`Tui.requestImmediateRender`); the
  preview tier preserves that hybrid, so ticker-only pacing would itself be
  the pi divergence.
- Cost containment comes from the Block Frozen Protocol (frozen committed
  line caches) plus differential terminal rendering, not from pacing alone,
  so per-preview frame cost stays bounded.

The deviation was deliberately tuned with the frame/reflow convergence work
(#597), and the interactive scenario suite settles its assertions on those
painted preview outcomes; re-timing it requires re-validating that suite.
Future projections (Web, GUI) should start ticker-only and add a preview tier
only with the same measured justification.
