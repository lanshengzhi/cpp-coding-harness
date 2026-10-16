#!/usr/bin/env tsx
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent frozen observations of the live renderer switch (#1018, spec
/// #946).
///
/// The authoritative seam is the frozen `InteractiveMode.prototype.switchTuiMode`
/// body (`packages/coding-agent/src/modes/interactive/interactive-mode.ts:883-934`)
/// plus the exit path that calls it (`stopInteractiveTui`, `:874-881`). Both are
/// private, so the capture drives them exactly the way the frozen arbiter does
/// (`packages/coding-agent/test/interactive-tui.test.ts:111-127`): a context object
/// built with `Object.assign(Object.create(InteractiveMode.prototype), …)` so the
/// real method body runs against the real frozen toolkit.
///
/// The component tree mirrors `InteractiveMode.init` (`interactive-mode.ts:961-982`):
/// the same region containers are direct children of the active root *and*
/// children of the `createChatViewport` root installed with `setLayoutRoot`, so
/// the captured alias is the product's own tree shape. Every step records the
/// renderer mode, focus, child list, callback/rebind order, raw terminal bytes,
/// and the complete frame (rows, cursor, cursor visibility, and the full styled
/// cell grid whenever the frame draws any style at all).
///
/// Evidence boundary: the missing generated `packages/ai/src/providers/data/azure.json`
/// in the read-only frozen checkout is satisfied at module-load time with an
/// empty JSON document (see the loader below). Nothing on the exercised path
/// reads the Azure model catalog; without the stub the whole frozen module graph
/// cannot be imported because `npm run generate-models` never ran there.
export async function captureRendererSwitch(piCheckout: string) {
	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
	const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

	// Runtime-only stub for the generated Azure catalog absent from the frozen
	// checkout. It is installed before the first frozen import and never writes
	// inside the checkout.
	const { registerHooks } = await import("node:module");
	registerHooks({
		resolve(specifier, context, nextResolve) {
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return nextResolve(specifier, context);
		},
		load(url, context, nextLoad) {
			if (url.includes("/packages/ai/src/providers/data/azure.json")) {
				return { format: "json", source: "{}", shortCircuit: true };
			}
			return nextLoad(url, context);
		},
	});

	const { createChatViewport } = await import(coding("src/modes/interactive/chat-viewport.ts"));
	const { createInteractiveTui, createInteractiveTuiReference, InteractiveMode } = await import(
		coding("src/modes/interactive/interactive-mode.ts")
	);
	const { Container, Text, isViewportTUI } = await import(source("src/index.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	type Mode = "regular" | "fullscreen";
	type OverlayKind = "none" | "visible" | "hidden";

	/// xterm attributes that make a frame a styled frame: when any cell has one,
	/// the frame must record its complete grid.
	const isStyledCell = (cell: any): boolean =>
		Boolean(
			cell.style.bold ||
				cell.style.dim ||
				cell.style.italic ||
				cell.style.underline ||
				cell.style.blink ||
				cell.style.inverse ||
				cell.style.hidden ||
				cell.style.strikethrough ||
				cell.style.fg_color ||
				cell.style.bg_color ||
				cell.style.hyperlink,
		);

	/// The frozen VirtualTerminal is the renderer adapter; its exact output goes
	/// to a second public xterm instance that exposes full cell attributes
	/// (mirrors `capture/capture-named-tui.mts`, which never uses private pi
	/// hooks). The observer refuses frames the public adapter disagrees with.
	class SwitchTerminal extends VirtualTerminal {
		readonly emulator: any;
		cursorVisible = true;
		started = false;
		startCount = 0;
		stopCount = 0;
		readonly progressCalls: boolean[] = [];
		readonly writes: string[] = [];
		onEvent?: (event: string) => void;
		private phaseStart = 0;

		constructor(columns: number, rows: number) {
			super(columns, rows);
			this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
		}
		override write(data: string) {
			if ([...data.matchAll(/\x1b\]8;[^;]*;([^\x07\x1b]*)/g)].some((match) => match[1])) {
				throw new Error("OSC 8 cell capture needs link-aware evidence before adding linked scenarios");
			}
			this.writes.push(data);
			super.write(data);
			this.emulator.write(data);
		}
		override start(onInput: (data: string) => void, onResize: () => void): void {
			this.startCount += 1;
			this.started = true;
			this.onEvent?.("terminal-start");
			super.start(onInput, onResize);
		}
		override stop(): void {
			this.stopCount += 1;
			this.started = false;
			this.onEvent?.("terminal-stop");
			super.stop();
		}
		override setProgress(active: boolean): void {
			this.progressCalls.push(active);
			this.onEvent?.(`progress:${active}`);
			super.setProgress(active);
		}
		override hideCursor(): void {
			super.hideCursor();
			this.cursorVisible = false;
		}
		override showCursor(): void {
			super.showCursor();
			this.cursorVisible = true;
		}
		override resize(columns: number, rows: number) {
			super.resize(columns, rows);
			this.emulator.resize(columns, rows);
		}
		takeBytes(): string[] {
			const bytes = this.writes.slice(this.phaseStart);
			this.phaseStart = this.writes.length;
			return bytes;
		}
		async snapshot() {
			await this.flush();
			await new Promise<void>((resolve) => this.emulator.write("", resolve));
			const buffer = this.emulator.buffer.active;
			const screen = this.getViewport();
			const observedScreen = Array.from({ length: this.rows }, (_, row) =>
				buffer.getLine(buffer.viewportY + row).translateToString(true),
			);
			const observedCursor = this.getCursorPosition();
			if (
				JSON.stringify(screen) !== JSON.stringify(observedScreen) ||
				observedCursor.x !== buffer.cursorX ||
				observedCursor.y !== buffer.cursorY
			) {
				throw new Error(
					"cell observer diverged from frozen VirtualTerminal; extend the public terminal adapter before recording this scenario",
				);
			}
			const cells = Array.from({ length: this.rows }, (_, row) =>
				Array.from({ length: this.columns }, (_, col) => {
					const cell = buffer.getLine(buffer.viewportY + row).getCell(col);
					// Palette indices map to canonical SGR color values; default colors remain empty.
					const color = (foreground: boolean) => {
						const mode = foreground ? cell.isFgPalette() : cell.isBgPalette();
						const rgb = foreground ? cell.isFgRGB() : cell.isBgRGB();
						if (rgb) {
							const rgbValue = foreground ? cell.getFgColor() : cell.getBgColor();
							return `${foreground ? 38 : 48};2;${rgbValue >> 16};${(rgbValue >> 8) & 255};${rgbValue & 255}`;
						}
						if (!mode) return "";
						const value = foreground ? cell.getFgColor() : cell.getBgColor();
						return value < 8 ? String((foreground ? 30 : 40) + value) : `${foreground ? 38 : 48};5;${value}`;
					};
					if (cell.isOverline()) {
						throw new Error("overline cells need explicit C++ representation evidence before capture");
					}
					return {
						grapheme: cell.getChars(),
						continuation: cell.getWidth() === 0,
						style: {
							bold: !!cell.isBold(),
							dim: !!cell.isDim(),
							italic: !!cell.isItalic(),
							underline: !!cell.isUnderline(),
							blink: !!cell.isBlink(),
							inverse: !!cell.isInverse(),
							hidden: !!cell.isInvisible(),
							strikethrough: !!cell.isStrikethrough(),
							fg_color: color(true),
							bg_color: color(false),
							hyperlink: "",
							hyperlink_params: "",
						},
					};
				}),
			);
			return {
				screen,
				cells,
				cursor: { column: buffer.cursorX, row: buffer.cursorY },
				cursorVisible: this.cursorVisible,
				scrollback: this.getScrollBuffer().slice(0, buffer.baseY),
			};
		}
	}

	/// Plain surfaces only: the product call site needs the full interactive
	/// runtime, and a rendered input cursor would put a style in every frame.
	class EvidenceText extends Text {
		private current: string;
		constructor(text: string) {
			super(text, 0, 0);
			this.current = text;
		}
		override setText(text: string): void {
			super.setText(text);
			this.current = text;
		}
		value(): string {
			return this.current;
		}
	}

	interface HarnessOptions {
		readonly columns: number;
		readonly rows: number;
		readonly initialMode: Mode;
		readonly document: string[];
		readonly editor: string;
		readonly status: string;
		readonly footer: string;
		readonly showTerminalProgress?: boolean;
		readonly isStreaming?: boolean;
		readonly isCompacting?: boolean;
	}

	/// One live renderer pair with the product region tree. The five region
	/// containers are the same instances mounted on whichever root is active,
	/// exactly like `init`'s `mountInteractiveTui(this.renderer, [...])` call.
	class SwitchHarness {
		readonly terminal: SwitchTerminal;
		readonly order: string[] = [];
		readonly invalidated: string[] = [];
		readonly steps: any[] = [];
		readonly context: any;
		readonly regions: any[];
		readonly editor: any;
		readonly documentText: any;
		readonly focusProbe: any;
		readonly transcript: any;
		readonly layoutRoot: any;
		private readonly labels: Map<any, string>;
		private readonly options: HarnessOptions;
		private readonly overlayComponent: any;
		private overlayHandle: any;

		constructor(options: HarnessOptions) {
			this.options = options;
			this.terminal = new SwitchTerminal(options.columns, options.rows);
			this.documentText = new EvidenceText(options.document.join("\n"));
			this.editor = new EvidenceText(options.editor);
			// The frozen arbiter focuses a plain component beside the content it
			// checks (`interactive-tui.test.ts:121-127`); the same shape keeps the
			// focus-retention reading free of a rendered input cursor.
			this.focusProbe = {
				focused: false,
				render: () => [],
				invalidate: () => {},
			};
			this.overlayComponent = new Text("overlay entry", 0, 0);
			const documentContainer = new Container();
			documentContainer.addChild(this.documentText);
			const pendingContainer = new Container();
			const statusContainer = new Container();
			statusContainer.addChild(new Text(options.status, 0, 0));
			const editorContainer = new Container();
			editorContainer.addChild(this.editor);
			const footerContainer = new Container();
			footerContainer.addChild(new Text(options.footer, 0, 0));
			const viewport = createChatViewport({
				document: documentContainer,
				pendingMessages: pendingContainer,
				status: statusContainer,
				editor: editorContainer,
				footer: footerContainer,
				scrollbar: "hidden",
			});
			this.transcript = viewport.transcript;
			this.layoutRoot = viewport.root;
			this.regions = [documentContainer, pendingContainer, statusContainer, editorContainer, footerContainer];
			this.labels = new Map<any, string>([
				[documentContainer, "document"],
				[pendingContainer, "pendingMessages"],
				[statusContainer, "status"],
				[editorContainer, "editor"],
				[footerContainer, "footer"],
				[this.editor, "editor-text"],
				[this.documentText, "document-text"],
				[this.focusProbe, "focus-probe"],
				[this.overlayComponent, "overlay-entry"],
			]);
			// Our own synthetic components are instrumented so the switch's
			// `nextUi.invalidate()` and the mounted-root invalidation cascade are
			// visible in the recorded callback order.
			for (const [component, label] of this.labels) {
				if (typeof component?.invalidate !== "function") {
					throw new Error(`component ${label} has no invalidate: ${Object.prototype.toString.call(component)}`);
				}
				const original = component.invalidate.bind(component);
				component.invalidate = () => {
					this.invalidated.push(label);
					original();
				};
			}
			this.context = Object.assign(Object.create(InteractiveMode.prototype), {
				runtimeHost: {
					session: {
						settingsManager: {
							getFullscreenCopyOnSelect: () => true,
							getFullscreenWheelScrollLines: () => "auto",
							getShowTerminalProgress: () => options.showTerminalProgress ?? true,
						},
						isStreaming: options.isStreaming ?? false,
						isCompacting: options.isCompacting ?? false,
					},
				},
				renderer: undefined,
				ui: undefined,
				fullscreenLayoutRoot: this.layoutRoot,
				options: { tuiMode: options.initialMode },
				themeController: {
					rebindTui: () => this.order.push(`theme-rebind@${this.mode()}`),
				},
				extensionTerminalInputSubscriptions: new Set(),
				mainScreenRenderState: undefined,
			});
			this.context.ui = createInteractiveTuiReference(() => this.context.renderer);
			this.context.renderer = this.createRenderer(options.initialMode);
			this.terminal.onEvent = (event: string) => this.order.push(`${event}@${this.mode()}`);
			this.mount(this.context.renderer);
			this.context.renderer.setFocus(this.focusProbe);
			const subscription: any = { handler: () => {} };
			// The accessor pair stays installed so every switch cycle records the
			// unsubscribe-then-rebind sequence, not just the first one.
			let rebound: (() => void) | undefined;
			Object.defineProperty(subscription, "unsubscribe", {
				configurable: true,
				get: () => {
					this.order.push(`extension-unsubscribe@${this.mode()}`);
					return rebound ?? (() => {});
				},
				set: (next: () => void) => {
					assert.equal(typeof next, "function", "rebind must install the new renderer's listener");
					this.order.push(`extension-listener-rebound@${this.mode()}`);
					rebound = next;
				},
			});
			this.context.extensionTerminalInputSubscriptions.add(subscription);
		}

		async start(): Promise<void> {
			this.context.renderer.start();
			await this.terminal.waitForRender();
		}

		resetTrace(): void {
			this.order.length = 0;
			this.invalidated.length = 0;
			this.steps.length = 0;
			this.terminal.takeBytes();
		}

		mode(): Mode {
			return this.context.renderer.mode;
		}

		createRenderer(mode: Mode) {
			return createInteractiveTui({
				tuiMode: mode,
				showHardwareCursor: false,
				logDirectory: "/tmp",
				terminal: this.terminal,
				fullscreenCopyOnSelect: true,
				fullscreenWheelScrollLines: "auto",
			});
		}

		mount(renderer: any): void {
			for (const region of this.regions) renderer.addChild(region);
			// The probe is a direct child on purpose: it is captured and moved by
			// the same `[...children]` path the product's regions use.
			renderer.addChild(this.focusProbe);
			if (isViewportTUI(renderer)) renderer.setLayoutRoot(this.layoutRoot);
		}

		labelOf(component: any): string {
			return this.labels.get(component) ?? "other";
		}

		children(): string[] {
			return this.context.renderer.children.map((child: any) => this.labelOf(child));
		}

		layoutRootInstalled(): boolean {
			// The frozen instance's own field, read only for this recorded fact;
			// every behavior is driven through public methods.
			return isViewportTUI(this.context.renderer) && this.context.renderer.layoutRoot === this.layoutRoot;
		}

		presentOverlay(kind: OverlayKind) {
			if (kind === "none") return;
			this.steps.push({ action: "present-overlay", overlay: kind });
			this.overlayHandle = this.context.renderer.showOverlay(this.overlayComponent, {
				width: 14.0,
				anchor: "top-right",
			});
			if (kind === "hidden") this.overlayHandle.setHidden(true);
		}

		switch(mode: Mode, restoreProgress = true, startRenderer = true): boolean {
			this.steps.push({ action: "switch-tui-mode", mode, restoreProgress, startRenderer });
			return (InteractiveMode.prototype as any).switchTuiMode.call(this.context, mode, restoreProgress, startRenderer);
		}

		stopInteractiveTui(output: "transcript" | "resume-hint"): void {
			this.steps.push({ action: "stop-interactive-tui", fullscreenExitOutput: output });
			(InteractiveMode.prototype as any).stopInteractiveTui.call(this.context, output);
		}

		state() {
			return {
				mode: this.mode(),
				focus: this.context.renderer.getFocusedComponent() === null ? null : this.labelOf(this.context.renderer.getFocusedComponent()),
				children: this.children(),
				editorText: this.editor.value(),
				transcript: {
					scrollTop: this.transcript.scrollTop,
					viewportHeight: this.transcript.viewportHeight,
					isFollowingEnd: this.transcript.isFollowingEnd,
					isScrollbarVisible: this.transcript.isScrollbarVisible,
				},
				terminal: {
					startCount: this.terminal.startCount,
					stopCount: this.terminal.stopCount,
					progressCalls: [...this.terminal.progressCalls],
				},
				layoutRootInstalled: this.layoutRootInstalled(),
			};
		}

		async frame(name: string, options: { styled?: boolean } = {}) {
			this.steps.push({ action: "capture-frame", frame: name, ...(options.styled === true ? { styled: true } : {}) });
			if (options.styled === true) {
				// One designated frame per family direction carries a colour run so
				// the capture proves the complete styled grid is recorded; every
				// other frame must be unstyled and is verified as such below.
				this.documentText.setText(this.styledDocument());
				this.context.renderer.requestRender();
			}
			await this.terminal.waitForRender();
			const snapshot = await this.terminal.snapshot();
			const bytes = this.terminal.takeBytes();
			const styled = snapshot.cells.some((row: any[]) => row.some(isStyledCell));
			assert.equal(
				styled,
				options.styled === true,
				`${name}: ${styled ? "styled" : "unstyled"} frame where ${options.styled === true ? "the complete grid" : "no styles"} was expected; samples=${JSON.stringify(
					snapshot.cells
						.flatMap((row: any[], rowIndex: number) =>
							row.map((cell: any, column: number) => ({ cell, row: rowIndex, column })),
						)
						.filter(({ cell }: any) => isStyledCell(cell))
						.slice(0, 8),
				)}`,
			);
			return {
				name,
				screen: snapshot.screen.map((line: string) => line.trimEnd()),
				cursor: snapshot.cursor,
				cursorVisible: snapshot.cursorVisible,
				// A styled frame always carries the complete grid and scrollback,
				// so a style can never hide in a frame that did not record it.
				...styled ? { cells: snapshot.cells, scrollback: snapshot.scrollback } : {},
				...(options.styled === true ? { styleProbe: true, styleProbeDocument: this.styledDocument() } : {}),
				bytes,
			};
		}

		private styledDocument(): string {
			const lines = [...this.options.document];
			lines[lines.length - 1] = `\x1b[31m${lines[lines.length - 1]}\x1b[0m`;
			return lines.join("\n");
		}

		finish() {
			if (this.terminal.started) this.context.renderer.stop();
			this.terminal.emulator.dispose();
		}
	}

	const scenario = async (name: string, inputs: any, run: (harness: SwitchHarness) => Promise<any>) => {
		const harness = new SwitchHarness(inputs);
		await harness.start();
		harness.resetTrace();
		const expected = await run(harness);
		harness.finish();
		return {
			name,
			dimensions: { columns: inputs.columns, rows: inputs.rows },
			inputs: { ...inputs, steps: [...harness.steps] },
			expected,
		};
	};

	const baseInputs = (initialMode: Mode, overrides: any = {}) => ({
		columns: 28,
		rows: 8,
		initialMode,
		document: ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"],
		editor: "draft prompt",
		status: "status ready",
		footer: "footer hints",
		...overrides,
	});

	const scenarios: any[] = [];

	// 1. Same mode is a no-op, checked before the overlay guard.
	scenarios.push(
		await scenario("same-mode-returns-true-without-touching-the-renderer", baseInputs("regular", { overlay: "visible" }), async (harness) => {
			harness.presentOverlay("visible");
			const before = await harness.frame("initial");
			const stateBefore = harness.state();
			const rendererBefore = harness.context.renderer;
			const bytesBefore = harness.terminal.writes.length;
			const result = harness.switch("regular");
			const bytesWrittenBySwitch = harness.terminal.writes.length - bytesBefore;
			const after = await harness.frame("same-mode-request");
			assert.equal(result, true);
			assert.equal(harness.context.renderer, rendererBefore, "same mode must keep the live renderer");
			assert.equal(bytesWrittenBySwitch, 0, "same mode must not write");
			assert.equal(harness.terminal.startCount, 1);
			assert.equal(harness.terminal.stopCount, 0);
			return {
				result,
				rendererReplaced: harness.context.renderer !== rendererBefore,
				bytesWrittenBySwitch,
				stateBefore,
				stateAfter: harness.state(),
				callbacks: [...harness.order],
				invalidated: [...harness.invalidated],
				frameBefore: before,
				frameAfter: after,
			};
		}),
	);

	// 2. An overlay entry refuses the switch, hidden entries included.
	scenarios.push(
		await scenario(
			"overlay-entry-refuses-the-switch-and-leaves-everything-intact",
			{ ...baseInputs("fullscreen"), cases: ["hidden", "visible"] },
			async () => {
				const cases: any[] = [];
				for (const kind of ["hidden", "visible"] as OverlayKind[]) {
					const caseHarness = new SwitchHarness(baseInputs("fullscreen", { overlay: kind }));
					await caseHarness.start();
					caseHarness.resetTrace();
					caseHarness.presentOverlay(kind);
					const before = await caseHarness.frame(`overlay-${kind}`);
					const stateBefore = caseHarness.state();
					const bytesBefore = caseHarness.terminal.writes.length;
					const result = caseHarness.switch("regular");
					const bytesWrittenBySwitch = caseHarness.terminal.writes.length - bytesBefore;
					const after = await caseHarness.frame("refused");
					assert.equal(result, false, `${kind} overlay must refuse the switch`);
					assert.equal(caseHarness.mode(), "fullscreen");
					assert.equal(caseHarness.terminal.startCount, 1);
					assert.equal(caseHarness.terminal.stopCount, 0);
					assert.equal(bytesWrittenBySwitch, 0, `${kind} overlay must not write`);
					assert.deepEqual(caseHarness.children(), stateBefore.children);
					cases.push({
						name: `${kind}-overlay`,
						dimensions: { columns: caseHarness.terminal.columns, rows: caseHarness.terminal.rows },
						inputs: { ...baseInputs("fullscreen", { overlay: kind }), steps: [...caseHarness.steps] },
						expected: {
							overlayHidden: kind === "hidden",
							result,
							bytesWrittenBySwitch,
							stateBefore,
							stateAfter: caseHarness.state(),
							callbacks: [...caseHarness.order],
							frameBefore: before,
							frameAfter: after,
						},
					});
					caseHarness.finish();
				}
				return { cases };
			},
		),
	);

	// 3. Regular -> fullscreen keeps the one component tree.
	scenarios.push(
		await scenario("regular-to-fullscreen-retains-children-focus-editor-and-transcript", baseInputs("regular"), async (harness) => {
			const before = await harness.frame("regular");
			const stateBefore = harness.state();
			const result = harness.switch("fullscreen");
			const after = await harness.frame("fullscreen", { styled: true });
			return {
				result,
				stateBefore,
				stateAfter: harness.state(),
				callbacks: [...harness.order],
				invalidated: [...harness.invalidated],
				frameBefore: before,
				frameAfter: after,
			};
		}),
	);

	// 4. Fullscreen -> regular keeps the same tree and detaches the retained root.
	scenarios.push(
		await scenario("fullscreen-to-regular-retains-the-tree-and-detaches-the-layout-root", baseInputs("fullscreen"), async (harness) => {
			const before = await harness.frame("fullscreen");
			const stateBefore = harness.state();
			const retired = harness.context.renderer;
			const rootRows = harness.layoutRoot.render(harness.terminal.columns);
			const result = harness.switch("regular");
			const after = await harness.frame("regular", { styled: true });
			const retiredRows = retired.render(harness.terminal.columns);
			assert.equal(result, true);
			return {
				result,
				stateBefore,
				stateAfter: harness.state(),
				// The retired root's render() no longer returns the installed
				// root while the live root does: the alias was detached.
				retiredRootStillRendersLayoutRoot: JSON.stringify(retiredRows) === JSON.stringify(rootRows),
				callbacks: [...harness.order],
				invalidated: [...harness.invalidated],
				frameBefore: before,
				frameAfter: after,
			};
		}),
	);

	// 5. The main-screen render-state capture/restore path, both directions.
	scenarios.push(
		await scenario("main-screen-render-state-transfers-out-and-back", baseInputs("regular"), async (harness) => {
			const before = await harness.frame("regular");
			const captured = harness.context.renderer.captureRenderState();
			const result = harness.switch("fullscreen");
			const contextState = harness.context.mainScreenRenderState;
			assert.equal(contextState.previousWidth, captured.previousWidth, "the switch must store the live main-screen width");
			assert.deepEqual(contextState.previousLines, captured.previousLines, "the switch must store the live main-screen lines");
			const fullscreen = await harness.frame("fullscreen");
			const back = harness.switch("regular");
			const after = await harness.frame("regular-again");
			const recaptured = harness.context.renderer.captureRenderState();
			return {
				result,
				backResult: back,
				// What the frozen switch itself captured off the live main
				// screen before replacing it, and what it restored on return.
				switchCapturedWidth: contextState?.previousWidth ?? null,
				switchCapturedLines: contextState?.previousLines?.length ?? null,
				capturedWidth: captured.previousWidth,
				capturedLines: captured.previousLines.length,
				restoredPreviousLinesMatch: JSON.stringify(recaptured.previousLines) === JSON.stringify(contextState?.previousLines ?? null),
				stateBefore: harness.state(),
				callbacks: [...harness.order],
				frameBefore: before,
				frameFullscreen: fullscreen,
				frameAfter: after,
			};
		}),
	);

	// 6. startRenderer = false completes the swap without starting the new root.
	scenarios.push(
		await scenario("start-renderer-false-completes-without-starting-the-new-root", baseInputs("regular"), async (harness) => {
			const before = await harness.frame("regular");
			const stateBefore = harness.state();
			const bytesBefore = harness.terminal.writes.length;
			const result = harness.switch("fullscreen", false, false);
			const after = await harness.frame("not-started");
			assert.equal(result, true);
			assert.equal(harness.mode(), "fullscreen");
			assert.equal(harness.terminal.startCount, 1, "the new root must not start");
			return {
				result,
				stateBefore,
				stateAfter: harness.state(),
				bytesWrittenBySwitch: harness.terminal.writes.length - bytesBefore,
				callbacks: [...harness.order],
				invalidated: [...harness.invalidated],
				frameBefore: before,
				frameAfter: after,
			};
		}),
	);

	// 7. The progress-restore condition.
	scenarios.push(
		await scenario(
			"progress-restore-uses-the-show-terminal-progress-and-session-busy-condition",
			{ ...baseInputs("regular"), cases: ["all-true", "streaming-false", "compacting-true", "progress-disabled", "restore-disabled"] },
			async () => {
				const cases: any[] = [];
				const caseInputs = [
					{ name: "all-true", showTerminalProgress: true, isStreaming: true, isCompacting: false, restoreProgress: true },
					{ name: "streaming-false", showTerminalProgress: true, isStreaming: false, isCompacting: false, restoreProgress: true },
					{ name: "compacting-true", showTerminalProgress: true, isStreaming: false, isCompacting: true, restoreProgress: true },
					{ name: "progress-disabled", showTerminalProgress: false, isStreaming: true, isCompacting: false, restoreProgress: true },
					{ name: "restore-disabled", showTerminalProgress: true, isStreaming: true, isCompacting: false, restoreProgress: false },
				];
				for (const entry of caseInputs) {
					const caseHarness = new SwitchHarness(baseInputs("regular", entry));
					await caseHarness.start();
					caseHarness.resetTrace();
					const before = await caseHarness.frame("regular");
					const result = caseHarness.switch("fullscreen", entry.restoreProgress);
					const after = await caseHarness.frame("fullscreen");
					cases.push({
						name: entry.name,
						dimensions: { columns: caseHarness.terminal.columns, rows: caseHarness.terminal.rows },
						inputs: { ...baseInputs("regular", entry), steps: [...caseHarness.steps] },
						expected: {
							showTerminalProgress: entry.showTerminalProgress,
							isStreaming: entry.isStreaming,
							isCompacting: entry.isCompacting,
							restoreProgress: entry.restoreProgress,
							result,
							progressCalls: [...caseHarness.terminal.progressCalls],
							stateAfter: caseHarness.state(),
							callbacks: [...caseHarness.order],
							frameBefore: before,
							frameAfter: after,
						},
					});
					caseHarness.finish();
				}
				return { cases };
			},
		),
	);

	// 8. The exit path: stopInteractiveTui through a real overlay stack.
	scenarios.push(
		await scenario(
			"transcript-exit-switches-to-regular-renders-without-starting-and-stops",
			baseInputs("fullscreen", { overlay: "visible" }),
			async (harness) => {
				harness.presentOverlay("visible");
				const before = await harness.frame("fullscreen-with-overlay");
				const stateBefore = harness.state();
				const exitingRenderer = harness.context.renderer;
				const overlayEntriesBefore = exitingRenderer.hasOverlayEntries;
				const startCountBefore = harness.terminal.startCount;
				harness.stopInteractiveTui("transcript");
				const after = await harness.frame("transcript-exit");
				assert.equal(harness.mode(), "regular");
				assert.equal(harness.terminal.startCount, startCountBefore, "the exit path must not start a renderer");
				return {
					stateBefore,
					stateAfter: harness.state(),
					overlayEntriesBefore,
					oldRendererOverlayEntriesAfter: exitingRenderer.hasOverlayEntries,
					currentRendererOverlayEntries: harness.context.renderer.hasOverlayEntries,
					callbacks: [...harness.order],
					invalidated: [...harness.invalidated],
					frameBefore: before,
					frameAfter: after,
				};
			},
		),
	);

	return {
		sourceEndpoints: [
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:883",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:874",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:866",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:2594",
			"packages/coding-agent/src/modes/interactive/tui-renderer.ts:22",
			"packages/coding-agent/src/modes/interactive/chat-viewport.ts:createChatViewport",
			"packages/coding-agent/test/interactive-tui.test.ts:111",
			"packages/tui/src/tui.ts:911",
			"packages/tui/src/tui-main-screen.ts:captureRenderState,restoreRenderState",
			"packages/tui/src/tui-alt-screen.ts:321",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.getViewport,VirtualTerminal.waitForRender",
		],
		scenarios,
	};
}
