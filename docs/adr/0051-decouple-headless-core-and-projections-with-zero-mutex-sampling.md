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
- Projections (TUI, Web, GUI) pull state snapshots on independent frame-rate ticks (30/60 FPS), automatically coalescing intermediate tokens and decoupling render cost from inference rate.
- Future frontends (Web via WebSocket/SSE, GUI) can attach as peer projections to the versioned patch stream without modifying the Headless Core.
