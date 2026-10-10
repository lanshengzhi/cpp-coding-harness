---
status: accepted
---

# Align the Linux TUI Toolkit with pi v1.0.4

The toolkit audit found observable behavior differences and missing reusable capabilities against pi v1.0.4. The owner confirmed the complete scope and design below through grill-with-docs. This record accepts the target architecture and capability scope; it does not claim delivery or start implementation. [Spec #946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946) and the [published ticket index](../specs/pi-tui-v1.0.4-tickets.md) turn these decisions into the implementation plan.

## Confirmed direction

The owner selected the following directions in round 1 of this discussion:

- Include the complete pi TUI capability surface available on Linux, delivered in independently verifiable slices. This includes previously deferred toolkit capabilities; delivery order and product assembly requirements are still to be resolved.
- Permit idiomatic C++ type and lifecycle representations, while matching capability availability, default behavior, and caller responsibilities. Reassess each previously recorded divergence rather than carrying it forward automatically.
- Limit the platform obligation to Pike's current Supported Platform: Linux x86-64 with glibc. This work introduces no macOS or Windows support obligation.
- Compare against the already selected pi v1.0.4 revision `7c10bd4337495ee613f2224843ecdf349b80d1df` in `/home/lansy/Work/github/coding-agent/pi`, rather than a floating upstream revision.

The owner's motivation is to align both observable experience and reusable module interfaces/architecture as closely as practical. Semantic Parity retains its glossary meaning: observable meanings and state transitions, independent of C++ interface shape. Equivalent capability coverage and caller responsibility are additional requirements of this effort, not a redefinition of that term.

The owner selected the following directions in round 2:

- Deliver both the reusable toolkit and the necessary Native TUI/CLI integration, so users can actually exercise the included capabilities. The initial recommendation assumed pi's historical main-screen default; round 3 corrected that assumption against the frozen source and selected the actual v1.0.4 fullscreen default and entry points.
- Require matching final screen text/cell styles, cursor placement, scroll behavior, state transitions, default configuration, and asynchronous ordering. Raw ANSI sequences may differ when outcomes are equivalent; protocol encoding and explicit string contracts are compared exactly where relevant. Acceptance requires frozen pi v1.0.4 differential evidence rather than Pike-only goldens.
- Retain Pike additions only when recorded evidence establishes their value and they do not obstruct alignment. Replace and physically remove conflicting or duplicate paths instead of maintaining parallel compatibility modes. Individual additions still need disposition.

The owner selected the following directions in round 3:

- Match pi v1.0.4's fullscreen default, `tuiMode`, `--tui-mode regular|fullscreen`, settings-menu controls, runtime mode switching, and fullscreen exit behavior. The historical main-screen-default recommendation is withdrawn after checking the frozen product source.
- Deliver replaceable-editor and consume/rewrite input-listener capabilities through both the toolkit and the minimum product extension-UI host. Include necessary registration, cleanup, reload, and renderer-switch rebinding, without expanding this into an unrelated extension-system rewrite.
- Make `cch_tui` own first-frame scheduling, render-request coalescing, the ordinary 16 ms cadence, immediate keyboard rendering, and shutdown cancellation. Execute through a narrow host-provided scheduling capability over the existing Runtime Root; introduce no per-Owner event loop. Session Projection consumption and accounting remain product-owned, and duplicate paint scheduling must be removed.

The owner selected the following directions in round 4:

- Preserve typed component input, preceded by an ordered raw-input consume/rewrite listener chain at the frozen upstream-equivalent stage. Shortcut identity and inserted text must be distinct values; listeners must see the same applicable input as pi, including rewriting and paste/release behavior.
- Preserve out-of-band cursor and protocol-neutral image metadata, but make the toolkit own complete coordinate composition through nested content, viewport/clipping, overlays, and terminal placement. Product callers must not manually repair layout offsets.
- Eliminate regular-mode DSR-timeout screen/scrollback destruction and other observable main-screen differences. Use a smallest runnable probe to select one coherent positioning strategy; ADR 0041's absolute model is not automatically retained, and a second permanent renderer path is not an acceptable workaround.

The owner selected the following directions in round 5:

- Use explicitly updatable shared and local keybinding contexts over immutable resolved snapshots. Dispatch, hints, and help must converge on the same updated bindings. Do not introduce a process-global mutable manager.
- Place reusable color values/conversion/mixing and terminal color query/notification capabilities in `cch_tui`. Keep theme selection, pairing, and product policy in the frontend; wire startup, late replies, and live changes end to end.
- Expose Linux-equivalent toolkit clipboard/transcoding capabilities through injected adapters. Product composition supplies PNG conversion and the applicable Wayland/X11/remote OSC 52 paths. Do not introduce global state merely to imitate TypeScript registration functions.

The owner selected the following directions in round 6:

- Use relative line flow for regular mode and absolute viewport positioning for fullscreen. Remove regular-mode startup DSR dependence and its destructive timeout fallback, with no permanent dual regular-positioning strategy. The [small positioning probe](../research/tui-regular-positioning-probe.md) supports the unknown-origin scenario; the remaining renderer scenarios require differential acceptance.
- Match frozen pi's observable Unicode width, completion, fuzzy ranking, and paste-count/threshold results while retaining UTF-8/grapheme internals. A suspected upstream defect requires an explicit owner exception, not an implementer's silent improvement.
- Remove the visible 1 MiB paste truncation. Use chunking/backpressure for resource control; any rejection/cancellation must have an explicit result rather than silent text loss.

The owner selected the following directions in round 7:

- Provide one common TUI interface with MainScreen and AltScreen implementations, plus an explicit fullscreen capability interface. Share component/focus/overlay/input lifecycle semantics and preserve state through mode switching. Idiomatic C++ ownership and errors remain; mechanically inheriting pi's Container is not required.
- Remove Editor's direct terminal-writing/local-echo path and the old dock render protocol. Express fixed input regions through fullscreen layout and route output through the renderer. Future latency work must measure and optimize that coherent path rather than preserve the bypass.
- Align SelectList's reusable responsibilities, defaults, and input bubbling with pi. Move product-specific title/search/chrome composition to the product frontend. Keep VirtualTerminal and consumed C++ helpers, fixing original-offset reporting; do not manufacture C++ equivalents of unconsumable Marked/TypeScript parser types. Provide equivalent Linux diagnostic and reusable-function capabilities, with representation-specific image/cursor helpers expressed through the chosen metadata model.

The owner selected the following directions in round 8:

- Deliver equivalent extension-UI contracts and actual host integration for C++ extensions. Executing pi TypeScript/JavaScript extension files is outside this effort; no new language-execution host is implied by toolkit alignment.
- Establish a distinct frozen pi-v1.0.4 TUI evidence bundle before implementation acceptance. Retain older evidence provenance, but make new acceptance select the named frozen baseline explicitly. Each behavior ticket carries pi-derived comparisons; use virtual-terminal evidence for cells/styles/cursor/state, PTY evidence for lifecycle/framing, and dated real-terminal evidence for emulator-dependent image and IME outcomes that automation cannot establish.
- Finish with Full Validation, the Product Architecture Gate, and a row-by-row capability/interface matrix adjudication. Do not relabel unfinished included work as Deferred to close the effort.
- Deliver complete verifiable slices in blocking order. Adapt real consumers and delete replaced paths with their owning slice. Expose only assembled capability entry points; enable the fullscreen default in the final product-integration stage. Staging does not change the full target scope.

## Completion boundary

All product-scope branches raised by this discussion are resolved, and the owner confirmed the complete shared understanding in round 9. [Spec #946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946) synthesizes this accepted decision; its [local specification](../specs/pi-tui-v1.0.4-alignment.md) records the user stories, ownership, acceptance groups, and testing seams confirmed by the owner. The owner approved 79 small tickets, published as #947–#1025 with 137 direct blocking edges after independent review; the [ticket index](../specs/pi-tui-v1.0.4-tickets.md) records their scope, dependencies and context budgets. No implementation starts as part of this interview.

Detailed C++ signatures, dependency selection, capture schema, and test shard sizing remain engineering work within the assigned tickets. They must satisfy the settled contracts and use the smallest probe before choosing between materially different technical directions.

The complete target includes the Linux renderer/lifecycle/overlay/focus/input/editor/autocomplete/keybinding/component/Markdown/image/color/clipboard/layout/scroll/mouse/selection/search/navigation/diagnostic capabilities of the frozen package, and their necessary product entry points. Tables, LaTeX, image conversion, live themes, dynamic composition, exact configuration defaults, and editor submission/history/menu behavior are included under that target. TypeScript parser-object re-exports, native macOS/Windows support, and pi TS/JS extension-file execution are explicit exclusions; they are not excuses to omit equivalent Linux behavior or C++ caller capabilities.

## Relationship to existing decisions

This proposal does not change the global Product Architecture Contract or make pi the authority for unrelated packages (ADR 0053). It proposes an explicit per-capability alignment direction within the TUI Toolkit, consistent with ADR 0066's owner-controlled subset membership.

This accepted target supersedes or retains the following clauses. These are target decisions, not claims that the proposed implementation already exists.

| Existing record | Treatment for this effort |
| --- | --- |
| ADR 0025: modular toolkit versus product frontend | Retained, including deterministic terminal seams and RAII. No installed SDK/ABI commitment is added. |
| ADR 0035: regular-only Linux subset and deferred fullscreen/layout/mouse/extensions/live-color/diagnostics | Superseded by full Linux capability coverage, subject to the explicit language/platform exclusions above. |
| ADR 0035: typed events, immutable bindings, image sidecars, cursor query, injected timers, parser implementation | Retained only in the refined forms confirmed here: raw listeners precede typed dispatch, insertion text is separate, bindings have updatable contexts, coordinates compose, timers use the host Runtime, and parser output matches pi. |
| ADR 0035: bounded 1 MiB paste, absent disableSubmit, historical editor/menu defaults | Superseded by input preservation and frozen pi behavior. |
| ADR 0037: Main-Screen Scrollback Flow | Observable flow contract retained; differential renderer outcomes are checked against v1.0.4. |
| ADR 0041: anchored absolute regular flow, startup DSR and destructive timeout | Superseded by regular relative flow; absolute fullscreen viewport rendering remains a separate renderer. Retained rewrite/shrink policies lose automatic exemption and must meet current outcomes. |
| ADR 0040: Runtime Root, serialized lifecycle, ownership | Retained. Toolkit render policy executes through the host's narrow scheduling adapter, without reverse Owner edges or a per-Owner Runtime. |
| ADR 0050: input admission separated from rendering | Retained. Claiming input does not become a proxy for whether visible state changed. |
| ADR 0051: direct terminal local echo and frozen dock prediction | Superseded; renderer owns all output and fullscreen layout owns fixed regions. Projection convergence requirements remain. |
| ADR 0051/0052: product Session Projection consumption/accounting | Retained in the product frontend; duplicate paint scheduling is replaced by toolkit policy. Any incompatible counted/preview assumptions must be explicitly reconciled in the affected slice. |
| ADR 0053/0039: Product Architecture Contract and strict Owner graph | Retained. TUI alignment does not promote pi to global architecture authority or bypass the manifest/gate. |

The affected historical records carry supersession pointers to this decision. Their implementation/evidence details stay historical until the corresponding slices land; no record may claim capabilities are already delivered merely because this scope is chosen.

## Evidence

- [Current-source gap inventory](../research/cch-tui-pi-v1.0.4-gap-inventory.md).
- [Executed unknown-origin regular positioning probe](../research/tui-regular-positioning-probe.md).
- [Frozen pi CLI mode selection](</home/lansy/Work/github/coding-agent/pi/packages/coding-agent/src/cli/args.ts:219>).
- [Frozen pi fullscreen defaults and settings](</home/lansy/Work/github/coding-agent/pi/packages/coding-agent/src/core/settings-manager.ts:1348>).
- [Frozen pi renderer product assembly](</home/lansy/Work/github/coding-agent/pi/packages/coding-agent/src/modes/interactive/tui-renderer.ts:22>).
- [Toolkit scope and historical divergences](0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md).
- [Product architecture authority](0053-replace-pi-parity-authority-with-the-product-architecture-contract.md).
- [Owner capability membership principle](0066-pi-capability-scope-decisions-and-open-questions.md).
