import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent fullscreen search observations from frozen pi-v1.0.4.
export async function captureAltScreenSearch(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { AltScreenSearchIndex, findAltScreenSearchMatches } = await import(source("src/alt-screen-search.ts"));
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { VStack } = await import(source("src/components/v-stack.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	class RecordingTerminal extends VirtualTerminal {
		readonly writes: string[] = [];
		override write(data: string) {
			this.writes.push(data);
			super.write(data);
		}
	}

	const visible = (terminal: InstanceType<typeof VirtualTerminal>) => terminal.getViewport().map((line) => line.trimEnd());
	const wait = async (terminal: InstanceType<typeof VirtualTerminal>) => terminal.waitForRender();
	const scenarios: any[] = [];

	const normalizedLines = ["alpha QUICK", "brown fox"];
	scenarios.push({
		name: "search-normalized-multirow",
		dimensions: { columns: 40, rows: 6 },
		inputs: { lines: normalizedLines, query: "quick   brown" },
		expected: findAltScreenSearchMatches(normalizedLines, "quick   brown"),
	});

	const unicodeLines = ["\x1b[31mfoo  bar\x1b[0m", "A界🙂éZ"];
	scenarios.push({
		name: "search-ansi-cjk-columns",
		dimensions: { columns: 40, rows: 6 },
		inputs: { lines: unicodeLines, query: "oo   bar\nA界🙂é" },
		expected: findAltScreenSearchMatches(unicodeLines, "oo   bar\nA界🙂é"),
	});

	const cacheIndex = new AltScreenSearchIndex();
	const cacheLines = ["alpha needle", "omega"];
	const cacheInputs = [
		{ lines: cacheLines, query: "needle" },
		{ lines: cacheLines, query: "needle" },
		{ lines: cacheLines, query: "omega" },
		{ lines: ["alpha needle", "no match"], query: "omega" },
	];
	const cacheTrace = cacheInputs.map((input) => {
		const result = cacheIndex.search(input.lines, input.query);
		return { changed: result.changed, matches: result.matches };
	});
	scenarios.push({
		name: "search-index-cache",
		dimensions: { columns: 40, rows: 6 },
		inputs: cacheInputs,
		expected: cacheTrace,
	});

	{
		const terminal = new RecordingTerminal(60, 8);
		const tui = new TuiAltScreen(terminal);
		tui.addChild(new Text("alpha needle\nmiddle\nneedle omega\nend", 0, 0));
		tui.start();
		await wait(terminal);
		terminal.sendInput("\x1b[102;6u");
		await wait(terminal);
		scenarios.push({
			name: "search-render-controls",
			dimensions: { columns: 60, rows: 8 },
			inputs: { transcript: ["alpha needle", "middle", "needle omega", "end"], open: "\x1b[102;6u" },
			expected: { screen: visible(terminal) },
		});
		tui.stop();
	}

	{
		const terminal = new RecordingTerminal(120, 6);
		const tui = new TuiAltScreen(terminal, undefined, undefined, {
			searchNavigationButtonStyle: (text: string, hovered: boolean) => `${hovered ? "\x1b[45m" : "\x1b[44m"}${text}\x1b[49m`,
		});
		tui.addChild(new Text("needle one\nmiddle\nneedle two\nend", 0, 0));
		tui.start();
		await wait(terminal);
		terminal.sendInput("\x1b[102;6u");
		terminal.sendInput("needle");
		await wait(terminal);
		const phases = [{ action: "query", screen: visible(terminal) }];
		terminal.sendInput("\r");
		await wait(terminal);
		phases.push({ action: "next-wrap", screen: visible(terminal) });
		terminal.sendInput("\x1b[13;2u");
		await wait(terminal);
		phases.push({ action: "previous-wrap", screen: visible(terminal) });

		let screen = visible(terminal);
		const controlsRow = screen.findIndex((line) => line.includes("↑") && line.includes("↓"));
		const nextColumn = screen[controlsRow]?.lastIndexOf("Enter") ?? -1;
		assert.ok(controlsRow >= 0 && nextColumn >= 0);
		const hoverStart = terminal.writes.length;
		const cursorBeforeHover = terminal.getCursorPosition();
		const hover = `\x1b[<35;${nextColumn + 1};${controlsRow + 1}M`;
		terminal.sendInput(hover);
		await wait(terminal);
		const cursorAfterHover = terminal.getCursorPosition();
		assert.deepStrictEqual(cursorAfterHover, cursorBeforeHover);
		phases.push({ action: "hover-next", screen: visible(terminal) });
		const hoverWrites = terminal.writes.slice(hoverStart).filter((write) => write.includes("\x1b[45m↓ Enter\x1b[49m"));
		assert.ok(hoverWrites.length > 0);
		const nextClick = `\x1b[<0;${nextColumn + 1};${controlsRow + 1}M`;
		terminal.sendInput(nextClick);
		await wait(terminal);
		phases.push({ action: "mouse-next", screen: visible(terminal) });

		terminal.sendInput("\x7f");
		await wait(terminal);
		phases.push({ action: "query-edit", screen: visible(terminal) });
		terminal.sendInput("e");
		await wait(terminal);
		phases.push({ action: "query-restore", screen: visible(terminal) });

		terminal.sendInput("\x1b[102;6u");
		await wait(terminal);
		phases.push({ action: "toggle-close", screen: visible(terminal) });
		terminal.sendInput("\x1b[102;6u");
		await wait(terminal);
		phases.push({ action: "toggle-reopen", screen: visible(terminal) });
		tui.stop();
		scenarios.push({
			name: "search-navigate-wrap",
			dimensions: { columns: 120, rows: 6 },
			inputs: {
				transcript: ["needle one", "middle", "needle two", "end"],
				query: "needle",
				keys: ["enter", "shift+enter", "backspace", "e", "ctrl+shift+f"],
				hover,
				nextClick,
			},
			expected: { phases, hoverWrites, hoverCursor: { before: cursorBeforeHover, after: cursorAfterHover } },
		});
	}

	{
		const terminal = new RecordingTerminal(60, 8);
		const tui = new TuiAltScreen(terminal);
		const transcriptLines = Array.from({ length: 12 }, (_, index) => {
			if (index === 4) return "line 5 needle one";
			if (index === 9) return "line 10 needle two";
			return `line ${index + 1}`;
		});
		const transcript = new ScrollView(new Text(transcriptLines.join("\n"), 0, 0), { follow: "end", primary: true });
		const editorInputs: string[] = [];
		let editorFocused = false;
		const editor = {
			get focused() { return editorFocused; },
			set focused(value: boolean) { editorFocused = value; },
			render: () => ["editor"],
			invalidate: () => {},
			handleInput: (data: string) => editorInputs.push(data),
		};
		class InputOverlay {
			focused = false;
			inputs: string[] = [];
			handleInput(data: string) { this.inputs.push(data); }
			render() { return ["overlay"]; }
			invalidate() {}
		}
		tui.setLayoutRoot(new VStack([
			{ component: transcript, basis: 0, grow: 1, minSize: 1 },
			{ component: editor, basis: 1, shrink: 0 },
		]));
		tui.setFocus(editor);
		tui.start();
		await wait(terminal);
		const overlay = new InputOverlay();
		tui.showOverlay(overlay);
		await wait(terminal);
		const topBefore = tui.viewportTop;
		const pageUp = "\x1b[5~";
		terminal.sendInput(pageUp);
		await wait(terminal);
		const topAfterPageUp = tui.viewportTop;
		const open = "\x1b[102;6u";
		terminal.sendInput(open);
		terminal.sendInput("needle");
		await wait(terminal);
		const openScreen = visible(terminal);
		const close = "\x1b[27u";
		terminal.sendInput(close);
		await wait(terminal);
		const resumedInput = "x";
		terminal.sendInput(resumedInput);
		await wait(terminal);
		const closed = { screen: visible(terminal), editorFocused, editorInputs, overlayFocused: overlay.focused, overlayInputs: overlay.inputs };
		tui.stop();
		scenarios.push({
			name: "search-close-restores-focus",
			dimensions: { columns: 60, rows: 8 },
			inputs: { transcript: transcriptLines, open, query: "needle", close, resumedInput, pageUp },
			expected: { topBefore, topAfterPageUp, openScreen, closed },
		});
	}

	{
		const terminal = new RecordingTerminal(80, 10);
		const tui = new TuiAltScreen(terminal);
		tui.addChild(new Text([
			"needle one",
			"middle",
			"needle two",
			"filler",
			"┌────────────────────────────────────────┐",
			"│ box                                    │",
			"└────────────────────────────────────────┘",
			"end",
		].join("\n"), 0, 0));
		tui.start();
		await wait(terminal);
		terminal.sendInput("\x1b[102;6u");
		terminal.sendInput("needle");
		await wait(terminal);
		const before = visible(terminal);
		assert.ok(before.some((line) => line.includes("1/2")));
		const boxBottomRow = before.findIndex((line) => line.startsWith("└"));
		assert.ok(boxBottomRow >= 0);
		const click = `\x1b[<0;24;${boxBottomRow + 1}M`;
		terminal.sendInput(click);
		await wait(terminal);
		const after = visible(terminal);
		assert.ok(after.some((line) => line.includes("1/2")));
		assert.ok(!after.some((line) => line.includes("2/2")));
		tui.stop();
		scenarios.push({
			name: "search-boxdrawing-not-buttons",
			dimensions: { columns: 80, rows: 10 },
			inputs: { transcript: ["needle one", "middle", "needle two", "filler", "┌────────────────────────────────────────┐", "│ box                                    │", "└────────────────────────────────────────┘", "end"], query: "needle", click },
			expected: { before, after },
		});
	}

	{
		const terminal = new RecordingTerminal(60, 4);
		const tui = new TuiAltScreen(terminal, undefined, undefined, {
			searchMatchStyle: (text: string) => `\x1b[41m${text}\x1b[49m`,
			searchCurrentMatchStyle: (text: string) => `\x1b[42m${text}\x1b[49m`,
		});
		tui.addChild(new Text("needle first\nmiddle\nneedle second\nend", 0, 0));
		tui.start();
		await wait(terminal);
		terminal.sendInput("\x1b[102;6u");
		terminal.sendInput("needle");
		await wait(terminal);
		const highlightWrites = terminal.writes.filter((write) => write.includes("\x1b[42mneedle\x1b[49m") || write.includes("\x1b[41mneedle\x1b[49m"));
		assert.ok(highlightWrites.some((write) => write.includes("\x1b[42mneedle\x1b[49m")));
		assert.ok(highlightWrites.some((write) => write.includes("\x1b[41mneedle\x1b[49m")));
		tui.stop();
		scenarios.push({
			name: "search-styled-highlights",
			dimensions: { columns: 60, rows: 4 },
			inputs: { transcript: ["needle first", "middle", "needle second", "end"], query: "needle", currentStyle: "\\x1b[42m…\\x1b[49m", matchStyle: "\\x1b[41m…\\x1b[49m" },
			expected: { writes: highlightWrites },
		});
	}

	{
		const terminal = new RecordingTerminal(30, 5);
		const tui = new TuiAltScreen(terminal);
		const text = new Text("line 1\nneedle first\nline 3\nneedle second\nline 5", 0, 0);
		tui.addChild(text);
		tui.start();
		await wait(terminal);
		terminal.sendInput("\x1b[102;6u");
		terminal.sendInput("needle");
		await wait(terminal);
		const phases: any[] = [{ action: "initial", screen: visible(terminal) }];
		text.setText("line 1\nneedle first\nline 3\nneedle second\nline 5\nneedle third\nline 7");
		tui.requestRender();
		await wait(terminal);
		phases.push({ action: "append", screen: visible(terminal) });
		terminal.resize(40, 6);
		await wait(terminal);
		phases.push({ action: "resize", screen: visible(terminal) });
		tui.stop();
		scenarios.push({
			name: "search-streaming-resize",
			dimensions: { columns: 30, rows: 5 },
			inputs: { initialLines: 5, appendedLine: "needle third", resizedDimensions: { columns: 40, rows: 6 }, query: "needle" },
			expected: { phases },
		});
	}

	return {
		sourceEndpoints: [
			"packages/tui/src/alt-screen-search.ts:buildSearchCorpus,findSearchCorpusMatches,AltScreenSearchIndex,AltScreenSearchComponent.render",
			"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.handleViewportInput,TuiAltScreen.toggleSearch,TuiAltScreen.refreshSearch,TuiAltScreen.applySearchHighlights,TuiAltScreen.doRender",
			"packages/tui/src/keybindings.ts:TUI_KEYBINDINGS,KeybindingsManager",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.sendInput,VirtualTerminal.resize,VirtualTerminal.waitForRender",
		],
		scenarios,
	};
}
