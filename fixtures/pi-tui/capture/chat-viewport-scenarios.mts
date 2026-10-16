#!/usr/bin/env tsx
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent frozen observations of the product two-region fullscreen
/// viewport (#1036, spec #946).
///
/// The imported module is the authoritative seam: the frozen
/// `createChatViewport` (`packages/coding-agent/src/modes/interactive/chat-viewport.ts:22`)
/// that the CLI `--tui-mode fullscreen` path installs as the `AltScreen`
/// layout root (`interactive-mode.ts:961-980`, `:868-875`). The scenarios drive
/// it through the real frozen toolkit (`TuiAltScreen` + the frozen
/// `VirtualTerminal` of #947) and report complete frames, cells/styles and
/// cursor positions for each step.
///
/// The regions are plain `Container`/`Text` content because the product call
/// site needs the full interactive runtime: only the assembly seam is
/// executable in isolation. Nothing here is derived from Pike; the product
/// regions, the `setLayoutRoot`-over-mounted-children alias, and the session
/// wiring are recorded as cited source endpoints instead of executed
/// (handoff decision 7).
///
/// Every step records the complete frame (rows, cursor, cursor visibility) and
/// the transcript state. A step whose frame draws any styled cell fails the
/// capture unless it also records the complete styled grid, so a style can
/// never hide in an unrecorded frame; the scrollbar frames therefore carry
/// cells while the plain frames assert they have nothing to hide.
export async function captureChatViewport(piCheckout: string) {
	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
	const { Terminal: XtermTerminal } = requirePi("@xterm/headless");
	const { createChatViewport } = await import(coding("src/modes/interactive/chat-viewport.ts"));
	const { Container } = await import(source("src/tui.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	// The frozen VirtualTerminal is the renderer adapter; its exact output goes
	// to a second public xterm instance that exposes full cell attributes
	// (mirrors `capture/capture-named-tui.mts`, which never uses private pi
	// hooks). The observer refuses frames the public adapter disagrees with.
	class CellTerminal extends VirtualTerminal {
		readonly emulator: any;
		cursorVisible = true;
		constructor(columns: number, rows: number) {
			super(columns, rows);
			this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
		}
		override write(data: string) {
			if ([...data.matchAll(/\x1b\]8;[^;]*;([^\x07\x1b]*)/g)].some((match) => match[1])) {
				throw new Error("OSC 8 cell capture needs link-aware evidence before adding linked scenarios");
			}
			super.write(data);
			this.emulator.write(data);
		}
		override hideCursor() {
			super.hideCursor();
			this.cursorVisible = false;
		}
		override showCursor() {
			super.showCursor();
			this.cursorVisible = true;
		}
		// The observer must follow the terminal geometry: the resize scenario
		// compares frames at more than one size.
		override resize(columns: number, rows: number) {
			super.resize(columns, rows);
			this.emulator.resize(columns, rows);
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

	interface DockInputs {
		readonly columns: number;
		readonly rows: number;
		readonly document: string[];
		readonly editor: string;
		readonly pending?: string;
		readonly status?: string;
		readonly footer?: string[];
		readonly widgetsAbove?: string[];
		readonly widgetsBelow?: string[];
		readonly scrollbar?: "auto" | "hidden";
	}

	/// One replayed step: the frozen name plus the inputs that produced it, so
	/// the C++ replay applies recorded actions instead of re-deriving them.
	interface StepAction {
		readonly name: string;
		readonly inputs?: string[];
		readonly resize?: { columns: number; rows: number };
		readonly document?: string[];
		readonly editor?: string;
		/// Record the complete styled cell grid (required for styled frames).
		readonly cells?: boolean;
	}

	/// One live viewport: the document is a `Text` that can grow while the
	/// dock regions stay the same instances, exactly as the product appends
	/// chat rows into the scrollable document.
	class Viewport {
		readonly terminal: CellTerminal;
		readonly tui: any;
		readonly transcript: any;
		readonly documentText: Text;
		readonly editorText: Text;

		constructor(inputs: DockInputs) {
			this.terminal = new CellTerminal(inputs.columns, inputs.rows);
			const documentContainer = new Container();
			this.documentText = new Text(inputs.document.join("\n"), 0, 0);
			documentContainer.addChild(this.documentText);
			this.editorText = new Text(inputs.editor, 0, 0);
			const viewport = createChatViewport({
				document: documentContainer,
				pendingMessages: new Text(inputs.pending ?? "", 0, 0),
				status: new Text(inputs.status ?? "", 0, 0),
				editor: this.editorText,
				footer: new Text((inputs.footer ?? []).join("\n"), 0, 0),
				...(inputs.widgetsAbove === undefined ? {} : { widgetsAbove: new Text(inputs.widgetsAbove.join("\n"), 0, 0) }),
				...(inputs.widgetsBelow === undefined ? {} : { widgetsBelow: new Text(inputs.widgetsBelow.join("\n"), 0, 0) }),
				...(inputs.scrollbar === undefined ? {} : { scrollbar: inputs.scrollbar }),
			});
			this.transcript = viewport.transcript;
			this.tui = new TuiAltScreen(this.terminal);
			this.tui.setLayoutRoot(viewport.root);
			this.tui.start();
		}

		/// Applies one recorded step and returns its complete observation.
		async apply(action: StepAction) {
			if (action.resize !== undefined) this.terminal.resize(action.resize.columns, action.resize.rows);
			if (action.document !== undefined) this.documentText.setText(action.document.join("\n"));
			if (action.editor !== undefined) this.editorText.setText(action.editor);
			for (const input of action.inputs ?? []) this.terminal.sendInput(input);
			if (action.document !== undefined || action.editor !== undefined) this.tui.requestRender();
			await this.terminal.waitForRender();
			const frame = await this.terminal.snapshot();
			const styled = frame.cells.some((row: any[]) =>
				row.some((cell: any) =>
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
					),
				),
			);
			assert.ok(
				!styled || action.cells === true,
				`${action.name}: the frozen frame draws styled cells; record its complete cell grid`,
			);
			return {
				action: action.name,
				screen: frame.screen.map((line: string) => line.trimEnd()),
				cursor: frame.cursor,
				cursorVisible: frame.cursorVisible,
				transcript: {
					scrollTop: this.transcript.scrollTop,
					viewportHeight: this.transcript.viewportHeight,
					isFollowingEnd: this.transcript.isFollowingEnd,
					scrollbar: this.transcript.scrollbar,
					isScrollbarVisible: this.transcript.isScrollbarVisible,
				},
				tui: { viewportTop: this.tui.viewportTop, isFollowingOutput: this.tui.isFollowingOutput },
				...(action.cells === true ? { cells: frame.cells, scrollback: frame.scrollback } : {}),
			};
		}

		stop() {
			this.tui.stop();
			this.terminal.emulator.dispose();
		}
	}

	const scenario = async (
		name: string,
		inputs: DockInputs,
		actions: StepAction[],
		check: (viewport: Viewport, steps: any[]) => void = () => {},
	) => {
		const viewport = new Viewport(inputs);
		const steps: any[] = [];
		for (const action of actions) steps.push(await viewport.apply(action));
		check(viewport, steps);
		viewport.stop();
		return {
			name,
			dimensions: { columns: inputs.columns, rows: inputs.rows },
			inputs: { ...inputs, steps: actions },
			expected: { steps },
		};
	};

	// (a) short content: the transcript hands its free rows back to the dock
	// and never grows past its own content.
	const shortContent = await scenario(
		"viewport-short-content-keeps-dock-at-bottom",
		{
			columns: 24,
			rows: 9,
			document: ["one", "two"],
			pending: "pending-1",
			status: "status-1",
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
		},
		[{ name: "initial" }],
	);

	// (b) overflowing document with `follow: "end"`: the newest rows are in the
	// window and the dock keeps its fixed rows at the bottom.
	const overflowing = await scenario(
		"viewport-overflowing-document-follows-end",
		{
			columns: 20,
			rows: 8,
			document: Array.from({ length: 12 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			status: "status-1",
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
		},
		[{ name: "initial" }],
		(viewport, steps) => {
			assert.equal(viewport.transcript.isFollowingEnd, true);
			assert.equal(viewport.tui.isFollowingOutput, true);
			assert.ok(steps[0].screen.includes("doc-12"));
		},
	);

	// (c) appending while following keeps the window pinned to the end without
	// moving the dock rows.
	const appended = await scenario(
		"viewport-append-while-following-pins-end",
		{
			columns: 20,
			rows: 8,
			document: Array.from({ length: 12 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
		},
		[
			{ name: "initial" },
			{
				name: "after-append",
				document: Array.from({ length: 16 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			},
		],
		(viewport, steps) => {
			assert.equal(viewport.transcript.isFollowingEnd, true);
			assert.ok(steps[1].screen.includes("doc-16"));
			assert.deepEqual(steps[0].screen.slice(3), steps[1].screen.slice(3));
		},
	);

	// (d) narrow terminal with an editor line longer than the width: the dock
	// wraps it and the transcript keeps its own rows.
	const narrowEditor = await scenario(
		"viewport-narrow-editor-wraps-inside-dock",
		{
			columns: 16,
			rows: 8,
			document: ["doc-1", "doc-2", "doc-3", "doc-4", "doc-5", "doc-6"],
			editor: "a long editor line that must wrap",
			footer: ["footer-1", "footer-2"],
		},
		[{ name: "initial" }],
		(viewport, steps) => {
			assert.deepEqual(steps[0].screen.slice(3, 6), ["a long editor", "line that must", "wrap"]);
			assert.equal(steps[0].transcript.viewportHeight, 3);
		},
	);

	// (e) resize smaller and larger: the transcript re-wraps, and both footer
	// rows survive every geometry.
	const resized = await scenario(
		"viewport-resize-keeps-both-footer-rows",
		{
			columns: 24,
			rows: 10,
			document: ["doc-1", "doc-2", "doc-3", "doc-4", "doc-5", "doc-6"],
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
		},
		[
			{ name: "initial" },
			{ name: "smaller", resize: { columns: 14, rows: 7 } },
			{ name: "larger", resize: { columns: 30, rows: 12 } },
		],
		(viewport, steps) => {
			for (const step of steps) {
				assert.ok(step.screen.includes("footer-1") && step.screen.includes("footer-2"));
			}
			assert.ok(steps[0].transcript.viewportHeight >= 1);
			assert.equal(steps[2].transcript.viewportHeight, 7);
		},
	);

	// (f) editor growth: one line is floored at `minSize: 3`, and a taller
	// editor takes the rows the document yields.
	const editorGrowth = await scenario(
		"viewport-editor-min-size-floor-and-growth",
		{
			columns: 20,
			rows: 12,
			document: ["doc-1", "doc-2"],
			editor: "prompt",
			footer: ["footer-1", "footer-2"],
		},
		[
			{ name: "one-line-editor" },
			{ name: "seven-line-editor", editor: Array.from({ length: 7 }, (_, row) => `prompt-${row + 1}`).join("\n") },
		],
		(viewport, steps) => {
			// One input row still occupies the `minSize: 3` dock slot.
			assert.deepEqual(steps[0].screen.slice(7, 10), ["prompt", "", ""]);
			assert.ok(steps[1].screen.includes("prompt-7"));
		},
	);

	// (g) transcript scrolled up: the wheel moves the transcript window while
	// the dock keeps every fixed row.
	const scrolledUp = await scenario(
		"viewport-wheel-scroll-keeps-dock-fixed",
		{
			columns: 20,
			rows: 8,
			document: Array.from({ length: 20 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			pending: "pending-1",
			status: "status-1",
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
		},
		[
			{ name: "initial" },
			{ name: "wheel-up", inputs: ["\x1b[<64;1;1M", "\x1b[<64;1;1M"], cells: true },
		],
		(viewport, steps) => {
			assert.equal(steps[0].transcript.isFollowingEnd, true);
			assert.equal(steps[1].transcript.isFollowingEnd, false);
			assert.ok(steps[1].transcript.scrollTop < steps[0].transcript.scrollTop);
			assert.equal(steps[1].tui.viewportTop, steps[1].transcript.scrollTop);
			assert.equal(steps[1].tui.isFollowingOutput, false);
			assert.deepEqual(steps[0].screen.slice(1), steps[1].screen.slice(1));
			assert.equal(steps[1].transcript.isScrollbarVisible, true);
		},
	);

	// (h) the frozen arbiter case (`packages/coding-agent/test/chat-viewport.test.ts`):
	// the scrollbar defaults to `auto`, and an explicit `hidden` override is
	// honoured. Wheel input makes the automatic bar draw its styled cells.
	const scrollbarDefault = await scenario(
		"viewport-scrollbar-defaults-to-auto",
		{
			columns: 20,
			rows: 6,
			document: Array.from({ length: 14 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			editor: "editor-1",
			footer: ["footer-1"],
		},
		[{ name: "initial" }, { name: "wheel-up", inputs: ["\x1b[<64;1;1M"], cells: true }],
		(viewport, steps) => {
			assert.equal(viewport.transcript.scrollbar, "auto");
			assert.equal(steps[0].transcript.isScrollbarVisible, false);
			assert.equal(steps[1].transcript.isScrollbarVisible, true);
		},
	);

	const scrollbarHidden = await scenario(
		"viewport-scrollbar-hidden-override",
		{
			columns: 20,
			rows: 6,
			document: Array.from({ length: 14 }, (_, row) => `doc-${String(row + 1).padStart(2, "0")}`),
			editor: "editor-1",
			footer: ["footer-1"],
			scrollbar: "hidden",
		},
		[{ name: "initial" }, { name: "wheel-up", inputs: ["\x1b[<64;1;1M"], cells: true }],
		(viewport, steps) => {
			assert.equal(viewport.transcript.scrollbar, "hidden");
			assert.equal(steps[1].transcript.isScrollbarVisible, false);
			assert.ok(steps[1].transcript.scrollTop < steps[0].transcript.scrollTop);
		},
	);

	// (i) the optional widget regions keep their frozen dock order:
	// pending, status, widgetsAbove, editor, widgetsBelow, footer.
	const widgets = await scenario(
		"viewport-widget-regions-keep-dock-order",
		{
			columns: 24,
			rows: 10,
			document: ["doc-1", "doc-2", "doc-3", "doc-4", "doc-5", "doc-6"],
			pending: "pending-1",
			status: "status-1",
			editor: "editor-1",
			footer: ["footer-1", "footer-2"],
			widgetsAbove: ["above-1"],
			widgetsBelow: ["below-1"],
		},
		[{ name: "initial" }],
		(viewport, steps) => {
			const order = ["pending-1", "status-1", "above-1", "editor-1", "below-1", "footer-1", "footer-2"];
			const where = order.map((row) => steps[0].screen.indexOf(row));
			assert.ok(where.every((index) => index >= 0));
			assert.deepEqual([...where].sort((left, right) => left - right), where);
		},
	);

	return {
		sourceEndpoints: [
			"packages/coding-agent/src/modes/interactive/chat-viewport.ts:createChatViewport",
			"packages/coding-agent/test/chat-viewport.test.ts:createChatViewport",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:setLayoutRoot,document,pendingMessages,status,editor,footer,widgetsAbove,widgetsBelow",
			"packages/tui/src/components/scroll-view.ts:ScrollView,scrollTop,viewportHeight,isFollowingEnd,scrollbar,isScrollbarVisible",
			"packages/tui/src/components/v-stack.ts:VStack",
			"packages/tui/src/components/stack.ts:allocateStackSizes,visibleStackEntries",
			"packages/tui/src/tui-alt-screen.ts:TuiAltScreen,setLayoutRoot,viewportTop,isFollowingOutput",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.getViewport,VirtualTerminal.sendInput,VirtualTerminal.resize,VirtualTerminal.waitForRender",
		],
		scenarios: [
			shortContent,
			overflowing,
			appended,
			narrowEditor,
			resized,
			editorGrowth,
			scrolledUp,
			scrollbarDefault,
			scrollbarHidden,
			widgets,
		],
	};
}
