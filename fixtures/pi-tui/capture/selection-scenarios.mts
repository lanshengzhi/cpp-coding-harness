import { pathToFileURL } from "node:url";
import path from "node:path";

/// Independent pi-v1.0.4 observations of fullscreen mouse selection and copy
/// lifecycle (#1006). The C++ replay reads these values from the named bundle;
/// no Pike output is used to construct the expectations.
export async function captureSelection(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	const { stripTerminalSequences } = await import(source("src/utils.ts"));

	const sourceEndpoints = [
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.handleSelectionMouseEvent,getActiveSelectionText,copySelectionToClipboard,copyTextToClipboard,hasActiveSelection,copyActiveSelectionToClipboard,setCopyOnSelect",
		"packages/tui/src/components/text.ts:Text.render,setText",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.sendInput,getViewport,waitForRender",
	];
	const visibleLines = (lines: readonly string[]) => lines.map((line) => stripTerminalSequences(line).trimEnd());
	const renderedCells = (terminal: any, rows: number, columns: number) => {
		const xterm = terminal.xterm;
		const buffer = xterm.buffer.active;
		return Array.from({ length: rows }, (_, row) => {
			const line = buffer.getLine(buffer.viewportY + row);
			return Array.from({ length: columns }, (_, column) => {
				const cell = line?.getCell(column);
				return cell
					? { text: cell.getChars(), width: cell.getWidth(), inverse: cell.isInverse() !== 0, foreground: cell.getFgColor() }
					: { text: "", width: 1, inverse: false, foreground: 0 };
			});
		});
	};
	const makeHost = (columns: number, rows: number, options: Record<string, unknown> = {}) => {
		const terminal = new VirtualTerminal(columns, rows);
		const writes: string[] = [];
		const recordedTerminal = new Proxy(terminal, {
			get(target, property) {
				if (property === "write") {
					return (data: string) => {
						writes.push(data);
						target.write(data);
					};
				}
				const value = Reflect.get(target, property);
				return typeof value === "function" ? value.bind(target) : value;
			},
		});
		const tui = new TuiAltScreen(recordedTerminal, undefined, undefined, options);
		return { terminal, tui, writes };
	};
	const drag = (terminal: any, start: { x: number; y: number }, end: { x: number; y: number }) => {
		terminal.sendInput(`\x1b[<0;${start.x};${start.y}M`);
		terminal.sendInput(`\x1b[<32;${end.x};${end.y}M`);
		terminal.sendInput(`\x1b[<0;${end.x};${end.y}m`);
	};
	const scenarios: any[] = [];

	// This single logical Text line wraps at six columns. Its styled span and
	// wide CJK grapheme cross the selected cell range, so extracting by UTF-8
	// byte or source-string index differs from pi's visible-cell selection.
	{
		const copied: string[] = [];
		const { terminal, tui, writes } = makeHost(6, 4, {
			copyOnSelect: false,
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		const text = new Text("\x1b[31mred界abc\x1b[0m", 0, 0);
		tui.addChild(text);
		tui.start();
		await terminal.waitForRender();
		const before = visibleLines(terminal.getViewport());
		drag(terminal, { x: 2, y: 1 }, { x: 2, y: 2 });
		await terminal.waitForRender();
		const hasSelection = tui.hasActiveSelection();
		const highlighted = writes.some((write) => write.includes("\x1b[7m"));
		const styledCells = renderedCells(terminal, 2, 6);
		const copyResult = await tui.copyActiveSelectionToClipboard();
		await terminal.waitForRender();
		scenarios.push({
			name: "styled-cjk-wrapped-selection",
			dimensions: { columns: 6, rows: 4 },
			inputs: { text: "\x1b[31mred界abc\x1b[0m", drag: [{ x: 2, y: 1 }, { x: 2, y: 2 }], copyOnSelect: false },
			expected: {
				before,
				hasSelection,
				highlighted,
				styledCells,
				copyResult,
				copied,
				copyOnSelect: tui.getCopyOnSelect(),
			},
		});
		tui.stop();
	}

	// pi's selection stores cell coordinates. Replacing a same-shape Text keeps
	// that range, so copying afterward reads the new content at those cells.
	// Focus changes preserve a completed selection; a normal content click clears it.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(20, 4, {
			copyOnSelect: false,
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		const text = new Text("alpha\nbeta\ngamma\ndelta", 0, 0);
		tui.addChild(text);
		tui.start();
		await terminal.waitForRender();
		const emptyCopyResult = await tui.copyActiveSelectionToClipboard();
		drag(terminal, { x: 1, y: 1 }, { x: 4, y: 2 });
		await terminal.waitForRender();
		const selected = tui.hasActiveSelection();
		const firstCopyResult = await tui.copyActiveSelectionToClipboard();
		text.setText("omega\nsigma\ngamma\ndelta");
		tui.requestRender();
		await terminal.waitForRender();
		const afterContentChange = tui.hasActiveSelection();
		const updatedCopyResult = await tui.copyActiveSelectionToClipboard();
		terminal.sendInput("\x1b[O");
		terminal.sendInput("\x1b[I");
		await terminal.waitForRender();
		const afterFocusChange = tui.hasActiveSelection();
		terminal.sendInput("\x1b[<0;1;3M");
		terminal.sendInput("\x1b[<0;1;3m");
		await terminal.waitForRender();
		const afterContentClick = tui.hasActiveSelection();
		scenarios.push({
			name: "selection-content-focus-and-clear-lifecycle",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				initialText: "alpha\nbeta\ngamma\ndelta",
				drag: [{ x: 1, y: 1 }, { x: 4, y: 2 }],
				updatedText: "omega\nsigma\ngamma\ndelta",
				focus: ["out", "in"],
				contentClick: { x: 1, y: 3 },
				copyOnSelect: false,
			},
			expected: {
				emptyCopyResult,
				selected,
				firstCopyResult,
				afterContentChange,
				updatedCopyResult,
				afterFocusChange,
				afterContentClick,
				copied,
			},
		});
		tui.stop();
	}

	// The injected copy path returns its specific failure message to the TUI's
	// feedback surface. Capture the result path as well as the no-selection guard.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(80, 4, {
			copyOnSelect: false,
			copySelection: async (text: string) => {
				copied.push(text);
				return "Clipboard unavailable: install wl-clipboard";
			},
		});
		tui.addChild(new Text("alpha beta", 0, 0));
		tui.start();
		await terminal.waitForRender();
		drag(terminal, { x: 1, y: 1 }, { x: 5, y: 1 });
		await terminal.waitForRender();
		const copyResult = await tui.copyActiveSelectionToClipboard();
		await terminal.waitForRender();
		scenarios.push({
			name: "selection-copy-specific-failure",
			dimensions: { columns: 80, rows: 4 },
			inputs: { text: "alpha beta", drag: [{ x: 1, y: 1 }, { x: 5, y: 1 }], copyOnSelect: false },
			expected: {
				copyResult,
				copied,
				copyError: "Clipboard unavailable: install wl-clipboard",
				failureVisible: terminal.getViewport().some((line: string) => line.includes("Clipboard unavailable: install wl-clipboard")),
			},
		});
		tui.stop();
	}

	// The pi default copies exactly once on release for a non-empty selection;
	// a click without a range does not call the copy sink.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(20, 2, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("alpha beta", 0, 0));
		tui.start();
		await terminal.waitForRender();
		terminal.sendInput("\x1b[<0;1;1M");
		terminal.sendInput("\x1b[<0;1;1m");
		await terminal.waitForRender();
		const afterEmptyClick = [...copied];
		drag(terminal, { x: 1, y: 1 }, { x: 5, y: 1 });
		await terminal.waitForRender();
		scenarios.push({
			name: "copy-on-select-default-and-empty-range",
			dimensions: { columns: 20, rows: 2 },
			inputs: { text: "alpha beta", emptyClick: { x: 1, y: 1 }, drag: [{ x: 1, y: 1 }, { x: 5, y: 1 }] },
			expected: {
				defaultCopyOnSelect: tui.getCopyOnSelect(),
				afterEmptyClick,
				copied,
				hasSelection: tui.hasActiveSelection(),
			},
		});
		tui.stop();
	}

	return { sourceEndpoints, scenarios };
}
