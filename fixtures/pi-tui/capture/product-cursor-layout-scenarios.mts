#!/usr/bin/env node
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

export interface TerminalEvent {
	op: "write" | "showCursor" | "hideCursor";
	bytes?: string;
}

export interface CursorPosition {
	column: number;
	row: number;
}

export interface ProductCursorLayoutScenario {
	name: string;
	description: string;
	dimensions: { columns: number; rows: number };
	inputs: Record<string, any>;
	expected: Record<string, any>;
}

export async function captureProductCursorLayout(piCheckout: string) {
	// In-memory runtime hook for missing generated Azure JSON in frozen pi checkout.
	// Does not write anything into frozen checkout.
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

	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
	const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

	const { createChatViewport } = await import(coding("src/modes/interactive/chat-viewport.ts"));
	const { createInteractiveTui, InteractiveMode } = await import(
		coding("src/modes/interactive/interactive-mode.ts")
	);
	const { Container, Text, CURSOR_MARKER, isViewportTUI } = await import(source("src/index.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	class ProductCursorLayoutTerminal extends VirtualTerminal {
		readonly emulator: any;
		cursorVisible = false;
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
			this.cursorVisible = false;
			this.events.push({ op: "hideCursor" });
			super.hideCursor();
		}

		override showCursor() {
			this.cursorVisible = true;
			this.events.push({ op: "showCursor" });
			super.showCursor();
		}

		override resize(columns: number, rows: number) {
			super.resize(columns, rows);
			this.emulator.resize(columns, rows);
		}

		takeEvents(): TerminalEvent[] {
			const res = [...this.events];
			this.events.length = 0;
			return res;
		}

		takeWrites(): string[] {
			const res = [...this.writes];
			this.writes.length = 0;
			return res;
		}

		async snapshot() {
			await this.flush();
			await new Promise<void>((resolve) => this.emulator.write("", () => resolve()));
			const buffer = this.emulator.buffer.active;
			const lines = Array.from({ length: this.rows }, (_, r) =>
				buffer.getLine(buffer.viewportY + r).translateToString(true),
			);
			return {
				screen: lines,
				cursor: { column: buffer.cursorX, row: buffer.cursorY },
				cursorVisible: this.cursorVisible,
			};
		}
	}

	class ControlledCursorEditor {
		focused = true;
		lines: string[];
		cursorCol?: number;
		cursorRow: number;

		constructor(lines: string[], cursorCol?: number, cursorRow = 0) {
			this.lines = lines;
			this.cursorCol = cursorCol;
			this.cursorRow = cursorRow;
		}

		invalidate(): void {}

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

	const sourceEndpoints = [
		"packages/coding-agent/src/modes/interactive/chat-viewport.ts:createChatViewport",
		"packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.switchTuiMode",
		"packages/coding-agent/src/modes/interactive/tui-renderer.ts:createInteractiveTui",
	];

	const scenarios: ProductCursorLayoutScenario[] = [];

	const standardDocument = ["header 0", "header 1", "header 2", "header 3"];
	const standardStatus = ["status line 0", "status line 1"];
	const standardEditor = ["line 0", "line 1 nested", "line 2"];
	const standardFooter = ["footer line 0", "footer line 1"];

	// 1-4. Four 48x24 live switch cases
	const runLiveSwitchCase = async (
		name: string,
		description: string,
		initialMode: "regular" | "fullscreen",
		targetMode: "regular" | "fullscreen",
		initialPref: boolean,
		livePref: boolean,
	) => {
		const terminal = new ProductCursorLayoutTerminal(48, 24);
		const initialRenderer = createInteractiveTui({
			tuiMode: initialMode,
			showHardwareCursor: initialPref,
			logDirectory: "/tmp",
			terminal,
			fullscreenCopyOnSelect: true,
			fullscreenWheelScrollLines: "auto",
		});

		const documentContainer = new Container();
		documentContainer.addChild(new Text(standardDocument.join("\n"), 0, 0));

		const pendingContainer = new Container();
		const statusContainer = new Container();
		statusContainer.addChild(new Text(standardStatus.join("\n"), 0, 0));

		const editor = new ControlledCursorEditor(standardEditor, 5, 1);
		const editorContainer = new Container();
		editorContainer.addChild(editor);

		const footerContainer = new Container();
		footerContainer.addChild(new Text(standardFooter.join("\n"), 0, 0));

		const viewport = createChatViewport({
			document: documentContainer,
			pendingMessages: pendingContainer,
			status: statusContainer,
			editor: editorContainer,
			footer: footerContainer,
			scrollbar: "hidden",
		});

		const regions = [documentContainer, pendingContainer, statusContainer, editorContainer, footerContainer];
		for (const r of regions) initialRenderer.addChild(r);
		if (isViewportTUI(initialRenderer)) initialRenderer.setLayoutRoot(viewport.root);
		initialRenderer.setFocus(editor);

		initialRenderer.start();
		initialRenderer.renderNow();

		const initialSnap = await terminal.snapshot();
		terminal.takeWrites();
		terminal.takeEvents();

		// Live preference toggle
		initialRenderer.setShowHardwareCursor(livePref);
		initialRenderer.renderNow();

		const liveSnap = await terminal.snapshot();
		const liveWrites = terminal.takeWrites();
		const liveEvents = terminal.takeEvents();

		// Mode switch
		const context: any = Object.assign(Object.create(InteractiveMode.prototype), {
			renderer: initialRenderer,
			options: { tuiMode: initialMode },
			runtimeHost: {
				session: {
					settingsManager: {
						getFullscreenCopyOnSelect: () => false,
						getFullscreenWheelScrollLines: () => "auto",
						getShowTerminalProgress: () => false,
					},
					isStreaming: false,
					isCompacting: false,
				},
			},
			fullscreenLayoutRoot: viewport.root,
			mainScreenRenderState: undefined,
			onRightClickPaste: undefined,
			themeController: { rebindTui: () => {} },
			extensionTerminalInputSubscriptions: new Set(),
		});

		const switchOk = context.switchTuiMode(targetMode);
		assert.equal(switchOk, true);
		context.renderer.renderNow();

		const switchSnap = await terminal.snapshot();
		const switchWrites = terminal.takeWrites();
		const switchEvents = terminal.takeEvents();

		if (terminal.started) context.renderer.stop();
		terminal.emulator.dispose();

		scenarios.push({
			name,
			description,
			dimensions: { columns: 48, rows: 24 },
			inputs: {
				initialMode,
				targetMode,
				initialPreference: initialPref,
				livePreference: livePref,
				document: standardDocument,
				pending: [],
				status: standardStatus,
				editor: standardEditor,
				footer: standardFooter,
				cursorCol: 5,
				cursorRow: 1,
			},
			expected: {
				initial: {
					mode: initialMode,
					showHardwareCursor: initialPref,
					cursorVisible: initialSnap.cursorVisible,
					cursor: initialSnap.cursor,
				},
				live: {
					mode: initialMode,
					showHardwareCursor: livePref,
					cursorVisible: liveSnap.cursorVisible,
					cursor: liveSnap.cursor,
					rawWrites: liveWrites,
					terminalEvents: liveEvents,
				},
				switch: {
					mode: targetMode,
					showHardwareCursor: livePref,
					cursorVisible: switchSnap.cursorVisible,
					cursor: switchSnap.cursor,
					screen: switchSnap.screen,
					rawWrites: switchWrites,
					terminalEvents: switchEvents,
				},
			},
		});
	};

	await runLiveSwitchCase(
		"switch-regular-to-fullscreen-live-true",
		"Live switch from regular to fullscreen preserving live preference true with exact dock offset 20",
		"regular",
		"fullscreen",
		false,
		true,
	);

	await runLiveSwitchCase(
		"switch-regular-to-fullscreen-live-false",
		"Live switch from regular to fullscreen preserving live preference false",
		"regular",
		"fullscreen",
		true,
		false,
	);

	await runLiveSwitchCase(
		"switch-fullscreen-to-regular-live-false",
		"Live switch from fullscreen to regular preserving live preference false with exact frame row 7",
		"fullscreen",
		"regular",
		true,
		false,
	);

	await runLiveSwitchCase(
		"switch-fullscreen-to-regular-live-true",
		"Live switch from fullscreen to regular preserving live preference true with exact frame row 7",
		"fullscreen",
		"regular",
		false,
		true,
	);

	// 5-6. Two real layout clipping scenarios with valid cursor col 5 row 1 at small terminal height 2
	const runClippedSwitchCase = async (
		name: string,
		description: string,
		initialMode: "regular" | "fullscreen",
		targetMode: "regular" | "fullscreen",
	) => {
		const terminal = new ProductCursorLayoutTerminal(48, 2);
		const initialRenderer = createInteractiveTui({
			tuiMode: initialMode,
			showHardwareCursor: true,
			logDirectory: "/tmp",
			terminal,
			fullscreenCopyOnSelect: true,
			fullscreenWheelScrollLines: "auto",
		});

		const documentContainer = new Container();
		documentContainer.addChild(new Text(standardDocument.join("\n"), 0, 0));

		const pendingContainer = new Container();
		const statusContainer = new Container();
		statusContainer.addChild(new Text(standardStatus.join("\n"), 0, 0));

		const editor = new ControlledCursorEditor(standardEditor, 5, 1);
		const editorContainer = new Container();
		editorContainer.addChild(editor);

		const footerContainer = new Container();
		footerContainer.addChild(new Text(standardFooter.join("\n"), 0, 0));

		const viewport = createChatViewport({
			document: documentContainer,
			pendingMessages: pendingContainer,
			status: statusContainer,
			editor: editorContainer,
			footer: footerContainer,
			scrollbar: "hidden",
		});

		const regions = [documentContainer, pendingContainer, statusContainer, editorContainer, footerContainer];
		for (const r of regions) initialRenderer.addChild(r);
		if (isViewportTUI(initialRenderer)) initialRenderer.setLayoutRoot(viewport.root);
		initialRenderer.setFocus(editor);

		initialRenderer.start();
		initialRenderer.renderNow();

		const initialClippedSnap = await terminal.snapshot();
		terminal.takeWrites();
		terminal.takeEvents();

		const context: any = Object.assign(Object.create(InteractiveMode.prototype), {
			renderer: initialRenderer,
			options: { tuiMode: initialMode },
			runtimeHost: {
				session: {
					settingsManager: {
						getFullscreenCopyOnSelect: () => false,
						getFullscreenWheelScrollLines: () => "auto",
						getShowTerminalProgress: () => false,
					},
					isStreaming: false,
					isCompacting: false,
				},
			},
			fullscreenLayoutRoot: viewport.root,
			mainScreenRenderState: undefined,
			onRightClickPaste: undefined,
			themeController: { rebindTui: () => {} },
			extensionTerminalInputSubscriptions: new Set(),
		});

		const switchOk = context.switchTuiMode(targetMode);
		assert.equal(switchOk, true);
		context.renderer.renderNow();

		const switchClippedSnap = await terminal.snapshot();
		const switchClippedWrites = terminal.takeWrites();
		const switchClippedEvents = terminal.takeEvents();

		// Resize to 48x24 to restore visible cursor
		terminal.resize(48, 24);
		context.renderer.renderNow();

		const restoredSnap = await terminal.snapshot();
		const restoredWrites = terminal.takeWrites();
		const restoredEvents = terminal.takeEvents();

		if (terminal.started) context.renderer.stop();
		terminal.emulator.dispose();

		scenarios.push({
			name,
			description,
			dimensions: { columns: 48, rows: 2 },
			inputs: {
				initialMode,
				targetMode,
				preference: true,
				document: standardDocument,
				pending: [],
				status: standardStatus,
				editor: standardEditor,
				footer: standardFooter,
				cursorCol: 5,
				cursorRow: 1,
				constrainedRows: 2,
				restoredRows: 24,
			},
			expected: {
				initialClipped: {
					mode: initialMode,
					showHardwareCursor: true,
					cursorVisible: initialClippedSnap.cursorVisible,
					cursor: initialClippedSnap.cursor,
				},
				afterSwitchClipped: {
					mode: targetMode,
					showHardwareCursor: true,
					cursorVisible: switchClippedSnap.cursorVisible,
					cursor: switchClippedSnap.cursor,
					screen: switchClippedSnap.screen,
					rawWrites: switchClippedWrites,
					terminalEvents: switchClippedEvents,
				},
				afterResizeRestored: {
					mode: targetMode,
					showHardwareCursor: true,
					cursorVisible: restoredSnap.cursorVisible,
					cursor: restoredSnap.cursor,
					screen: restoredSnap.screen,
					rawWrites: restoredWrites,
					terminalEvents: restoredEvents,
				},
			},
		});
	};

	await runClippedSwitchCase(
		"clipped-switch-fullscreen-to-regular",
		"Mode switch from fullscreen to regular while valid cursor is clipped at height 2, then restored at height 24",
		"fullscreen",
		"regular",
	);

	await runClippedSwitchCase(
		"clipped-switch-regular-to-fullscreen",
		"Mode switch from regular to fullscreen while valid cursor is clipped at height 2, then restored at height 24",
		"regular",
		"fullscreen",
	);

	return {
		sourceEndpoints,
		scenarios,
	};
}
