import { pathToFileURL } from "node:url";
import path from "node:path";

/// Independent pi-v1.0.4 observations of fullscreen mouse selection and copy
/// lifecycle (#1006) and frozen multi-click word/line selection (#1007). The
/// C++ replay reads these values from the named bundle; no Pike output is used
/// to construct the expectations.
///
/// Word segments come from pi's `Intl.Segmenter` (ICU word-break dictionaries
/// and rules). The C++ side reuses the pinned vcpkg ICU breaker
/// (`cch::tui::detail::segment_words`, UBRK_WORD), the same engine class, so
/// the corpus covers both the rule-based classes (letters, digits, `/` `-` `_`
/// `.` `,` `:` `'`, spaces, Katakana runs, Hangul runs) and the multi-character
/// Han/Hiragana/Thai dictionary runs.
export async function captureSelection(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	const { stripTerminalSequences, visibleWidth } = await import(source("src/utils.ts"));

	const sourceEndpoints = [
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.handleSelectionMouseEvent,getActiveSelectionText,copySelectionToClipboard,copyTextToClipboard,hasActiveSelection,copyActiveSelectionToClipboard,setCopyOnSelect,getClickCount,getWordSelection,getLineSelection,updateSelectionFocus,handleScrollbarMouseEvent,handleViewportInput",
		"packages/tui/src/components/text.ts:Text.render,setText",
		"packages/tui/src/components/scroll-view.ts:ScrollView.scrollTo",
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
	const sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));
	/// Visible column of `token` in a rendered row: UTF-16 indexes are not cell
	/// columns once a wide character precedes the token.
	const columnOf = (row: string, token: string, from = 0) =>
		visibleWidth(stripTerminalSequences(row).slice(0, stripTerminalSequences(row).indexOf(token, from)));
	const sgr = (code: number, x: number, y: number, release = false) => `\x1b[<${code};${x};${y}${release ? "m" : "M"}`;
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
	/// Execute #1007 multi-click steps against frozen pi and record one
	/// observation per `observe` (selection state and the accumulated copies)
	/// and one viewport snapshot per `observeViewport`. The recorded `steps`
	/// with their `advanceMs` clock deltas are the replay input.
	const runSteps = async (
		terminal: any,
		tui: any,
		steps: any[],
		copied: string[],
		observations: any[],
		viewports?: string[][],
	) => {
		for (const step of steps) {
			switch (step.action) {
				case "press":
					if (step.advanceMs > 0) await sleep(step.advanceMs);
					terminal.sendInput(sgr(0, step.x, step.y));
					break;
				case "release":
					terminal.sendInput(sgr(0, step.x, step.y, true));
					break;
				case "motion":
					terminal.sendInput(`\x1b[<32;${step.x};${step.y}M`);
					break;
				case "focusOut":
					terminal.sendInput("\x1b[O");
					break;
				case "focusIn":
					terminal.sendInput("\x1b[I");
					break;
				case "observe":
					observations.push({ hasSelection: tui.hasActiveSelection(), copied: [...copied] });
					break;
				case "observeViewport":
					viewports?.push(visibleLines(terminal.getViewport()));
					break;
			}
			await terminal.waitForRender();
		}
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

	// Repeated presses on one word cycle the granularity character -> word ->
	// line and wrap back to word (pi `getClickCount`, `DOUBLE_CLICK_INTERVAL_MS`).
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(30, 4, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("alpha beta gamma", 0, 0));
		tui.start();
		await terminal.waitForRender();
		const betaX = columnOf(terminal.getViewport()[0], "beta") + 1;
		const steps: any[] = [];
		for (let click = 0; click < 5; click++) {
			steps.push(
				{ action: "press", x: betaX, y: 1, advanceMs: click === 0 ? 0 : 150, target: "beta" },
				{ action: "release", x: betaX, y: 1 },
				{ action: "observe" },
			);
		}
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-word-line-cycle",
			dimensions: { columns: 30, rows: 4 },
			inputs: { text: "alpha beta gamma", steps },
			expected: { observations },
		});
		tui.stop();
	}

	// Word granularity merges joiner-adjacent segments (`/`, `-`) and the
	// rule-based classes; punctuation and spaces select only themselves.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(44, 6, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		const rows = ["src/main-component util", "a.b ee_ff 3.14 カタカナ", "中x foo,bar"];
		tui.addChild(new Text(rows.join("\n"), 0, 0));
		tui.start();
		await terminal.waitForRender();
		const targets = [
			{ row: 0, token: "main", note: "joiners merge left and right" },
			{ row: 0, token: "util", note: "plain word" },
			{ row: 1, token: "a.b", note: "MidNumLet joins letters" },
			{ row: 1, token: "ee_ff", note: "ExtendNumLet joins letters" },
			{ row: 1, token: "3.14", note: "MidNumLet joins digits" },
			{ row: 1, token: "カタカナ", note: "Katakana run" },
			{ row: 2, token: "中", note: "single dictionary-script character" },
			{ row: 2, token: "x", note: "letter after a wide character", from: "中" },
			{ row: 2, token: ",", note: "punctuation selects itself" },
		];
		const steps: any[] = [];
		for (const target of targets) {
			const rendered = stripTerminalSequences(terminal.getViewport()[target.row]);
			const x = columnOf(rendered, target.token, target.from ? rendered.indexOf(target.from) + 1 : 0) + 1;
			steps.push(
				{ action: "press", x, y: target.row + 1, advanceMs: 400, target: `${target.token} (${target.note})` },
				{ action: "release", x, y: target.row + 1 },
				{ action: "press", x, y: target.row + 1, advanceMs: 150, target: `${target.token} (second)` },
				{ action: "release", x, y: target.row + 1 },
				{ action: "observe" },
			);
		}
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-word-joiners-and-classes",
			dimensions: { columns: 44, rows: 6 },
			inputs: { text: rows.join("\n"), steps },
			expected: { observations },
		});
		tui.stop();
	}

	// A drag that started from a word press expands by whole words, forward and
	// backward (pi `updateSelectionFocus` with `selectionGranularity: "word"`).
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(40, 4, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("one two three four", 0, 0));
		tui.start();
		await terminal.waitForRender();
		const row = stripTerminalSequences(terminal.getViewport()[0]);
		const twoX = columnOf(row, "two") + 1;
		const fourX = columnOf(row, "four") + 1;
		const threeX = columnOf(row, "three") + 1;
		const steps: any[] = [
			// Double-click "two" then drag to "four": expands forward by words.
			{ action: "press", x: twoX, y: 1, advanceMs: 400, target: "two" },
			{ action: "release", x: twoX, y: 1 },
			{ action: "press", x: twoX, y: 1, advanceMs: 150, target: "two (second)" },
			{ action: "motion", x: fourX, y: 1, target: "four" },
			{ action: "release", x: fourX, y: 1 },
			{ action: "observe" },
			// Double-click "four" then drag back to "one": the anchor swaps to
			// the initial range end, so the whole span stays selected.
			{ action: "press", x: fourX, y: 1, advanceMs: 400, target: "four" },
			{ action: "release", x: fourX, y: 1 },
			{ action: "press", x: fourX, y: 1, advanceMs: 150, target: "four (second)" },
			{ action: "motion", x: 1, y: 1, target: "one" },
			{ action: "release", x: 1, y: 1 },
			{ action: "observe" },
			// A single press still drags by characters between two words.
			{ action: "press", x: twoX, y: 1, advanceMs: 600, target: "two" },
			{ action: "motion", x: threeX, y: 1, target: "three" },
			{ action: "release", x: threeX, y: 1 },
			{ action: "observe" },
		];
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-word-drag-expansion",
			dimensions: { columns: 40, rows: 4 },
			inputs: { text: "one two three four", steps },
			expected: { observations },
		});
		tui.stop();
	}

	// Line granularity selects the rendered screen row, so a wrapped logical
	// line contributes row by row; presses on different wrapped rows of one
	// logical line do not count as one multi-click.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(10, 4, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("abcdefghij klmnop", 0, 0));
		tui.start();
		await terminal.waitForRender();
		const steps: any[] = [
			{ action: "press", x: 3, y: 1, advanceMs: 400, target: "first wrapped row" },
			{ action: "release", x: 3, y: 1 },
			{ action: "press", x: 3, y: 1, advanceMs: 150 },
			{ action: "release", x: 3, y: 1 },
			{ action: "press", x: 3, y: 1, advanceMs: 150 },
			{ action: "release", x: 3, y: 1 },
			{ action: "observe" },
			{ action: "press", x: 3, y: 2, advanceMs: 400, target: "second wrapped row" },
			{ action: "release", x: 3, y: 2 },
			{ action: "press", x: 3, y: 2, advanceMs: 150 },
			{ action: "release", x: 3, y: 2 },
			{ action: "observe" },
			// Same logical line, different screen rows: each press is a fresh
			// single click, and only the repeated press on the same row counts.
			{ action: "press", x: 3, y: 1, advanceMs: 400, target: "row one again" },
			{ action: "release", x: 3, y: 1 },
			{ action: "observe" },
			{ action: "press", x: 3, y: 2, advanceMs: 150, target: "row two" },
			{ action: "release", x: 3, y: 2 },
			{ action: "observe" },
			{ action: "press", x: 3, y: 2, advanceMs: 150, target: "row two (second)" },
			{ action: "release", x: 3, y: 2 },
			{ action: "observe" },
		];
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-line-wrapped-rows",
			dimensions: { columns: 10, rows: 4 },
			inputs: { text: "abcdefghij klmnop", steps },
			expected: { observations },
		});
		tui.stop();
	}

	// Presses more than the double-click interval apart restart the count; a
	// press on a different word restarts it too. The interval clock refreshes
	// even when the intervening press expired.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(30, 4, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("alpha beta", 0, 0));
		tui.start();
		await terminal.waitForRender();
		const betaX = columnOf(terminal.getViewport()[0], "beta") + 1;
		const alphaX = columnOf(terminal.getViewport()[0], "alpha") + 1;
		const steps: any[] = [
			{ action: "press", x: betaX, y: 1, advanceMs: 400, target: "beta" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: betaX, y: 1, advanceMs: 650, target: "beta after 650ms" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: betaX, y: 1, advanceMs: 150, target: "beta again" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			// The exact 500 ms boundary (`now - at <= 500` counts up, 501 resets) is
			// pinned by the deterministic-clock AltScreen unit test: this runner
			// sleeps wall-clock time, whose overshoot cannot land exactly on the
			// inclusive boundary, so a capture step here would record noise.
			{ action: "press", x: alphaX, y: 1, advanceMs: 150, target: "alpha, a different word" },
			{ action: "release", x: alphaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: alphaX, y: 1, advanceMs: 150, target: "alpha (second)" },
			{ action: "release", x: alphaX, y: 1 },
			{ action: "observe" },
		];
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-expiry-and-word-identity",
			dimensions: { columns: 30, rows: 4 },
			inputs: { text: "alpha beta", steps },
			expected: { observations },
		});
		tui.stop();
	}

	// Focus loss cancels a pending multi-click: the click history clears, an
	// unfinished press dies with the gesture, and the next press starts at one.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(30, 4, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		tui.addChild(new Text("alpha beta", 0, 0));
		tui.start();
		await terminal.waitForRender();
		const betaX = columnOf(terminal.getViewport()[0], "beta") + 1;
		const steps: any[] = [
			{ action: "press", x: betaX, y: 1, advanceMs: 400, target: "beta" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "focusOut" },
			{ action: "focusIn" },
			{ action: "press", x: betaX, y: 1, advanceMs: 150, target: "beta after focus return" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: betaX, y: 1, advanceMs: 150, target: "beta (second)" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			// A press that never released dies with the focus loss.
			{ action: "press", x: betaX, y: 1, advanceMs: 400, target: "beta (held)" },
			{ action: "focusOut" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: betaX, y: 1, advanceMs: 150, target: "beta after held press" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: betaX, y: 1, advanceMs: 150, target: "beta (second after held)" },
			{ action: "release", x: betaX, y: 1 },
			{ action: "observe" },
		];
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-focus-out-cancels",
			dimensions: { columns: 30, rows: 4 },
			inputs: { text: "alpha beta", steps },
			expected: { observations },
		});
		tui.stop();
	}

	// A scrollbar press cancels a pending multi-click. The layout viewport uses
	// an always-visible bar so the track column accepts presses without a hover
	// reveal; the track presses also pin the frozen scroll-to-pointer mapping.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(30, 3, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		const layoutText = "one two\nthree four\nfive six\nseven eight\nnine ten\neleven";
		tui.setLayoutRoot(
			new ScrollView(new Text(layoutText, 0, 0), {
				follow: "none",
				primary: true,
				scrollbar: "always",
				scrollbarHideDelayMs: 0,
			}),
		);
		tui.start();
		await terminal.waitForRender();
		const trackX = 30;
		const twoX = columnOf(terminal.getViewport()[0], "two") + 1;
		const steps: any[] = [
			{ action: "press", x: trackX, y: 3, advanceMs: 400, target: "track bottom" },
			{ action: "release", x: trackX, y: 3 },
			{ action: "observeViewport" },
			{ action: "press", x: trackX, y: 1, advanceMs: 400, target: "track top" },
			{ action: "release", x: trackX, y: 1 },
			{ action: "observeViewport" },
			{ action: "press", x: twoX, y: 1, advanceMs: 400, target: "two" },
			{ action: "release", x: twoX, y: 1 },
			{ action: "observe" },
			// Grabbing the thumb scrolls nowhere: the viewport must stay put for
			// the cancellation clicks below to land on the same word.
			{ action: "press", x: trackX, y: 1, advanceMs: 150, target: "thumb grab" },
			{ action: "release", x: trackX, y: 1 },
			{ action: "observeViewport" },
			{ action: "press", x: twoX, y: 1, advanceMs: 150, target: "two after scrollbar" },
			{ action: "release", x: twoX, y: 1 },
			{ action: "observe" },
			{ action: "press", x: twoX, y: 1, advanceMs: 150, target: "two (second)" },
			{ action: "release", x: twoX, y: 1 },
			{ action: "observe" },
		];
		const observations: any[] = [];
		const viewports: string[][] = [];
		await runSteps(terminal, tui, steps, copied, observations, viewports);
		scenarios.push({
			name: "multiclick-scrollbar-cancels",
			dimensions: { columns: 30, rows: 3 },
			inputs: {
				layoutText,
				layoutScroll: { follow: "none", primary: true, scrollbar: "always", scrollbarHideDelayMs: 0 },
				steps,
			},
			expected: { observations, viewports },
		});
		tui.stop();
	}

	// Double-clicking inside a multi-character Han, Hiragana or Thai run selects
	// the whole ICU word segment (`Intl.Segmenter`), not one character. The C++
	// side reuses the pinned ICU breaker, the same engine class, so the replay
	// compares whole-run copies against frozen pi.
	{
		const copied: string[] = [];
		const { terminal, tui } = makeHost(40, 5, {
			copySelection: async (text: string) => {
				copied.push(text);
				return true;
			},
		});
		const text = "日本語\nこんにちは\nสวัสดีครับ";
		tui.addChild(new Text(text, 0, 0));
		tui.start();
		await terminal.waitForRender();
		const targets = [
			{ row: 0, token: "日本語", label: "han run" },
			{ row: 1, token: "こんにちは", label: "hiragana run" },
			{ row: 2, token: "สวัสดี", label: "thai first word" },
			{ row: 2, token: "ครับ", label: "thai second word" },
		];
		const steps: any[] = [];
		for (const target of targets) {
			const x = columnOf(terminal.getViewport()[target.row], target.token) + 2; // Interior cell of the run.
			steps.push(
				{ action: "press", x, y: target.row + 1, advanceMs: 600, target: `${target.label} first` },
				{ action: "release", x, y: target.row + 1 },
				{ action: "observe" },
				{ action: "press", x, y: target.row + 1, advanceMs: 150, target: `${target.label} second` },
				{ action: "release", x, y: target.row + 1 },
				{ action: "observe" },
			);
		}
		const observations: any[] = [];
		await runSteps(terminal, tui, steps, copied, observations);
		scenarios.push({
			name: "multiclick-dictionary-script-runs",
			dimensions: { columns: 40, rows: 5 },
			inputs: { text, steps },
			expected: { observations },
		});
		tui.stop();
	}

	return { sourceEndpoints, scenarios };
}
