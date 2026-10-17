import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { registerHooks } from "node:module";

/** Actual frozen controller/root observations. Instrumentation delegates every call. */
export async function captureProductThemeRebindLifecycle(piCheckout: string) {
	const directories: Record<string, string> = {
		"pi-tui": "tui", "pi-ai": "ai", "pi-agent-core": "agent", "pi-coding-agent": "coding-agent",
		"pi-protocol": "protocol", "pi-session-backends": "session-backends", "pi-mcp": "mcp",
		"pi-client": "client", "pi-chord": "chord", "pi-durable": "durable", "pi-env": "env", "pi-telemetry": "telemetry",
	};
	registerHooks({
		resolve(specifier, context, next) {
			if (specifier.startsWith("@earendil-works/")) {
				const [name, ...parts] = specifier.slice("@earendil-works/".length).split("/");
				if (directories[name]) {
					const relative = parts.length ? parts.join("/") : "index.ts";
					return { url: pathToFileURL(path.join(piCheckout, "packages", directories[name], "src",
						relative.endsWith(".ts") ? relative : `${relative}.ts`)).href, shortCircuit: true };
				}
			}
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return next(specifier, context);
		},
		load(url, context, next) {
			// The capture uses no Azure provider; avoid loading its unrelated generated catalog.
			if (url.endsWith("/providers/data/azure.json")) return { format: "json", source: "{}", shortCircuit: true };
			return next(url, context);
		},
	});
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages", relative)).href;
	const { InteractiveThemeController } = await import(source("coding-agent/src/modes/interactive/theme/theme-controller.ts"));
	const { setTerminalColors, setTerminalColorScheme, initTheme } = await import(source("coding-agent/src/modes/interactive/theme/theme.ts"));
	const { createInteractiveTui, createInteractiveTuiReference } = await import(source("coding-agent/src/modes/interactive/tui-renderer.ts"));
	const { InteractiveMode } = await import(source("coding-agent/src/modes/interactive/interactive-mode.ts"));
	const { Text } = await import(source("tui/src/index.ts"));
	const { VirtualTerminal } = await import(source("tui/test/virtual-terminal.ts"));
	const clone = (value: any) => value === undefined ? null : JSON.parse(JSON.stringify(value));
	const flush = async () => { await new Promise<void>((resolve) => process.nextTick(resolve)); };
	const bounded = async (completion: Promise<any>) => {
		let timer: ReturnType<typeof setTimeout>;
		try {
			return await Promise.race([completion, new Promise((_, reject) => {
				timer = setTimeout(() => reject(new Error("actual query completion exceeded 1500ms")), 1500);
			})]);
		} finally { clearTimeout(timer!); }
	};
	const queryCommands = () => ["\x1b]10;?\x07", "\x1b]11;?\x07",
		...Array.from({ length: 16 }, (_, i) => `\x1b]4;${i};?\x07`), "\x1b[c"];
	const commands = (writes: string[]) => writes.join("").match(/\x1b\[\?2031[hl]|\x1b\](?:10|11);\?\x07|\x1b\]4;\d+;\?\x07|\x1b\[c/g) ?? [];

	function harness() {
		setTerminalColors({}); setTerminalColorScheme(undefined); initTheme("dark");
		const events: any[] = [];
		const roots = new Map<any, string>();
		let activeOwner = "none";
		let changed = 0;
		const record = (kind: string, owner: string, data: any = {}) => {
			events.push(clone({ order: events.length + 1, kind, owner, ...data }));
		};
		class Terminal extends VirtualTerminal {
			writes: string[] = [];
			handler: ((data: string) => void) | undefined;
			write(data: string) { this.writes.push(data); super.write(data); }
			start(input: (data: string) => void, resize: () => void) { this.handler = input; super.start(input, resize); }
			stop() { this.handler = undefined; super.stop(); }
			inject(data: string) {
				record("input-dispatch", activeOwner, { bytes: data, handlerPresent: !!this.handler });
				assert.ok(this.handler); this.handler(data);
			}
		}
		const terminal = new Terminal(80, 24);
		let renderer: any;
		let controller: any;
		const wrapRoot = (root: any, owner: string) => {
			roots.set(root, owner);
			for (const method of ["invalidate", "requestRender", "start", "stop"]) {
				const original = root[method].bind(root);
				root[method] = (...args: any[]) => {
					record(method, owner, { queueDepth: root.pendingTerminalColorQueries.length });
					if (method === "start") activeOwner = owner;
					const result = original(...args);
					if (method === "stop") activeOwner = "none";
					return result;
				};
			}
			const query = root.queryTerminalColors.bind(root);
			root.queryTerminalColors = (options: any) => {
				record("query-start", owner, { queueDepth: root.pendingTerminalColorQueries.length });
				const result = query({ ...options, onLateReply: (colors: any) => {
					record("query-late", owner, { colors, queueDepth: root.pendingTerminalColorQueries.length });
					options.onLateReply?.(colors);
				} });
				record("query-pending", owner, { queueDepth: root.pendingTerminalColorQueries.length });
				return result.then((colors: any) => {
					record("query-complete", owner, { colors, queueDepth: root.pendingTerminalColorQueries.length });
					return colors;
				});
			};
			const listen = root.onTerminalColorSchemeChange.bind(root);
			root.onTerminalColorSchemeChange = (callback: any) => listen((scheme: any) => {
				record("scheme-callback", owner, { scheme, queueDepth: root.pendingTerminalColorQueries.length });
				return callback(scheme);
			});
		};
		const settingsManager = {
			getThemeSetting: () => undefined, getTheme: () => "system", getTerminalTheme: () => "dark",
			getFullscreenCopyOnSelect: () => false, getFullscreenWheelScrollLines: () => 3,
			getShowTerminalProgress: () => false, getShowHardwareCursor: () => false, getClearOnShrink: () => false,
		};
		const context: any = Object.assign(Object.create(InteractiveMode.prototype), {
			runtimeHost: { session: { settingsManager, isStreaming: false, isCompacting: false } },
			options: { tuiMode: "regular" }, fullscreenLayoutRoot: new Text("layout root", 0, 0),
			rebindExtensionTerminalInputListeners: () => {}, mainScreenRenderState: undefined, onRightClickPaste: undefined,
			mountInteractiveTui(root: any, components: any[]) {
				for (const component of components) root.addChild(component);
				if (typeof root.setLayoutRoot === "function") root.setLayoutRoot(context.fullscreenLayoutRoot);
			},
		});
		Object.defineProperty(context, "renderer", {
			get: () => renderer,
			set: (next) => {
				wrapRoot(next, renderer ? "replacement-fullscreen" : "initial-regular");
				renderer = next;
			},
		});
		context.renderer = createInteractiveTui({ tuiMode: "regular", terminal, showHardwareCursor: false,
			logDirectory: "/tmp", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 3 });
		context.ui = createInteractiveTuiReference(() => renderer);
		renderer.addChild(new Text("document line", 0, 0));
		controller = new InteractiveThemeController(context.ui, {
			getSettingsManager: () => settingsManager,
			showError: (error: string) => { throw new Error(error); },
			onChanged: () => { ++changed; record("changed", activeOwner); },
		});
		context.themeController = controller;
		const apply = controller.applyTerminalColors.bind(controller);
		controller.applyTerminalColors = (colors: any) => {
			record("controller-apply", activeOwner, { colors });
			apply(colors);
			record("controller-applied", activeOwner, { colors: controller.terminalColors });
		};
		const snapshot = () => clone({ activeOwner, terminalColors: controller.terminalColors ?? null, changed,
			roots: [...roots].map(([root, owner]) => ({ owner, stopped: root.stopped,
				queueDepth: root.pendingTerminalColorQueries.length, listenerCount: root.terminalColorSchemeListeners.size })) });
		return { context, terminal, controller, events, record, snapshot, root: () => renderer };
	}
	const originalColorfgbg = process.env.COLORFGBG;
	process.env.COLORFGBG = "0;15";
	try {
		const retired = harness();
		retired.root().start();
		retired.controller.applyFromSettings();
		const beforeSwitch = retired.snapshot();
		assert.equal(beforeSwitch.roots[0].queueDepth, 1);
		assert.equal(beforeSwitch.terminalColors, null);
		assert.equal(retired.events.filter((e) => e.kind === "query-complete").length, 0);
		const initialCommands = commands(retired.terminal.writes);
		assert.deepEqual(initialCommands, ["\x1b[?2031h", ...queryCommands()]);
		const switchMark = retired.terminal.writes.length;
		assert.equal(InteractiveMode.prototype.switchTuiMode.call(retired.context, "fullscreen"), true);
		const switchCommands = commands(retired.terminal.writes.slice(switchMark));
		assert.deepEqual(switchCommands, ["\x1b[?2031l", "\x1b[?2031h"]);
		const afterSwitch = retired.snapshot();
		assert.equal(afterSwitch.roots[0].queueDepth, 1);
		assert.equal(afterSwitch.roots[1].queueDepth, 0);
		const timerEventMark = retired.events.length;
		const timerWriteMark = retired.terminal.writes.length;
		// This awaits the actual retired query's promise and its controller apply.
		await bounded(retired.controller.waitForTerminalColors()); await flush();
		const afterRetiredCompletion = retired.snapshot();
		const timerEvents = clone(retired.events.slice(timerEventMark));
		const timerWrites = retired.terminal.writes.slice(timerWriteMark);
		assert.deepEqual(afterRetiredCompletion.terminalColors, {});
		assert.equal(afterRetiredCompletion.roots[0].queueDepth, 1);
		assert.equal(afterRetiredCompletion.roots[0].listenerCount, 0);
		assert.equal(afterRetiredCompletion.roots[1].listenerCount, 1);
		assert.equal(afterRetiredCompletion.changed - afterSwitch.changed, 1);
		assert.equal(timerEvents.filter((e: any) => e.kind === "invalidate" && e.owner === "replacement-fullscreen").length, 2);
		assert.equal(timerEvents.filter((e: any) => e.kind === "requestRender" && e.owner === "replacement-fullscreen").length, 1);
		assert.equal(timerEvents[0].kind, "query-complete");
		const activeMark = retired.terminal.writes.length;
		retired.terminal.inject("\x1b[?997;2n");
		const activeQueryCommands = commands(retired.terminal.writes.slice(activeMark));
		assert.deepEqual(activeQueryCommands, queryCommands());
		retired.terminal.inject("\x1b]10;#333333\x07");
		retired.terminal.inject("\x1b]11;#eeeeee\x07");
		retired.terminal.inject("\x1b[?1;2c");
		await bounded(retired.controller.waitForTerminalColors()); await flush();
		const afterActiveReply = retired.snapshot();
		assert.deepEqual(afterActiveReply.terminalColors, { foreground: { r: 51, g: 51, b: 51 }, background: { r: 238, g: 238, b: 238 } });
		const lifecycleEvents = clone(retired.events);
		retired.controller.dispose(); retired.root().stop();

		const palette = harness(); palette.root().start(); palette.controller.applyFromSettings();
		const values = Array.from({ length: 16 }, (_, i) => ({ r: i * 13 + 1, g: i * 11 + 2, b: i * 7 + 3 }));
		const hex = (value: number) => value.toString(16).padStart(2, "0");
		const colorReply = (index: number, rgb: any) => `\x1b]4;${index};#${hex(rgb.r)}${hex(rgb.g)}${hex(rgb.b)}\x07`;
		palette.terminal.inject("\x1b]10;#112233\x07");
		palette.terminal.inject("\x1b]11;#445566\x07");
		palette.terminal.inject(colorReply(0, values[0]));
		// Different second value for target0 must be ignored and cannot advance completion.
		palette.terminal.inject(colorReply(0, { r: 255, g: 255, b: 255 }));
		for (let i = 1; i < 15; ++i) palette.terminal.inject(colorReply(i, values[i]));
		await flush();
		const beforeLastPalette = palette.snapshot();
		assert.equal(beforeLastPalette.terminalColors, null);
		assert.equal(beforeLastPalette.roots[0].queueDepth, 1);
		palette.terminal.inject(colorReply(15, values[15]));
		await bounded(palette.controller.waitForTerminalColors()); await flush();
		const afterPaletteCompletion = palette.snapshot();
		assert.deepEqual(afterPaletteCompletion.terminalColors.palette, values);
		assert.equal(afterPaletteCompletion.roots[0].queueDepth, 1);
		palette.terminal.inject("\x1b[?1;2c"); await flush();
		const afterPaletteDa1 = palette.snapshot();
		assert.equal(afterPaletteDa1.roots[0].queueDepth, 0);
		assert.deepEqual(afterPaletteDa1.terminalColors, afterPaletteCompletion.terminalColors);
		palette.controller.applyFromSettings();
		const timeoutMark = palette.events.length;
		await bounded(palette.controller.waitForTerminalColors()); await flush();
		const afterObservedTimeout = palette.snapshot();
		const timeoutEvents = clone(palette.events.slice(timeoutMark));
		assert.ok(timeoutEvents.some((e: any) => e.kind === "query-complete" && Object.keys(e.colors).length === 0));
		assert.deepEqual(afterObservedTimeout.terminalColors, afterPaletteCompletion.terminalColors);
		const lateMark = palette.events.length;
		palette.terminal.inject("\x1b]11;#abcdef\x07");
		palette.terminal.inject("\x1b[?1;2c"); await flush();
		const afterLateReply = palette.snapshot();
		const lateEvents = clone(palette.events.slice(lateMark));
		assert.ok(lateEvents.some((e: any) => e.kind === "query-late"));
		assert.deepEqual(afterLateReply.terminalColors.background, { r: 171, g: 205, b: 239 });
		assert.deepEqual(afterLateReply.terminalColors.palette, values);
		const paletteEvents = clone(palette.events);
		palette.controller.dispose(); palette.root().stop();
		const controlledInputs = { COLORFGBG: "0;15", columns: 80, rows: 24, initialMode: "regular",
			envelopeEnvironment: "historical inherited baseline metadata; not a fresh host measurement",
			seams: ["real InteractiveMode.prototype switch with controlled session/settings", "real mount child and setLayoutRoot behavior", "no AgentSession/provider/network", "Azure catalog stub returns empty object", "retired root retained by observer and its pending timer", "physical bytes are observed at active terminal input-handler dispatch only"] };
		return {
			sourceEndpoints: ["packages/coding-agent/src/modes/interactive/theme/theme-controller.ts:requestTerminalColors,InteractiveThemeController.applyTerminalColors,waitForTerminalColors,rebindTui", "packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.switchTuiMode", "packages/tui/src/tui.ts:TuiBase.queryTerminalColors,consumeTerminalColorResponse"],
			scenarios: [
				{ name: "retired_query_completion_targets_replacement", dimensions: { columns: 80, rows: 24 }, inputs: controlledInputs,
					expected: { initialCommands, switchCommands, beforeSwitch, afterSwitch, afterRetiredCompletion,
						timerEvents, timerWrites, timerCommands: commands(timerWrites), activeQueryCommands, afterActiveReply, events: lifecycleEvents } },
				{ name: "same_root_palette_duplicate_and_observed_late_reply", dimensions: { columns: 80, rows: 24 },
					inputs: { ...controlledInputs, palette: values, duplicateTarget: 0, duplicateRgb: { r: 255, g: 255, b: 255 }, lateBackground: "#abcdef" },
					expected: { beforeLastPalette, afterPaletteCompletion, afterPaletteDa1, afterObservedTimeout, timeoutEvents, afterLateReply, lateEvents, events: paletteEvents } },
			],
		};
	} finally {
		if (originalColorfgbg === undefined) delete process.env.COLORFGBG;
		else process.env.COLORFGBG = originalColorfgbg;
	}
}
