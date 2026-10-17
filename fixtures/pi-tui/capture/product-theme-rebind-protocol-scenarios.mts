/**
 * product-theme-rebind-protocol-scenarios.mts
 *
 * Independent frozen measurement and canonical evidence capture for Issue #1037 (Spec #946).
 * Measures actual frozen pi v1.0.4 (`7c10bd4337495ee613f2224843ecdf349b80d1df`):
 *   1. Same-root late color query reply acceptance control via onLateReply after 100ms timeout.
 *   2. Direction A (Regular -> Fullscreen) switch with old query pending, exact 2031l disable / 2031h enable
 *      ordering, unconsumed old replies on replacement root, and light scheme notification rebind.
 *   3. Direction B (Fullscreen -> Regular) symmetrical switch with old query pending on fullscreen root,
 *      exact 2031l disable / 2031h enable ordering, unconsumed old replies on regular root, and dark scheme rebind.
 *   4. Ambiguity control for unkeyed OSC 10/11 color responses arriving at active root with pending query.
 *   5. Two-switch roundtrip rebind idempotency maintaining strictly 1 active listener and single burst.
 *   6. Malformed, unsupported, duplicate, and unrelated CSI sequences handling during query lifecycle.
 */

import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";

export async function captureProductThemeRebindProtocol(piCheckout: string) {
	const packageDirMap: Record<string, string> = {
		"pi-tui": "tui",
		"pi-ai": "ai",
		"pi-agent-core": "agent",
		"pi-coding-agent": "coding-agent",
		"pi-protocol": "protocol",
		"pi-session-backends": "session-backends",
		"pi-mcp": "mcp",
		"pi-client": "client",
		"pi-chord": "chord",
		"pi-durable": "durable",
		"pi-env": "env",
		"pi-telemetry": "telemetry",
	};

	const { registerHooks } = await import("node:module");
	registerHooks({
		resolve(specifier, context, nextResolve) {
			if (specifier.startsWith("@earendil-works/")) {
				const parts = specifier.slice("@earendil-works/".length).split("/");
				const pkgName = parts[0];
				const subParts = parts.slice(1);
				const dir = packageDirMap[pkgName];
				if (dir) {
					if (subParts.length === 0) {
						return {
							url: pathToFileURL(path.join(piCheckout, "packages", dir, "src/index.ts")).href,
							shortCircuit: true,
						};
					}
					const sub = subParts.join("/");
					return {
						url: pathToFileURL(
							path.join(
								piCheckout,
								"packages",
								dir,
								"src",
								sub.endsWith(".ts") ? sub : `${sub}.ts`,
							),
						).href,
						shortCircuit: true,
					};
				}
			}
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return nextResolve(specifier, context);
		},
		load(url, context, nextLoad) {
			if (url.includes("/packages/ai/src/providers/data/azure.json") || url.endsWith("/azure.json")) {
				return { format: "json", source: "{}", shortCircuit: true };
			}
			return nextLoad(url, context);
		},
	});

	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const tui = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;

	const { InteractiveThemeController } = await import(coding("src/modes/interactive/theme/theme-controller.ts"));
	const { setTerminalColors, setTerminalColorScheme, initTheme, getTerminalTheme } = await import(
		coding("src/modes/interactive/theme/theme.ts")
	);
	const { createInteractiveTui, createInteractiveTuiReference } = await import(
		coding("src/modes/interactive/tui-renderer.ts")
	);
	const { InteractiveMode } = await import(coding("src/modes/interactive/interactive-mode.ts"));
	const { Text } = await import(tui("src/index.ts"));
	const { VirtualTerminal } = await import(tui("test/virtual-terminal.ts"));

	class InstrumentedTerminal extends VirtualTerminal {
		writes: { timestamp: number; data: string }[] = [];
		activeInputHandler?: (data: string) => void;
		activeHandlerOwner = "none";

		constructor(cols = 80, rows = 24) {
			super(cols, rows);
		}

		override write(data: string): void {
			this.writes.push({ timestamp: Date.now(), data });
			super.write(data);
		}

		override start(onInput: (data: string) => void, onResize: () => void): void {
			this.activeInputHandler = onInput;
			super.start(onInput, onResize);
		}

		override stop(): void {
			this.activeInputHandler = undefined;
			this.activeHandlerOwner = "none";
			super.stop();
		}

		injectInput(data: string): { deliveredToOwner: string; handlerPresent: boolean } {
			const handler = this.activeInputHandler;
			const owner = this.activeHandlerOwner;
			if (handler) {
				handler(data);
				return { deliveredToOwner: owner, handlerPresent: true };
			}
			return { deliveredToOwner: "none", handlerPresent: false };
		}

		getWritesSince(markIndex: number): string[] {
			return this.writes.slice(markIndex).map((w) => w.data);
		}
	}

	function escapeRepresentation(str: string): string {
		return str
			.replace(/\x1b/g, "\\x1b")
			.replace(/\x07/g, "\\x07")
			.replace(/\n/g, "\\n")
			.replace(/\r/g, "\\r");
	}

	function extractProtocolCommands(writes: string[]): string[] {
		const text = writes.join("");
		const commands: string[] = [];
		const regex = /(\x1b\[\?2031[hl]|\x1b\]10;\?\x07|\x1b\]11;\?\x07|\x1b\]4;\d+;\?\x07|\x1b\[c)/g;
		let match;
		while ((match = regex.exec(text)) !== null) {
			commands.push(match[1]);
		}
		return commands;
	}

	const OSC10_QUERY = "\x1b]10;?\x07";
	const OSC11_QUERY = "\x1b]11;?\x07";
	const DA1_QUERY = "\x1b[c";
	const MODE_2031_ENABLE = "\x1b[?2031h";
	const MODE_2031_DISABLE = "\x1b[?2031l";
	const SCHEME_REPORT_DARK = "\x1b[?997;1n";
	const SCHEME_REPORT_LIGHT = "\x1b[?997;2n";
	const DA1_REPLY = "\x1b[?1;2c";

	function makeOsc10Reply(hexRgb: string): string {
		const r = hexRgb.slice(1, 3);
		const g = hexRgb.slice(3, 5);
		const b = hexRgb.slice(5, 7);
		return `\x1b]10;rgb:${r}${r}/${g}${g}/${b}${b}\x07`;
	}

	function makeOsc11Reply(hexRgb: string): string {
		const r = hexRgb.slice(1, 3);
		const g = hexRgb.slice(3, 5);
		const b = hexRgb.slice(5, 7);
		return `\x1b]11;rgb:${r}${r}/${g}${g}/${b}${b}\x07`;
	}

	let rootCounter = 0;

	interface InstrumentedTui {
		tui: any;
		rootId: string;
		mode: string;
		consumeColorCalls: { data: string; result: boolean; queueLengthBefore: number }[];
		consumeSchemeCalls: { data: string; result: boolean; listenersCountBefore: number }[];
		schemeListenerAddCount: number;
		schemeListenerRemoveCount: number;
	}

	function instrumentTuiInstance(
		tuiInstance: any,
		mode: string,
		terminal: InstrumentedTerminal,
	): InstrumentedTui {
		const rootId = `root-${++rootCounter}-${mode}`;
		const instr: InstrumentedTui = {
			tui: tuiInstance,
			rootId,
			mode,
			consumeColorCalls: [],
			consumeSchemeCalls: [],
			schemeListenerAddCount: 0,
			schemeListenerRemoveCount: 0,
		};

		const origConsumeColor = tuiInstance.consumeTerminalColorResponse?.bind(tuiInstance);
		if (origConsumeColor) {
			tuiInstance.consumeTerminalColorResponse = (data: string) => {
				const queueLen = tuiInstance.pendingTerminalColorQueries?.length ?? 0;
				const res = origConsumeColor(data);
				instr.consumeColorCalls.push({
					data: escapeRepresentation(data),
					result: res,
					queueLengthBefore: queueLen,
				});
				return res;
			};
		}

		const origConsumeScheme = tuiInstance.consumeTerminalColorSchemeReport?.bind(tuiInstance);
		if (origConsumeScheme) {
			tuiInstance.consumeTerminalColorSchemeReport = (data: string) => {
				const listenersCount = tuiInstance.terminalColorSchemeListeners?.size ?? 0;
				const res = origConsumeScheme(data);
				instr.consumeSchemeCalls.push({
					data: escapeRepresentation(data),
					result: res,
					listenersCountBefore: listenersCount,
				});
				return res;
			};
		}

		const origOnSchemeChange = tuiInstance.onTerminalColorSchemeChange?.bind(tuiInstance);
		if (origOnSchemeChange) {
			tuiInstance.onTerminalColorSchemeChange = (listener: any) => {
				instr.schemeListenerAddCount++;
				const unsubscribe = origOnSchemeChange(listener);
				return () => {
					instr.schemeListenerRemoveCount++;
					unsubscribe();
				};
			};
		}

		const origStart = tuiInstance.start?.bind(tuiInstance);
		if (origStart) {
			tuiInstance.start = () => {
				terminal.activeHandlerOwner = rootId;
				origStart();
			};
		}

		const origStop = tuiInstance.stop?.bind(tuiInstance);
		if (origStop) {
			tuiInstance.stop = (opts?: any) => {
				if (terminal.activeHandlerOwner === rootId) {
					terminal.activeHandlerOwner = "none";
				}
				origStop(opts);
			};
		}

		tuiInstance.__instrumented = instr;
		return instr;
	}

	function buildControlledContext(terminal: InstrumentedTerminal, initialMode: "regular" | "fullscreen") {
		const settingsManager: any = {
			getFullscreenCopyOnSelect: () => false,
			getFullscreenWheelScrollLines: () => 3,
			getShowTerminalProgress: () => false,
			getThemeSetting: () => undefined,
			getShowHardwareCursor: () => false,
			getClearOnShrink: () => false,
			theme: "system",
			getTheme: () => settingsManager.theme,
			getTerminalTheme: () => "dark",
			onSettingsChanged: () => () => {},
		};

		let currentRenderer: any = null;

		const context: any = Object.assign(Object.create(InteractiveMode.prototype), {
			runtimeHost: {
				session: {
					settingsManager,
					isStreaming: false,
					isCompacting: false,
				},
			},
			fullscreenLayoutRoot: new Text("layout root", 0, 0),
			options: { tuiMode: initialMode },
			rebindExtensionTerminalInputListeners: () => {},
			mountInteractiveTui: (tuiInstance: any, components: any[]) => {
				for (const c of components) tuiInstance.addChild(c);
			},
			mainScreenRenderState: undefined,
			onRightClickPaste: undefined,
		});

		Object.defineProperty(context, "renderer", {
			get() {
				return currentRenderer;
			},
			set(next) {
				if (next && next !== currentRenderer) {
					if (!next.__instrumented) {
						instrumentTuiInstance(next, next.mode ?? "unknown", terminal);
					}
				}
				currentRenderer = next;
			},
			configurable: true,
			enumerable: true,
		});

		const initialRaw = createInteractiveTui({
			tuiMode: initialMode,
			showHardwareCursor: false,
			logDirectory: "/tmp",
			terminal,
			fullscreenCopyOnSelect: false,
			fullscreenWheelScrollLines: 3,
		});
		context.renderer = initialRaw;
		context.ui = createInteractiveTuiReference(() => context.renderer);

		const text1 = new Text("document line", 0, 0);
		const text2 = new Text("editor line", 0, 0);
		context.renderer.addChild(text1);
		context.renderer.addChild(text2);

		let themeChangedCount = 0;
		let themeErrors: string[] = [];
		const themeController = new InteractiveThemeController(context.ui, {
			getSettingsManager: () => settingsManager,
			showError: (err: string) => {
				themeErrors.push(err);
			},
			onChanged: () => {
				themeChangedCount++;
			},
		});
		context.themeController = themeController;

		return {
			context,
			themeController,
			settingsManager,
			getRenderer: () => context.renderer,
			getInstrumented: (tuiInstance?: any): InstrumentedTui => (tuiInstance ?? context.renderer).__instrumented,
			themeErrors,
			getThemeChangedCount: () => themeChangedCount,
		};
	}

	const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
	const flushMicrotasks = () => new Promise<void>((resolve) => process.nextTick(resolve));

	function resetGlobalTheme() {
		setTerminalColors({});
		setTerminalColorScheme(undefined);
		initTheme("dark");
	}

	// ── Scenario 1: Same-root late query acceptance control ───────────────────
	async function runScenario1() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "regular");
		const { themeController, getRenderer, getInstrumented } = harness;

		const regularRoot = getInstrumented();
		getRenderer().start();
		assert.equal(terminal.activeHandlerOwner, regularRoot.rootId);

		const markStart = terminal.writes.length;
		themeController.applyFromSettings();
		const queryWrites = terminal.getWritesSince(markStart);
		const queryCommands = extractProtocolCommands(queryWrites);

		const allCommands = extractProtocolCommands(terminal.writes.map((w) => w.data));
		assert.ok(allCommands.includes(MODE_2031_ENABLE));
		assert.ok(queryCommands.includes(OSC10_QUERY));
		assert.ok(queryCommands.includes(OSC11_QUERY));
		assert.ok(queryCommands.includes(DA1_QUERY));
		assert.equal(queryCommands.length, 20);

		// Wait 120ms (timeout is 100ms)
		await sleep(120);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		// Delayed replies injected: FG #112233, BG #445566
		const LATE_FG = "#112233";
		const LATE_BG = "#445566";
		const markReplies = terminal.writes.length;
		terminal.injectInput(makeOsc10Reply(LATE_FG));
		terminal.injectInput(makeOsc11Reply(LATE_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		const colors = (themeController as any).terminalColors;
		assert.deepEqual(colors, {
			foreground: { r: 17, g: 34, b: 51 },
			background: { r: 68, g: 85, b: 102 },
			palette: undefined,
		});
		assert.equal(regularRoot.consumeColorCalls.length, 3);
		assert.equal(regularRoot.consumeColorCalls.every((c) => c.result === true), true);

		return {
			name: "same_root_late_query",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				initialMode: "regular",
				lateFg: LATE_FG,
				lateBg: LATE_BG,
				timeoutDelayMs: 120,
			},
			expected: {
				initialQueryCommands: queryCommands,
				mode2031Enabled: true,
				lateRepliesAccepted: true,
				settledColors: colors,
				consumeResults: regularRoot.consumeColorCalls.map((c) => ({ data: c.data, result: c.result })),
			},
		};
	}

	// ── Scenario 2: Direction A — Regular -> Fullscreen ───────────────────────
	async function runScenario2() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "regular");
		const { context, themeController, getRenderer, getInstrumented } = harness;

		const regularRoot = getInstrumented();
		getRenderer().start();
		assert.equal(terminal.activeHandlerOwner, regularRoot.rootId);

		// Phase A1: Start and trigger query on regular root
		const markA1 = terminal.writes.length;
		themeController.applyFromSettings();
		const phaseA1Writes = terminal.getWritesSince(markA1);
		const phaseA1Commands = extractProtocolCommands(phaseA1Writes);

		assert.equal(regularRoot.tui.pendingTerminalColorQueries?.length, 1);
		assert.equal(regularRoot.tui.terminalColorSchemeListeners?.size, 1);

		// Phase A2: Switch to fullscreen while query is pending
		const markA2 = terminal.writes.length;
		const switchResult = (InteractiveMode.prototype as any).switchTuiMode.call(context, "fullscreen");
		assert.equal(switchResult, true);
		const fullscreenRoot = getInstrumented();
		assert.notEqual(regularRoot.rootId, fullscreenRoot.rootId);
		assert.equal(terminal.activeHandlerOwner, fullscreenRoot.rootId);

		const phaseA2Writes = terminal.getWritesSince(markA2);
		const phaseA2Commands = extractProtocolCommands(phaseA2Writes);
		assert.deepEqual(phaseA2Commands, [MODE_2031_DISABLE, MODE_2031_ENABLE]);
		assert.equal(regularRoot.tui.stopped, true);
		assert.equal(regularRoot.tui.terminalColorSchemeListeners?.size, 0);
		assert.equal(fullscreenRoot.tui.terminalColorSchemeListeners?.size, 1);

		// Phase A3: Inject old regular-root query replies into active fullscreen terminal
		const OLD_FG = "#111111";
		const OLD_BG = "#222222";
		terminal.injectInput(makeOsc10Reply(OLD_FG));
		terminal.injectInput(makeOsc11Reply(OLD_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();

		const phaseA3ActiveCalls = fullscreenRoot.consumeColorCalls.map((c) => ({ ...c }));
		assert.equal(regularRoot.consumeColorCalls.length, 0);
		assert.equal(phaseA3ActiveCalls.length, 3);
		// Replacement root did not issue this query: all unconsumed
		assert.equal(phaseA3ActiveCalls.every((c) => c.result === false), true);
		assert.equal((themeController as any).terminalColors, undefined);

		// Phase A4: Wait for retired root timer
		await sleep(120);
		await flushMicrotasks();

		// Phase A5: Scheme report light on active fullscreen root
		const markA5 = terminal.writes.length;
		terminal.injectInput(SCHEME_REPORT_LIGHT);
		await flushMicrotasks();
		assert.equal(fullscreenRoot.consumeSchemeCalls.filter((c) => c.result === true).length, 1);

		const phaseA5Writes = terminal.getWritesSince(markA5);
		const phaseA5Commands = extractProtocolCommands(phaseA5Writes);
		assert.ok(phaseA5Commands.includes(OSC10_QUERY));
		assert.ok(phaseA5Commands.includes(OSC11_QUERY));
		assert.ok(phaseA5Commands.includes(DA1_QUERY));
		assert.equal(fullscreenRoot.tui.pendingTerminalColorQueries?.length, 1);

		// Phase A6: Answer new query on active root
		const NEW_FG = "#333333";
		const NEW_BG = "#eeeeee";
		terminal.injectInput(makeOsc10Reply(NEW_FG));
		terminal.injectInput(makeOsc11Reply(NEW_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		const colors = (themeController as any).terminalColors;
		assert.deepEqual(colors, {
			foreground: { r: 51, g: 51, b: 51 },
			background: { r: 238, g: 238, b: 238 },
			palette: undefined,
		});
		assert.equal(getTerminalTheme(), "light");

		const phaseA6Calls = fullscreenRoot.consumeColorCalls.slice(4);
		assert.equal(phaseA6Calls.length, 3);
		assert.equal(phaseA6Calls.every((c) => c.result === true), true);

		return {
			name: "direction_a_regular_to_fullscreen",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				direction: "regular_to_fullscreen",
				oldFg: OLD_FG,
				oldBg: OLD_BG,
				schemeReport: "light",
				newFg: NEW_FG,
				newBg: NEW_BG,
			},
			expected: {
				phaseA2SwitchCommands: phaseA2Commands,
				phaseA5QueryCommands: phaseA5Commands,
				activeHandlerOwnerAfterSwitch: fullscreenRoot.rootId,
				oldRepliesAcceptedByActive: false,
				schemeNotificationConsumed: true,
				settledTheme: "light",
				settledColors: colors,
				phaseA3ActiveConsume: phaseA3ActiveCalls.map((c) => ({ data: c.data, result: c.result })),
				phaseA6ActiveConsume: phaseA6Calls.map((c) => ({ data: c.data, result: c.result })),
			},
		};
	}

	// ── Scenario 3: Direction B — Fullscreen -> Regular ───────────────────────
	async function runScenario3() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "fullscreen");
		const { context, themeController, getRenderer, getInstrumented } = harness;

		const fullscreenRoot = getInstrumented();
		getRenderer().start();
		assert.equal(terminal.activeHandlerOwner, fullscreenRoot.rootId);

		// Phase B1: Start fullscreen and trigger query so query is PENDING BEFORE switch
		const markB1 = terminal.writes.length;
		themeController.applyFromSettings();
		const phaseB1Writes = terminal.getWritesSince(markB1);
		const phaseB1Commands = extractProtocolCommands(phaseB1Writes);

		assert.equal(fullscreenRoot.tui.pendingTerminalColorQueries?.length, 1);
		assert.equal(fullscreenRoot.tui.terminalColorSchemeListeners?.size, 1);

		// Phase B2: Switch to regular while query is pending on fullscreen root
		const markB2 = terminal.writes.length;
		const switchResult = (InteractiveMode.prototype as any).switchTuiMode.call(context, "regular");
		assert.equal(switchResult, true);
		const regularRoot = getInstrumented();
		assert.notEqual(fullscreenRoot.rootId, regularRoot.rootId);
		assert.equal(terminal.activeHandlerOwner, regularRoot.rootId);

		const phaseB2Writes = terminal.getWritesSince(markB2);
		const phaseB2Commands = extractProtocolCommands(phaseB2Writes);
		assert.deepEqual(phaseB2Commands, [MODE_2031_DISABLE, MODE_2031_ENABLE]);
		assert.equal(fullscreenRoot.tui.stopped, true);
		assert.equal(fullscreenRoot.tui.terminalColorSchemeListeners?.size, 0);
		assert.equal(regularRoot.tui.terminalColorSchemeListeners?.size, 1);

		// Phase B3: Inject old fullscreen-root replies into active regular terminal
		const OLD_FG = "#333333";
		const OLD_BG = "#444444";
		terminal.injectInput(makeOsc10Reply(OLD_FG));
		terminal.injectInput(makeOsc11Reply(OLD_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();

		const phaseB3ActiveCalls = regularRoot.consumeColorCalls.map((c) => ({ ...c }));
		assert.equal(fullscreenRoot.consumeColorCalls.length, 0);
		assert.equal(phaseB3ActiveCalls.length, 3);
		// Replacement root did not issue this query: all unconsumed
		assert.equal(phaseB3ActiveCalls.every((c) => c.result === false), true);
		assert.equal((themeController as any).terminalColors, undefined);

		// Phase B4: Wait for retired root timer
		await sleep(120);
		await flushMicrotasks();

		// Phase B5: Dark scheme report on regular root
		const markB5 = terminal.writes.length;
		terminal.injectInput(SCHEME_REPORT_DARK);
		await flushMicrotasks();
		assert.equal(regularRoot.consumeSchemeCalls.filter((c) => c.result === true).length, 1);

		const phaseB5Writes = terminal.getWritesSince(markB5);
		const phaseB5Commands = extractProtocolCommands(phaseB5Writes);
		assert.ok(phaseB5Commands.includes(OSC10_QUERY));
		assert.ok(phaseB5Commands.includes(OSC11_QUERY));
		assert.ok(phaseB5Commands.includes(DA1_QUERY));
		assert.equal(regularRoot.tui.pendingTerminalColorQueries?.length, 1);

		// Phase B6: Answer new query on active root
		const NEW_FG = "#222222";
		const NEW_BG = "#111111";
		terminal.injectInput(makeOsc10Reply(NEW_FG));
		terminal.injectInput(makeOsc11Reply(NEW_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		const colors = (themeController as any).terminalColors;
		assert.deepEqual(colors, {
			foreground: { r: 34, g: 34, b: 34 },
			background: { r: 17, g: 17, b: 17 },
			palette: undefined,
		});
		assert.equal(getTerminalTheme(), "dark");

		const phaseB6Calls = regularRoot.consumeColorCalls.slice(4);
		assert.equal(phaseB6Calls.length, 3);
		assert.equal(phaseB6Calls.every((c) => c.result === true), true);

		return {
			name: "direction_b_fullscreen_to_regular",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				direction: "fullscreen_to_regular",
				oldFg: OLD_FG,
				oldBg: OLD_BG,
				schemeReport: "dark",
				newFg: NEW_FG,
				newBg: NEW_BG,
			},
			expected: {
				phaseB2SwitchCommands: phaseB2Commands,
				phaseB5QueryCommands: phaseB5Commands,
				activeHandlerOwnerAfterSwitch: regularRoot.rootId,
				oldRepliesAcceptedByActive: false,
				schemeNotificationConsumed: true,
				settledTheme: "dark",
				settledColors: colors,
				phaseB3ActiveConsume: phaseB3ActiveCalls.map((c) => ({ data: c.data, result: c.result })),
				phaseB6ActiveConsume: phaseB6Calls.map((c) => ({ data: c.data, result: c.result })),
			},
		};
	}

	// ── Scenario 4: Ambiguity control — unkeyed reply with active pending query ─
	async function runScenario4() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "regular");
		const { context, themeController, getRenderer, getInstrumented } = harness;

		getRenderer().start();
		themeController.applyFromSettings();

		// Switch to fullscreen
		(InteractiveMode.prototype as any).switchTuiMode.call(context, "fullscreen");
		const fullscreenRoot = getInstrumented();

		// Explicitly query colors on active root
		const markQuery = terminal.writes.length;
		themeController.queryTerminalColors();
		const queryCommands = extractProtocolCommands(terminal.getWritesSince(markQuery));
		assert.equal(fullscreenRoot.tui.pendingTerminalColorQueries?.length, 1);

		// Arriving unkeyed reply
		const UNKEYED_FG = "#778899";
		const UNKEYED_BG = "#112233";
		terminal.injectInput(makeOsc10Reply(UNKEYED_FG));
		terminal.injectInput(makeOsc11Reply(UNKEYED_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		const colors = (themeController as any).terminalColors;
		assert.deepEqual(colors, {
			foreground: { r: 119, g: 136, b: 153 },
			background: { r: 17, g: 34, b: 51 },
			palette: undefined,
		});
		assert.equal(fullscreenRoot.consumeColorCalls.length, 3);
		assert.equal(fullscreenRoot.consumeColorCalls.every((c) => c.result === true), true);

		return {
			name: "unkeyed_reply_pending_target_fifo",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				unkeyedFg: UNKEYED_FG,
				unkeyedBg: UNKEYED_BG,
			},
			expected: {
				queryCommands,
				consumedFifo: true,
				settledColors: colors,
				consumeResults: fullscreenRoot.consumeColorCalls.map((c) => ({ data: c.data, result: c.result })),
			},
		};
	}

	// ── Scenario 5: Roundtrip rebind idempotency ──────────────────────────────
	async function runScenario5() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "regular");
		const { context, themeController, getRenderer, getInstrumented } = harness;

		getRenderer().start();
		themeController.applyFromSettings();
		const root1 = getInstrumented();
		assert.equal(root1.tui.terminalColorSchemeListeners?.size, 1);

		// Switch 1: Fullscreen
		const markSw1 = terminal.writes.length;
		(InteractiveMode.prototype as any).switchTuiMode.call(context, "fullscreen");
		const root2 = getInstrumented();
		const sw1Commands = extractProtocolCommands(terminal.getWritesSince(markSw1));
		assert.deepEqual(sw1Commands, [MODE_2031_DISABLE, MODE_2031_ENABLE]);
		assert.equal(root1.tui.terminalColorSchemeListeners?.size, 0);
		assert.equal(root2.tui.terminalColorSchemeListeners?.size, 1);

		// Switch 2: Regular
		const markSw2 = terminal.writes.length;
		(InteractiveMode.prototype as any).switchTuiMode.call(context, "regular");
		const root3 = getInstrumented();
		const sw2Commands = extractProtocolCommands(terminal.getWritesSince(markSw2));
		assert.deepEqual(sw2Commands, [MODE_2031_DISABLE, MODE_2031_ENABLE]);
		assert.equal(root2.tui.terminalColorSchemeListeners?.size, 0);
		assert.equal(root3.tui.terminalColorSchemeListeners?.size, 1);

		// Repeat rebind calls: strictly idempotent
		themeController.rebindTui();
		themeController.rebindTui();
		themeController.rebindTui();
		assert.equal(root3.tui.terminalColorSchemeListeners?.size, 1);

		// Single scheme notification triggers exactly 1 query burst
		const markScheme = terminal.writes.length;
		terminal.injectInput(SCHEME_REPORT_LIGHT);
		await flushMicrotasks();
		const burstCommands = extractProtocolCommands(terminal.getWritesSince(markScheme));
		const da1Count = burstCommands.filter((c) => c === DA1_QUERY).length;
		assert.equal(da1Count, 1);
		assert.equal(burstCommands.length, 19);

		return {
			name: "roundtrip_rebind_idempotency",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				switches: ["fullscreen", "regular"],
				extraRebindCalls: 3,
			},
			expected: {
				sw1Commands,
				sw2Commands,
				burstCommands,
				finalListenerCount: 1,
				queryBurstsEmitted: 1,
				idempotent: true,
			},
		};
	}

	// ── Scenario 6: Malformed and unsupported control sequences ───────────────
	async function runScenario6() {
		resetGlobalTheme();
		const terminal = new InstrumentedTerminal(80, 24);
		const harness = buildControlledContext(terminal, "regular");
		const { themeController, getRenderer, getInstrumented } = harness;

		getRenderer().start();
		themeController.applyFromSettings();
		const root = getInstrumented();
		assert.equal(root.tui.pendingTerminalColorQueries?.length, 1);

		// 1. Invalid OSC 10 rgb format
		terminal.injectInput("\x1b]10;rgb:invalid\x07");
		await flushMicrotasks();

		// 2. Unsupported OSC 12
		terminal.injectInput("\x1b]12;rgb:00/00/00\x07");
		await flushMicrotasks();

		// 3. Unrelated CSI sequence
		terminal.injectInput("\x1b[24;80R");
		await flushMicrotasks();

		// The query is still pending because invalid responses and unrelated CSI did not complete it:
		assert.equal(root.tui.pendingTerminalColorQueries?.length, 1);

		// 4. Valid background and valid DA1
		const VALID_BG = "#654321";
		terminal.injectInput(makeOsc11Reply(VALID_BG));
		terminal.injectInput(DA1_REPLY);
		await flushMicrotasks();
		await (themeController as any).waitForTerminalColors();

		assert.equal(root.tui.pendingTerminalColorQueries?.length, 0);
		const colors = (themeController as any).terminalColors;
		assert.deepEqual(colors, {
			foreground: undefined,
			background: { r: 101, g: 67, b: 33 },
			palette: undefined,
		});

		return {
			name: "malformed_and_unsupported_controls",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				invalidOsc: "\\x1b]10;rgb:invalid\\x07",
				unsupportedOsc12: "\\x1b]12;rgb:00/00/00\\x07",
				unrelatedCsi: "\\x1b[24;80R",
				validBg: VALID_BG,
			},
			expected: {
				invalidControlsIgnored: true,
				queryRemainedPendingBeforeValidDa1: true,
				settledColors: colors,
			},
		};
	}

	const s1 = await runScenario1();
	const s2 = await runScenario2();
	const s3 = await runScenario3();
	const s4 = await runScenario4();
	const s5 = await runScenario5();
	const s6 = await runScenario6();

	return {
		sourceEndpoints: [
			"packages/tui/src/tui.ts:Tui.setTerminalColorSchemeNotifications,Tui.consumeTerminalColorResponse,Tui.consumeTerminalColorSchemeReport,Tui.onTerminalColorSchemeChange",
			"packages/coding-agent/src/modes/interactive/theme/theme-controller.ts:InteractiveThemeController.applyFromSettings,InteractiveThemeController.rebindTui",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.switchTuiMode",
		],
		scenarios: [s1, s2, s3, s4, s5, s6],
	};
}
