---
status: accepted
---

# Align the Linux TUI Toolkit and Native TUI with pi v1.0.4

[Spec #946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946) selects the complete
Linux TUI capability surface and its necessary product consumers, replacing the historical
regular-only subset and its behavioral exemptions. This record is attributed to that spec
and the user's authorization to apply its readiness-review revisions. It records the accepted
target and migration obligations; acceptance of this decision does not claim implementation
or behavioral verification has completed.

## Target and evidence authority

The Named Baseline is **`pi-v1.0.4`**, exact revision
**`7c10bd4337495ee613f2224843ecdf349b80d1df`**, on Linux x86-64 with glibc.
The frozen source and a separately captured TUI Evidence Bundle determine comparison outcomes.
Pi remains evidence for these selected capabilities, not architecture authority for unrelated
packages. The Product Architecture Contract continues to govern the Owner graph.

The target includes equivalent capability availability, defaults, caller responsibilities,
and observable text, screen, input, lifecycle and asynchronous outcomes. TypeScript method names,
inheritance and byte-identical ANSI are not blanket requirements; exact protocol contracts still
require exact comparison. C++ representation differences cannot conceal a missing capability.

## Included capabilities and ownership

`cch_tui` owns reusable terminal input and framing, raw-input consume/rewrite listeners, render
policy, common lifecycle/focus/overlay behavior, regular and fullscreen renderers, coordinate
composition, constrained layout and scrolling. Its capability surface includes mouse routing,
selection/search/navigation, toolkit components and editor contracts, Unicode/ANSI/fuzzy utilities,
Markdown tables and reusable math, images, colors/terminal queries, Linux clipboard/transcoding
capability interfaces, diagnostics and reusable buffering/parsing helpers.

Product frontends own Session Projection consumption and accounting, prompt/interrupt admission,
CLI/settings policy, theme pairing, selector composition, extension-UI hosting and host-adapter
assembly. Clipboard, image conversion and other physical operations use injected capabilities.
The Owner graph is unchanged: `cch_tui` has no Owner dependencies, and headless Session/Agent/AI
acquire no frontend dependency. New Owner Interface values remain passive.

A common TUI interface serves working MainScreen and AltScreen implementations. Fullscreen-only
capabilities remain explicit; shared lifecycle, components, focus and input have one authority.
Typed input preserves shortcut identity separately from insertion text. Immutable resolved binding
snapshots remain, with updatable shared/local contexts. Cursor and image values stay out of band,
and `VirtualTerminal` remains the reusable evidence harness. Per-instance ownership, explicit errors,
the existing Runtime Root and strict no-exception toolchain remain required. No process-global
mutable manager, per-Owner event loop or new reverse dependency is introduced.

## Render policy and terminal output

The toolkit owns the first frame, request coalescing, ordinary **16 ms** pacing, immediate keyboard
updates, forced rendering, Busy recovery scheduling and cancellation at stop/restart/destruction.
An injected scheduler provides monotonic time, deferred execution and cancellable timing on the
existing Runtime Root; hosts supply execution capabilities rather than a second paint policy.

Projection delivery remains asynchronous and product-owned. Base/Patch convergence, serialized
Core state, bounded mailboxes, successful-frame accounting and frozen transcript caches remain.
The product requests toolkit rendering instead of rendering synchronously during stream delivery.
Queued work or Busy does not count as a successfully committed frame. Input admission remains
independent of repaint, and a claimed key does not establish that visible state changed.

Only the renderer path writes interactive output. MainScreen uses one relative line-flow strategy
from the shell's existing cursor, without startup DSR anchoring or destructive timeout fallback.
Growth, shrink, resize, scrollback, cursor, images, render-state transfer and stop/preserve-screen
follow the frozen outcomes. AltScreen owns the absolute viewport and alternate-buffer lifecycle;
new terminal modes require normal-stop, failure-rollback and emergency-restoration coverage.

Toolkit composition translates cursor/image geometry through stacking, padding, layout, clipping,
scrolling and overlays before placement. Product callers do not repair offsets. ScrollView geometry
and clipping metadata must support later protocol-level source cropping; reducing destination bounds
alone does not establish correct image rendering. Terminal negotiation, partial-write recovery,
backpressure and Close remain required for both modes.

## Product modes and migration obligations

The product uses `tuiMode` and `--tui-mode regular|fullscreen` with frozen precedence and controls.
Fullscreen becomes the default only at final integration, after included capabilities and product
flows are assembled and verified. Mode changes preserve text, component/focus state and one
Session/terminal/runtime lifetime; overlay handling, query/theme/listener rebinding and exit output
follow the frozen product behavior. Toolkit restoration and product exit policy must not emit the
transcript twice.

Each slice adapts real consumers and stays buildable and focused-testable. The following handoffs
make the transition explicit, rather than authorizing permanent duplicate paths:

- [#978](https://github.com/lanshengzhi/cpp-coding-harness/issues/978) transfers actual Native TUI
  paint policy to the toolkit and stops Native Editor terminal injection/local echo. Projection
  consumption and successful-frame accounting remain in the product.
- [#983](https://github.com/lanshengzhi/cpp-coding-harness/issues/983) moves actual regular product
  composition to relative line flow and stops consuming the old dock/absolute-margin path.
- [#987](https://github.com/lanshengzhi/cpp-coding-harness/issues/987) physically removes old
  local-echo/dock APIs and bypasses and accepts streaming/input/Busy/Close convergence.
- [#1013](https://github.com/lanshengzhi/cpp-coding-harness/issues/1013) owns the shared frontend
  C++ extension-UI host, registration/disposal and real product assembly;
  [#1014](https://github.com/lanshengzhi/cpp-coding-harness/issues/1014) consumes that host for
  editor replacement. Reload and renderer replacement cannot retain stale callbacks.

## Supersession and retained contracts

- [ADR 0035](0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md):
  superseded for the old toolkit subset, old baseline and behavioral exemptions, including
  fullscreen/extension/diagnostic omissions, the 1 MiB paste bound and consumer-specific defaults.
  Typed keys, immutable bindings, passive values, image/cursor metadata and VirtualTerminal remain
  C++ representations, with their observable outcomes verified against the new baseline.
- [ADR 0037](0037-own-the-pi-aligned-main-screen-scrollback-flow-for-the-tui-toolkit-renderer.md)
  and [ADR 0041](0041-own-the-anchored-absolute-flow-model-for-the-tui-main-screen-renderer.md):
  the regular scrollback objective remains; absolute positioning, startup DSR, destructive fallback,
  the fullscreen exclusion and blanket renderer exemptions are replaced by this frozen contract.
- [ADR 0051](0051-decouple-headless-core-and-projections-with-zero-mutex-sampling.md): Native TUI
  counted ticker/preview paint and direct local-echo policy are superseded. Headless/serialized Core,
  asynchronous presentation and the Block Frozen Protocol remain. This also replaces Native paint
  requirements retained by [ADR 0052](0052-replace-projection-sampling-with-one-patch-stream-and-bounded-mailboxes.md),
  while its one Projection Stream, Base/Patch convergence and bounded subscriber mailboxes remain.
- [ADR 0066](0066-pi-capability-scope-decisions-and-open-questions.md): this records the toolkit
  membership ruling for #946. Unrelated capability decisions and open questions remain separate.

## Exclusions and acceptance

Native macOS/Windows helpers, TS/JS extension execution, unconsumable parser/runtime-object exports,
an installed SDK and stable ABI are excluded. Linux operations frozen pi itself leaves unavailable
retain that outcome. Unrelated Agent/AI/storage/provider/configuration redesign is outside this scope.
Concrete C++ API names and math/image dependency choices belong to focused implementation probes
within the accepted scope, using pinned dependencies and the supported strict toolchain.

[#947](https://github.com/lanshengzhi/cpp-coding-harness/issues/947) establishes independent named
capture/replay with provenance and rejection of missing, wrong-revision or tampered bundles;
[#948](https://github.com/lanshengzhi/cpp-coding-harness/issues/948) accounts for every export,
public operation, default and necessary consumer. Each included row needs an owning contract,
implementation ticket and discriminating comparison case. Old fixtures retain historical identity;
Pike-generated expectations or fallback to old evidence cannot accept new behavior.

Acceptance combines public component/Terminal/VirtualTerminal comparisons, real CLI/Interactive
Session Run and process/PTY coverage, and dated emulator evidence for IME and Kitty/iTerm2-protocol
outcomes. Missing emulator evidence leaves its row unverified. Included failures remain open with
explicit blockers; they cannot be reclassified Deferred or hidden in close-out. Final delivery
requires complete capability accounting, removal evidence and the repository's Full Validation.
