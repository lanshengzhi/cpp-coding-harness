#!/usr/bin/env tsx
/**
 * Capture the `audit-1209` frozen observations: route batch B1 (issue #1209,
 * root TUI lifecycle, root mutation and diagnostic callbacks) recaptured at
 * the frozen pi v1.3.0 checkout.
 *
 * Artifacts:
 * - screen-state.json: the common-root lifecycle and the styled frame with a
 *   focused cursor, driving the frozen TuiMainScreen through pi's own
 *   VirtualTerminal with a public cell-attribute observer (the same observer
 *   shape the historical pi-v1.0.4 bundle records).
 * - product-cursor.json: the frozen product cursor-switch family (reused
 *   capture module; the historical bundle recorded the same scenarios).
 * - root-extras.json: the root obligations the historical family never
 *   captured at v1.3.0: key-release filtering, input listeners (consume and
 *   transform), the debug and color-scheme callbacks with a write log (so a
 *   callback that never runs separates from one that runs late), render
 *   controls (requestRender coalescing, full redraws, clear-on-shrink),
 *   the overlay lifecycle (options, bounds, focus order, hide/hidden), the
 *   terminal color query with a late reply, the capability predicates, and
 *   compositeTuiLine.
 *
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1209/capture-1209.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1209";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT
	? path.resolve(process.env.PI_CHECKOUT)
	: path.resolve(repositoryRoot, "../pi");

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: {
			...process.env,
			NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
			TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
		},
	});
	if (result.error) throw new Error(`cannot re-exec ${tsx}: ${result.error}`);
	process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
	throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
	throw new Error(
		`unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
			Object.keys(registry.baselines).sort().join(", "),
	);
}
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report?.getReport() as any)?.header?.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
	if (name !== baselineName && entry.bundles && family in entry.bundles) {
		throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
	}
}

let tsxVersion = "unknown";
try {
	tsxVersion = execFileSync(path.join(piCheckout, "node_modules", ".bin", "tsx"), ["--version"], {
		encoding: "utf8",
	}).trim();
} catch {
	// Provenance, not a capture precondition.
}
const environment = {
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
const { Terminal: XtermTerminal } = requirePi("@xterm/headless");
const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
const { CURSOR_MARKER, isFocusable, isViewportTUI, compositeTuiLine } = await import(source("src/tui.ts"));
const { isKeyRelease, matchesKey } = await import(source("src/keys.ts"));
const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
const captureDir = path.join(repositoryRoot, "fixtures/pi-tui/capture");
const { captureProductCursor } = await import(pathToFileURL(path.join(captureDir, "product-cursor-scenarios.mts")).href);

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

/// The frozen VirtualTerminal as the renderer adapter, feeding a second public
/// xterm instance so full cell attributes are observed without private pi
/// hooks (the same observer the historical named capture built). Every write
/// and every observed callback lands in `log` with ordering, so a callback
/// that never runs separates from one that runs late.
class CaptureTerminal extends VirtualTerminal {
	readonly emulator: any;
	cursorVisible = true;
	readonly writes: string[] = [];
	readonly log: Array<{ source: string; data: string }> = [];
	constructor(columns: number, rows: number) {
		super(columns, rows);
		this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
	}
	override write(data: string) {
		if ([...data.matchAll(/\x1b\]8;[^;]*;([^\x07\x1b]*)/g)].some((match) => match[1]))
			throw new Error("OSC 8 cell capture needs link-aware evidence before adding linked scenarios");
		this.writes.push(data);
		this.log.push({ source: "write", data });
		super.write(data);
		this.emulator.write(data);
	}
	override resize(columns: number, rows: number) {
		super.resize(columns, rows);
		this.emulator.resize(columns, rows);
	}
	override hideCursor() {
		super.hideCursor();
		this.cursorVisible = false;
	}
	override showCursor() {
		super.showCursor();
		this.cursorVisible = true;
	}
	record(sourceName: string, data: string) {
		this.log.push({ source: sourceName, data });
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
			throw new Error("cell observer diverged from frozen VirtualTerminal; extend the public terminal adapter");
		}
		const cells = Array.from({ length: this.rows }, (_, row) =>
			Array.from({ length: this.columns }, (_, col) => {
				const cell = buffer.getLine(buffer.viewportY + row).getCell(col);
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
				if (cell.isOverline()) throw new Error("overline cells need explicit C++ representation evidence");
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

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// ---------------------------------------------------------------------------
// screen-state.json: common-root lifecycle + styled frame with focused cursor.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	// Styled frame and focused cursor: a styled two-row child that draws the
	// CURSOR_MARKER at a fixed cell while focused; the second frame changes the
	// style so a static frame cannot stand in for the render.
	{
		const inputs = {
			frames: [
				["\x1b[1;31mred\x1b[0m", "tail"],
				["\x1b[4;32mgo\x1b[0m", "tail"],
			],
			cursor: { column: 2, row: 1 },
			showHardwareCursor: false,
		};
		const terminal = new CaptureTerminal(16, 6);
		const renderer = new TuiMainScreen(terminal, inputs.showHardwareCursor);
		let lines = inputs.frames[0];
		const child = {
			focused: false,
			render: () =>
				lines.map((line, row) =>
					row === inputs.cursor.row
						? line.slice(0, inputs.cursor.column) + CURSOR_MARKER + line.slice(inputs.cursor.column)
						: line,
				),
			invalidate: () => {},
		};
		renderer.addChild(child);
		renderer.start();
		renderer.setFocus(child);
		renderer.renderNow();
		const initial = await terminal.snapshot();
		lines = inputs.frames[1];
		renderer.renderNow();
		const changed = await terminal.snapshot();
		renderer.stop();
		terminal.emulator.dispose();
		scenarios.push({
			name: "styled-frame-and-focused-cursor",
			dimensions: { columns: 16, rows: 6 },
			inputs,
			expected: { initial, changed },
		});
	}

	// Common-root lifecycle: start, focus, input, removal, clear and stop on the
	// regular root; the events log records focus and input callbacks.
	{
		const inputs = { first: "input", second: "tail", input: "x", cursor: { column: 2, row: 0 } };
		const terminal = new CaptureTerminal(16, 6);
		const root = new TuiMainScreen(terminal, false);
		const events: string[] = [];
		let focused = false;
		const child = {
			get focused() {
				return focused;
			},
			set focused(value: boolean) {
				focused = value;
				events.push(`focus:${value}`);
			},
			render: () => [
				focused
					? inputs.first.slice(0, inputs.cursor.column) + CURSOR_MARKER + inputs.first.slice(inputs.cursor.column)
					: inputs.first,
			],
			invalidate: () => {},
			handleInput: (data: string) => events.push(`input:${data}`),
		};
		const tail = { render: () => [inputs.second], invalidate: () => {} };
		root.addChild(child);
		root.addChild(tail);
		root.start();
		root.setFocus(child);
		root.renderNow();
		const initial = await terminal.snapshot();
		terminal.sendInput(inputs.input);
		root.removeChild(tail);
		root.renderNow();
		const removed = await terminal.snapshot();
		root.setFocus(null);
		root.clear();
		root.renderNow();
		const cleared = await terminal.snapshot();
		const beforeStop = { children: root.children.length, mode: root.mode, stopped: false };
		root.stop();
		const stopped = await terminal.snapshot();
		terminal.sendInput("after-stop");
		terminal.emulator.dispose();
		scenarios.push({
			name: "common-root-lifecycle",
			dimensions: { columns: 16, rows: 6 },
			inputs,
			expected: { initial, removed, cleared, stopped, events, beforeStop },
		});
	}

	artifacts.push({
		path: "screen-state.json",
		sourceEndpoints: [
			"packages/tui/src/tui-main-screen.ts:TuiMainScreen.constructor,renderNow,doRender",
			"packages/tui/src/tui.ts:TuiBase.start,stop,setFocus,Container.addChild,removeChild,clear,CURSOR_MARKER",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
			"packages/tui/package.json:@xterm/headless",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// product-cursor.json: the frozen product cursor-switch family.
// ---------------------------------------------------------------------------
{
	const observation = await captureProductCursor(piCheckout);
	artifacts.push({
		path: "product-cursor.json",
		sourceEndpoints: observation.sourceEndpoints,
		scenarios: observation.scenarios,
	});
}

// ---------------------------------------------------------------------------
// root-extras.json: the root obligations not covered by the families above.
// ---------------------------------------------------------------------------
{
	const scenarios: any[] = [];

	// Key-release filtering: a component without wantsKeyRelease sees the press
	// but not the release; opting in delivers both (pi `Component.wantsKeyRelease`).
	{
		const inputs = {
			press: "\x1b[97;5u",
			release: "\x1b[97;5:3u",
			typed: "a",
		};
		if (!matchesKey(inputs.press, "ctrl+a")) throw new Error("press sequence is not ctrl+a");
		if (!isKeyRelease(inputs.release)) throw new Error("release sequence is not a key release");
		const received: Record<string, string[]> = {};
		const makeComponent = (name: string, wantsKeyRelease: boolean) => {
			const component: any = {
				focused: false,
				render: () => [name],
				invalidate: () => {},
				handleInput: (data: string) => received[name].push(data),
			};
			if (wantsKeyRelease) component.wantsKeyRelease = true;
			return component;
		};
		received.filtering = [];
		received.optedIn = [];
		const terminal = new CaptureTerminal(20, 2);
		const root = new TuiMainScreen(terminal, false);
		const filtering = makeComponent("filtering", false);
		const optedIn = makeComponent("optedIn", true);
		root.addChild(filtering);
		root.addChild(optedIn);
		root.start();
		root.renderNow();
		root.setFocus(filtering);
		terminal.sendInput(inputs.press);
		terminal.sendInput(inputs.release);
		terminal.sendInput(inputs.typed);
		const filteringPhase = [...received.filtering];
		root.setFocus(optedIn);
		terminal.sendInput(inputs.press);
		terminal.sendInput(inputs.release);
		const optedInPhase = [...received.optedIn];
		root.stop();
		terminal.emulator.dispose();
		scenarios.push({
			name: "root-key-release-filtering",
			dimensions: { columns: 20, rows: 2 },
			inputs,
			expected: { filteringPhase, optedInPhase },
		});
	}

	// Input listeners: a listener may transform the data (returning a new
	// string) or consume it entirely; removal stops the listener (pi
	// `TuiBase.addInputListener,removeInputListener`).
	{
		const terminal = new CaptureTerminal(20, 2);
		const root = new TuiMainScreen(terminal, false);
		const received: string[] = [];
		const invocations: string[] = [];
		const focused: any = {
			focused: false,
			render: () => ["listener-target"],
			invalidate: () => {},
			handleInput: (data: string) => received.push(data),
		};
		root.addChild(focused);
		root.start();
		root.renderNow();
		root.setFocus(focused);

		const transformListener: any = (data: string) => {
			invocations.push(`transform:${data}`);
			return { data: `${data}->t` };
		};
		const consumeListener: any = (data: string) => {
			invocations.push(`consume:${data}`);
			return { consume: true };
		};
		const unregisterTransform = root.addInputListener(transformListener);
		root.addInputListener(consumeListener);
		terminal.sendInput("one");
		const consumedPhase = { invocations: [...invocations], received: [...received] };
		root.removeInputListener(consumeListener);
		terminal.sendInput("two");
		const afterRemove = { invocations: [...invocations], received: [...received] };
		unregisterTransform();
		terminal.sendInput("three");
		const afterUnregister = { invocations: [...invocations], received: [...received] };
		root.stop();
		terminal.emulator.dispose();
		const inputs = { first: "one", second: "two", third: "three" };
		scenarios.push({
			name: "root-input-listeners",
			dimensions: { columns: 20, rows: 2 },
			inputs,
			expected: { consumedPhase, afterRemove, afterUnregister },
		});
	}

	// Diagnostic callbacks with a write log: the global debug key runs onDebug
	// before any focused component sees the data; without onDebug the same
	// sequence is ordinary input. Color-scheme notifications write the 2031
	// mode and deliver parsed 997 reports to listeners.
	{
		const terminal = new CaptureTerminal(20, 2);
		const root = new TuiMainScreen(terminal, false);
		const debugKey = "\x1b[68;6u";
		if (!matchesKey(debugKey, "shift+ctrl+d")) throw new Error("debug sequence is not shift+ctrl+d");
		const callbacks: string[] = [];
		root.onDebug = () => {
			callbacks.push("onDebug");
			terminal.record("callback:onDebug", "");
		};
		const received: string[] = [];
		const focused: any = {
			focused: false,
			render: () => ["debug-target"],
			invalidate: () => {},
			handleInput: (data: string) => {
				received.push(data);
				terminal.record("callback:focusedInput", data);
			},
		};
		root.addChild(focused);
		const schemeReports: string[] = [];
		root.onTerminalColorSchemeChange((scheme: string) => {
			schemeReports.push(scheme);
			terminal.record("callback:scheme", scheme);
		});
		// Enabled before start: start() must arm the notification mode.
		root.setTerminalColorSchemeNotifications(true);
		root.start();
		root.renderNow();
		root.setFocus(focused);
		terminal.sendInput(debugKey);
		const withCallback = { callbacks: [...callbacks], received: [...received] };
		root.onDebug = undefined;
		terminal.sendInput(debugKey);
		const withoutCallback = { callbacks: [...callbacks], received: [...received] };
		terminal.sendInput("\x1b[?997;1n");
		terminal.sendInput("\x1b[?997;2n");
		terminal.sendInput("\x1b[?997;3n");
		terminal.sendInput("not-a-report");
		const log = [...terminal.log];
		root.stop();
		terminal.emulator.dispose();
		scenarios.push({
			name: "root-debug-callbacks",
			dimensions: { columns: 20, rows: 2 },
			inputs: {
				debugKey,
				schemeReports: ["\x1b[?997;1n", "\x1b[?997;2n", "\x1b[?997;3n", "not-a-report"],
				notificationsEnabledBeforeStart: true,
			},
			expected: { withCallback, withoutCallback, schemeReports, log },
		});
	}

	// Render controls: requestRender coalesces pending renders, renderNow(true)
	// forces a full redraw, and clear-on-shrink decides whether stale rows stay
	// after the terminal shrinks (pi `TuiBase.requestRender`,
	// `TuiMainScreen.doRender`).
	{
		const inputs = { lines: ["one", "two", "three", "four"], shrinkTo: { columns: 20, rows: 2 } };
		const exercise = async (clearOnShrink: boolean) => {
			const terminal = new CaptureTerminal(20, 4);
			const root = new TuiMainScreen(terminal, false);
			root.setClearOnShrink(clearOnShrink);
			const child = { render: () => inputs.lines, invalidate: () => {} };
			root.addChild(child);
			root.start();
			root.renderNow();
			const redrawsBefore = root.fullRedraws;
			root.requestRender();
			root.requestRender();
			root.requestRender();
			await delay(40);
			const redrawsAfterCoalesce = root.fullRedraws;
			root.renderNow(true);
			const redrawsAfterForce = root.fullRedraws;
			terminal.resize(inputs.shrinkTo.columns, inputs.shrinkTo.rows);
			await delay(40);
			const shrunk = await terminal.snapshot();
			root.stop();
			terminal.emulator.dispose();
			return { redrawsBefore, redrawsAfterCoalesce, redrawsAfterForce, shrunk };
		};
		const keep = await exercise(false);
		const clear = await exercise(true);
		scenarios.push({
			name: "root-render-controls",
			dimensions: { columns: 20, rows: 4 },
			inputs,
			expected: { keep, clear },
		});
	}

	// Overlay lifecycle: options drive bounds; handles report focus/visibility;
	// hide removes; non-capturing overlays skip focus (pi `TuiBase.showOverlay`).
	{
		const terminal = new CaptureTerminal(30, 8);
		const root = new TuiMainScreen(terminal, false);
		const base = { render: () => ["base"], invalidate: () => {} };
		root.addChild(base);
		root.start();
		root.renderNow();
		const visibleCalls: Array<{ columns: number; rows: number }> = [];
		const overlayContent: any = {
			focused: false,
			render: () => ["overlay-line-0", "overlay-line-1"],
			invalidate: () => {},
		};
		const options = {
			anchor: "top-left",
			col: 2,
			row: 1,
			width: 12,
			minWidth: 8,
			maxHeight: 1,
			offsetX: 1,
			offsetY: 1,
			margin: { top: 1, bottom: 0, left: 1, right: 0 },
			visible: (columns: number, rows: number) => {
				visibleCalls.push({ columns, rows });
				return true;
			},
		};
		const handle = root.showOverlay(overlayContent, options);
		const state = {
			hasOverlay: root.hasOverlay(),
			isFocused: handle.isFocused(),
			isHidden: handle.isHidden(),
		};
		root.renderNow();
		const bounds = handle.getBounds() ?? null;
		const visibleAfterRender = [...visibleCalls];
		const frame = await terminal.snapshot();
		handle.setHidden(true);
		const hiddenState = { isHidden: handle.isHidden(), isFocused: handle.isFocused(), hasOverlay: root.hasOverlay() };
		root.renderNow();
		const hiddenFrame = await terminal.snapshot();
		handle.setHidden(false);
		const reshownState = { isHidden: handle.isHidden(), isFocused: handle.isFocused() };
		root.renderNow();
		const nonCapturing: any = { focused: false, render: () => ["pass-through"], invalidate: () => {} };
		const second = root.showOverlay(nonCapturing, { nonCapturing: true });
		root.renderNow();
		const nonCapturingState = { hasOverlay: root.hasOverlay(), secondFocused: second.isFocused() };
		second.hide();
		handle.hide();
		const afterHide = { hasOverlay: root.hasOverlay() };
		root.stop();
		terminal.emulator.dispose();
		scenarios.push({
			name: "root-overlay-lifecycle",
			dimensions: { columns: 30, rows: 8 },
			inputs: { options },
			expected: { state, bounds, visibleAfterRender, frame, hiddenState, hiddenFrame, reshownState, nonCapturingState, afterHide },
		});
	}

	// Terminal color query: a reply that arrives after the timeout resolves the
	// promise empty and is delivered to onLateReply (pi
	// `TuiBase.queryTerminalColors`).
	{
		const terminal = new CaptureTerminal(20, 2);
		const root = new TuiMainScreen(terminal, false);
		root.start();
		root.renderNow();
		const late: any[] = [];
		const query = root.queryTerminalColors({
			timeoutMs: 40,
			onLateReply: (colors: any) => {
				late.push({
					foreground: colors.foreground ?? null,
					background: colors.background ?? null,
					paletteSet: colors.palette !== undefined,
				});
				terminal.record("callback:lateReply", JSON.stringify(colors));
			},
		});
		const timedOut = await query;
		const afterTimeout = {
			foreground: timedOut.foreground ?? null,
			background: timedOut.background ?? null,
			paletteSet: timedOut.palette !== undefined,
			lateSoFar: [...late],
		};
		terminal.sendInput("\x1b]10;#ff0000\x07");
		terminal.sendInput("\x1b]11;#00ff00\x07");
		for (let index = 0; index < 16; ++index) {
			const channel = (index * 16).toString(16).padStart(4, "0");
			terminal.sendInput(`\x1b]4;${index};rgb:${channel}/${channel}/${channel}\x07`);
		}
		terminal.sendInput("\x1b[?1;2c");
		const log = [...terminal.log];
		root.stop();
		terminal.emulator.dispose();
		scenarios.push({
			name: "root-color-query-late-reply",
			dimensions: { columns: 20, rows: 2 },
			inputs: { timeoutMs: 40, replies: "sixteen OSC 4 replies plus OSC 10/11 and DA1, all after the timeout" },
			expected: { queryWrites: 1, afterTimeout, late, log },
		});
	}

	// Capability predicates and compositeTuiLine: the exported helpers observe
	// component capability and overlay line composition.
	{
		const focusable: any = { focused: false, render: () => [], invalidate: () => {} };
		const plain: any = { render: () => [], invalidate: () => {} };
		const main = new TuiMainScreen(new VirtualTerminal(20, 2), false);
		const alt = new TuiAltScreen(new VirtualTerminal(20, 2));
		const cases = [
			{ base: "hello world", overlay: "OVER", startCol: 6, overlayWidth: 4, totalWidth: 11 },
			{ base: "\x1b[31mred\x1b[0m tail", overlay: "XY", startCol: 0, overlayWidth: 6, totalWidth: 9 },
			{ base: "abcdef", overlay: "1234567890", startCol: 2, overlayWidth: 4, totalWidth: 8 },
		];
		const composed = cases.map((entry) => ({
			...entry,
			result: compositeTuiLine(entry.base, entry.overlay, entry.startCol, entry.overlayWidth, entry.totalWidth),
		}));
		scenarios.push({
			name: "capability-predicates-and-line-composition",
			dimensions: { columns: 20, rows: 2 },
			inputs: { cases },
			expected: {
				isFocusableFocusable: isFocusable(focusable),
				isFocusablePlain: isFocusable(plain),
				isFocusableNull: isFocusable(null),
				isViewportMain: isViewportTUI(main as any),
				isViewportAlt: isViewportTUI(alt as any),
				composed,
			},
		});
	}

	artifacts.push({
		path: "root-extras.json",
		sourceEndpoints: [
			"packages/tui/src/tui.ts:TuiBase.handleTerminalInput,addInputListener,removeInputListener,onDebug,onTerminalColorSchemeChange,setTerminalColorSchemeNotifications,queryTerminalColors,requestRender,renderNow,setClearOnShrink,getClearOnShrink,showOverlay,hideOverlay,hasOverlay,isFocusable,isViewportTUI,compositeTuiLine,Component.wantsKeyRelease",
			"packages/tui/src/tui-main-screen.ts:TuiMainScreen.doRender,fullRedraws",
			"packages/tui/src/terminal-colors.ts:parseTerminalColorSchemeReport",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
		],
		scenarios,
	});
}

// ---------------------------------------------------------------------------
// Write the artifacts and the manifest only after every scenario succeeded.
// ---------------------------------------------------------------------------
const manifestArtifacts = [];
const artifactBytes = new Map<string, Buffer>();
for (const artifact of artifacts) {
	const document = {
		schema: "pike/pi-v1.3.0-audit-evidence/1",
		baseline: baselineName,
		revision: baseline.revision,
		capturedAt,
		environment,
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	};
	const bytes = Buffer.from(`${JSON.stringify(document, null, 2)}\n`);
	artifactBytes.set(artifact.path, bytes);
	manifestArtifacts.push({
		path: artifact.path,
		family,
		bytes: bytes.length,
		sha256: sha256(bytes),
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	});
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const scenarioSources = ["product-cursor-scenarios.mts"].map((name) => {
	const relative = `fixtures/pi-tui/capture/${name}`;
	return { path: relative, sha256: sha256(readFileSync(path.join(repositoryRoot, relative))) };
});
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1209",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }, ...scenarioSources],
	},
	artifacts: manifestArtifacts,
};

mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ` +
		`${manifestArtifacts.reduce((total, item) => total + item.scenarios.length, 0)} scenarios) ` +
		`for ${baselineName} (${baseline.revision})`,
);
