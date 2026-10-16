#!/usr/bin/env tsx
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent frozen observations of hardware cursor visibility and IME positioning (#1037, spec #946).
///
/// Observes both frozen roots (TuiMainScreen and TuiAltScreen) with deterministic components across:
/// - default preference (false)
/// - enabled preference (true)
/// - disabled preference (explicit false)
/// - dynamic toggle (false -> true -> false)
/// - invalid / null cursor (hidden regardless of preference)
/// - nested components (Container + Box with padding)
/// - clipped / scrolled coordinates (off-screen cursor hides)
///
/// Cursor visibility is observed accurately across both MainScreen's explicit terminal methods
/// (showCursor / hideCursor) and AltScreen's embedded terminal escape bytes (\x1b[?25h / \x1b[?25l).
export async function captureHardwareCursor(piCheckout: string) {
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
	const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

	const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { Container, CURSOR_MARKER } = await import(source("src/tui.ts"));
	const { Box } = await import(source("src/components/box.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	class TestCursorComponent {
		focused = true;
		constructor(
			public lines: string[],
			public cursorCol?: number,
			public cursorRow: number = 0,
		) {}
		render(_width: number): string[] {
			const result = [...this.lines];
			if (this.focused && this.cursorCol !== undefined && this.cursorRow < result.length) {
				const line = result[this.cursorRow] ?? "";
				const before = line.slice(0, this.cursorCol);
				const after = line.slice(this.cursorCol);
				result[this.cursorRow] = before + CURSOR_MARKER + after;
			}
			return result;
		}
	}

	interface TerminalEvent {
		op: "write" | "showCursor" | "hideCursor";
		bytes?: string;
	}

	class HardwareCursorTerminal extends VirtualTerminal {
		readonly emulator: any;
		cursorVisible = true;
		events: TerminalEvent[] = [];
		writes: string[] = [];

		constructor(columns: number, rows: number) {
			super(columns, rows);
			this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
		}

		override write(data: string) {
			this.writes.push(data);
			this.events.push({ op: "write", bytes: data });
			const lastShow = data.lastIndexOf("\x1b[?25h");
			const lastHide = data.lastIndexOf("\x1b[?25l");
			if (lastShow !== -1 || lastHide !== -1) {
				this.cursorVisible = lastShow > lastHide;
			}
			super.write(data);
			this.emulator.write(data);
		}

		override hideCursor() {
			this.events.push({ op: "hideCursor" });
			this.cursorVisible = false;
			super.hideCursor();
		}

		override showCursor() {
			this.events.push({ op: "showCursor" });
			this.cursorVisible = true;
			super.showCursor();
		}

		override resize(columns: number, rows: number) {
			super.resize(columns, rows);
			this.emulator.resize(columns, rows);
		}

		takeEvents(): TerminalEvent[] {
			const result = [...this.events];
			this.events.length = 0;
			return result;
		}

		takeWrites(): string[] {
			const result = [...this.writes];
			this.writes.length = 0;
			return result;
		}

		async snapshot() {
			await this.flush();
			await new Promise<void>((resolve) => this.emulator.write("", () => resolve()));
			const buffer = this.emulator.buffer.active;
			const screen = this.getViewport();
			const observedScreen = Array.from({ length: this.rows }, (_, row) =>
				buffer.getLine(buffer.viewportY + row)?.translateToString(true) ?? "",
			);
			const observedCursor = this.getCursorPosition();
			if (
				JSON.stringify(screen) !== JSON.stringify(observedScreen) ||
				observedCursor.x !== buffer.cursorX ||
				observedCursor.y !== buffer.cursorY
			) {
				throw new Error("xterm emulator diverged from frozen VirtualTerminal");
			}
			return {
				screen,
				cursor: { col: observedCursor.x, row: observedCursor.y },
				cursorVisible: this.cursorVisible,
			};
		}
	}

	const sourceEndpoints = [
		"packages/tui/src/tui.ts:getShowHardwareCursor,setShowHardwareCursor,extractCursorPosition,hideTerminalCursor",
		"packages/tui/src/tui-main-screen.ts:positionHardwareCursor,render",
		"packages/tui/src/tui-alt-screen.ts:constructor,render",
	];

	const scenarios: any[] = [];

	// 1. main-screen-default-hidden
	{
		const term = new HardwareCursorTerminal(20, 5);
		const tui = new TuiMainScreen(term);
		const comp = new TestCursorComponent(["test text"], 4, 0);
		tui.addChild(comp);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();
		tui.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		tui.stop();
		scenarios.push({
			name: "main-screen-default-hidden",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "MainScreen", preference: "default", cursorCol: 4, cursorRow: 0 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: tui.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 2. main-screen-enabled-visible
	{
		const term = new HardwareCursorTerminal(20, 5);
		const tui = new TuiMainScreen(term, true);
		const comp = new TestCursorComponent(["test text"], 4, 0);
		tui.addChild(comp);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();
		tui.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		tui.stop();
		scenarios.push({
			name: "main-screen-enabled-visible",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "MainScreen", preference: "enabled", cursorCol: 4, cursorRow: 0 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: tui.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 3. main-screen-disabled-explicit
	{
		const term = new HardwareCursorTerminal(20, 5);
		const tui = new TuiMainScreen(term, false);
		const comp = new TestCursorComponent(["test text"], 4, 0);
		tui.addChild(comp);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();
		tui.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		tui.stop();
		scenarios.push({
			name: "main-screen-disabled-explicit",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "MainScreen", preference: "disabled", cursorCol: 4, cursorRow: 0 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: tui.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 4. main-screen-toggle
	{
		const term = new HardwareCursorTerminal(20, 5);
		const tui = new TuiMainScreen(term, false);
		const comp = new TestCursorComponent(["test text"], 4, 0);
		tui.addChild(comp);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();

		tui.renderNow();
		const snap1 = await term.snapshot();
		const events1 = term.takeEvents();
		const writes1 = term.takeWrites();

		tui.setShowHardwareCursor(true);
		tui.renderNow();
		const snap2 = await term.snapshot();
		const events2 = term.takeEvents();
		const writes2 = term.takeWrites();

		tui.setShowHardwareCursor(false);
		tui.renderNow();
		const snap3 = await term.snapshot();
		const events3 = term.takeEvents();
		const writes3 = term.takeWrites();

		tui.stop();
		scenarios.push({
			name: "main-screen-toggle",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "MainScreen", transitions: ["default-false", "toggle-true", "toggle-false"] },
			expected: {
				steps: [
					{
						action: "default-false",
						cursor: snap1.cursor,
						cursorVisible: snap1.cursorVisible,
						showHardwareCursor: false,
						screen: snap1.screen,
						terminalEvents: events1,
						rawWrites: writes1,
					},
					{
						action: "toggle-true",
						cursor: snap2.cursor,
						cursorVisible: snap2.cursorVisible,
						showHardwareCursor: true,
						screen: snap2.screen,
						terminalEvents: events2,
						rawWrites: writes2,
					},
					{
						action: "toggle-false",
						cursor: snap3.cursor,
						cursorVisible: snap3.cursorVisible,
						showHardwareCursor: false,
						screen: snap3.screen,
						terminalEvents: events3,
						rawWrites: writes3,
					},
				],
			},
		});
	}

	// 5. main-screen-invalid-cursor
	{
		const term = new HardwareCursorTerminal(20, 5);
		const tui = new TuiMainScreen(term, true);
		const comp = new TestCursorComponent(["no cursor line"], undefined);
		tui.addChild(comp);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();
		tui.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		tui.stop();
		scenarios.push({
			name: "main-screen-invalid-cursor",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "MainScreen", preference: "enabled", cursor: null },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: tui.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 6. main-screen-nested
	{
		const term = new HardwareCursorTerminal(30, 10);
		const tui = new TuiMainScreen(term, false);
		const container = new Container();
		container.addChild(new Text("header line", 0, 0));
		const box = new Box(2, 1);
		const comp = new TestCursorComponent(["nested input"], 3, 0);
		box.addChild(comp);
		container.addChild(box);
		tui.addChild(container);
		tui.setFocus(comp);
		tui.start();
		term.takeEvents();
		term.takeWrites();

		tui.renderNow();
		const snap1 = await term.snapshot();
		const events1 = term.takeEvents();
		const writes1 = term.takeWrites();

		tui.setShowHardwareCursor(true);
		tui.renderNow();
		const snap2 = await term.snapshot();
		const events2 = term.takeEvents();
		const writes2 = term.takeWrites();

		tui.stop();
		scenarios.push({
			name: "main-screen-nested",
			dimensions: { columns: 30, rows: 10 },
			inputs: {
				root: "MainScreen",
				layout: "Container(Text(1 row), Box(padX=2, padY=1, Input(col=3, row=0)))",
			},
			expected: {
				steps: [
					{
						action: "preference-false",
						cursor: snap1.cursor,
						cursorVisible: snap1.cursorVisible,
						showHardwareCursor: false,
						screen: snap1.screen,
						terminalEvents: events1,
						rawWrites: writes1,
					},
					{
						action: "preference-true",
						cursor: snap2.cursor,
						cursorVisible: snap2.cursorVisible,
						showHardwareCursor: true,
						screen: snap2.screen,
						terminalEvents: events2,
						rawWrites: writes2,
					},
				],
			},
		});
	}

	// 7. alt-screen-default-hidden
	{
		const term = new HardwareCursorTerminal(20, 5);
		const alt = new TuiAltScreen(term);
		const comp = new TestCursorComponent(["line 0", "line 1"], 6, 1);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();
		alt.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		alt.stop();
		scenarios.push({
			name: "alt-screen-default-hidden",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "AltScreen", preference: "default", cursorCol: 6, cursorRow: 1 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: alt.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 8. alt-screen-enabled-visible
	{
		const term = new HardwareCursorTerminal(20, 5);
		const alt = new TuiAltScreen(term, true);
		const comp = new TestCursorComponent(["line 0", "line 1"], 6, 1);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();
		alt.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		alt.stop();
		scenarios.push({
			name: "alt-screen-enabled-visible",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "AltScreen", preference: "enabled", cursorCol: 6, cursorRow: 1 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: alt.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 9. alt-screen-disabled-explicit
	{
		const term = new HardwareCursorTerminal(20, 5);
		const alt = new TuiAltScreen(term, false);
		const comp = new TestCursorComponent(["line 0", "line 1"], 6, 1);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();
		alt.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		alt.stop();
		scenarios.push({
			name: "alt-screen-disabled-explicit",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "AltScreen", preference: "disabled", cursorCol: 6, cursorRow: 1 },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: alt.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 10. alt-screen-toggle
	{
		const term = new HardwareCursorTerminal(20, 5);
		const alt = new TuiAltScreen(term, false);
		const comp = new TestCursorComponent(["line 0", "line 1"], 6, 1);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();

		alt.renderNow();
		const snap1 = await term.snapshot();
		const events1 = term.takeEvents();
		const writes1 = term.takeWrites();

		alt.setShowHardwareCursor(true);
		alt.renderNow();
		const snap2 = await term.snapshot();
		const events2 = term.takeEvents();
		const writes2 = term.takeWrites();

		alt.setShowHardwareCursor(false);
		alt.renderNow();
		const snap3 = await term.snapshot();
		const events3 = term.takeEvents();
		const writes3 = term.takeWrites();

		alt.stop();
		scenarios.push({
			name: "alt-screen-toggle",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "AltScreen", transitions: ["default-false", "toggle-true", "toggle-false"] },
			expected: {
				steps: [
					{
						action: "default-false",
						cursor: snap1.cursor,
						cursorVisible: snap1.cursorVisible,
						showHardwareCursor: false,
						screen: snap1.screen,
						terminalEvents: events1,
						rawWrites: writes1,
					},
					{
						action: "toggle-true",
						cursor: snap2.cursor,
						cursorVisible: snap2.cursorVisible,
						showHardwareCursor: true,
						screen: snap2.screen,
						terminalEvents: events2,
						rawWrites: writes2,
					},
					{
						action: "toggle-false",
						cursor: snap3.cursor,
						cursorVisible: snap3.cursorVisible,
						showHardwareCursor: false,
						screen: snap3.screen,
						terminalEvents: events3,
						rawWrites: writes3,
					},
				],
			},
		});
	}

	// 11. alt-screen-invalid-cursor
	{
		const term = new HardwareCursorTerminal(20, 5);
		const alt = new TuiAltScreen(term, true);
		const comp = new TestCursorComponent(["no cursor line"], undefined);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();
		alt.renderNow();
		const snap = await term.snapshot();
		const events = term.takeEvents();
		const writes = term.takeWrites();
		alt.stop();
		scenarios.push({
			name: "alt-screen-invalid-cursor",
			dimensions: { columns: 20, rows: 5 },
			inputs: { root: "AltScreen", preference: "enabled", cursor: null },
			expected: {
				steps: [
					{
						action: "initial",
						cursor: snap.cursor,
						cursorVisible: snap.cursorVisible,
						showHardwareCursor: alt.getShowHardwareCursor(),
						screen: snap.screen,
						terminalEvents: events,
						rawWrites: writes,
					},
				],
			},
		});
	}

	// 12. alt-screen-nested
	{
		const term = new HardwareCursorTerminal(30, 10);
		const alt = new TuiAltScreen(term, false);
		const container = new Container();
		container.addChild(new Text("first row", 0, 0));
		const box = new Box(3, 1);
		const comp = new TestCursorComponent(["alt nested"], 4, 0);
		box.addChild(comp);
		container.addChild(box);
		alt.addChild(container);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();

		alt.renderNow();
		const snap1 = await term.snapshot();
		const events1 = term.takeEvents();
		const writes1 = term.takeWrites();

		alt.setShowHardwareCursor(true);
		alt.renderNow();
		const snap2 = await term.snapshot();
		const events2 = term.takeEvents();
		const writes2 = term.takeWrites();

		alt.stop();
		scenarios.push({
			name: "alt-screen-nested",
			dimensions: { columns: 30, rows: 10 },
			inputs: {
				root: "AltScreen",
				layout: "Container(Text(1 row), Box(padX=3, padY=1, Input(col=4, row=0)))",
			},
			expected: {
				steps: [
					{
						action: "preference-false",
						cursor: snap1.cursor,
						cursorVisible: snap1.cursorVisible,
						showHardwareCursor: false,
						screen: snap1.screen,
						terminalEvents: events1,
						rawWrites: writes1,
					},
					{
						action: "preference-true",
						cursor: snap2.cursor,
						cursorVisible: snap2.cursorVisible,
						showHardwareCursor: true,
						screen: snap2.screen,
						terminalEvents: events2,
						rawWrites: writes2,
					},
				],
			},
		});
	}

	// 13. alt-screen-clipped
	{
		const term = new HardwareCursorTerminal(20, 4);
		const alt = new TuiAltScreen(term, true);
		const lines = Array.from({ length: 8 }, (_, i) => `line ${i}`);
		const comp = new TestCursorComponent(lines, 5, 2);
		alt.addChild(comp);
		alt.setFocus(comp);
		alt.start();
		term.takeEvents();
		term.takeWrites();

		alt.renderNow();
		const snap1 = await term.snapshot();
		const events1 = term.takeEvents();
		const writes1 = term.takeWrites();

		alt.scrollToTop();
		alt.renderNow();
		const snap2 = await term.snapshot();
		const events2 = term.takeEvents();
		const writes2 = term.takeWrites();

		alt.scrollBy(1);
		alt.renderNow();
		const snap3 = await term.snapshot();
		const events3 = term.takeEvents();
		const writes3 = term.takeWrites();

		alt.scrollToBottom();
		alt.renderNow();
		const snap4 = await term.snapshot();
		const events4 = term.takeEvents();
		const writes4 = term.takeWrites();

		alt.stop();
		scenarios.push({
			name: "alt-screen-clipped",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				root: "AltScreen",
				preference: "enabled",
				contentRows: 8,
				cursorCol: 5,
				cursorRow: 2,
				transitions: ["following-end", "scroll-to-top", "scroll-by-1", "scroll-to-bottom"],
			},
			expected: {
				steps: [
					{
						action: "following-end",
						cursor: snap1.cursor,
						cursorVisible: snap1.cursorVisible,
						showHardwareCursor: true,
						screen: snap1.screen,
						terminalEvents: events1,
						rawWrites: writes1,
					},
					{
						action: "scroll-to-top",
						cursor: snap2.cursor,
						cursorVisible: snap2.cursorVisible,
						showHardwareCursor: true,
						screen: snap2.screen,
						terminalEvents: events2,
						rawWrites: writes2,
					},
					{
						action: "scroll-by-1",
						cursor: snap3.cursor,
						cursorVisible: snap3.cursorVisible,
						showHardwareCursor: true,
						screen: snap3.screen,
						terminalEvents: events3,
						rawWrites: writes3,
					},
					{
						action: "scroll-to-bottom",
						cursor: snap4.cursor,
						cursorVisible: snap4.cursorVisible,
						showHardwareCursor: true,
						screen: snap4.screen,
						terminalEvents: events4,
						rawWrites: writes4,
					},
				],
			},
		});
	}

	return { sourceEndpoints, scenarios };
}
