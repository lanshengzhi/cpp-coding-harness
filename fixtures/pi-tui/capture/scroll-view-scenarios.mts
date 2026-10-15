import { pathToFileURL } from "node:url";
import path from "node:path";
import assert from "node:assert/strict";

/// Independent pi-v1.0.4 observation of scroll viewports (#1000).
///
/// Every value below comes from pi's own modules: `ScrollView` for the scroll
/// algebra (`scrollTo`/`scrollBy`/`updateLayout`), `renderLayoutFrame` for the
/// constrained frame, pi's `visibleLines` convention (`test/layout.test.ts:10-12`:
/// strip terminal sequences and trailing padding) for what the terminal shows,
/// `getScrollViewsAt` for the viewport order a wheel delta is offered to, and a
/// real `TuiAltScreen` over pi's `VirtualTerminal` for the implicit viewport a
/// child mounted without a layout root gets. The C++ replay reads the artifact
/// through `scripts/tui/evidence.py`; nothing here is derived from the C++ side.
export async function captureScrollView(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { HStack } = await import(source("src/components/h-stack.ts"));
	const { VStack } = await import(source("src/components/v-stack.ts"));
	const { getScrollViewsAt, renderLayoutFrame } = await import(source("src/layout.ts"));
	const { WheelScrollAccelerator } = await import(source("src/wheel-scroll.ts"));
	const { KeybindingsManager, TUI_KEYBINDINGS, getKeybindings, setKeybindings } = await import(source("src/keybindings.ts"));
	const { encodeKitty, getKittyImageMetadata, registerKittyImageMetadata } = await import(source("src/terminal-image.ts"));
	const { stripTerminalSequences } = await import(source("src/utils.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	const visibleLines = (lines: readonly string[]) => lines.map((line) => stripTerminalSequences(line).trimEnd());
	const numberedText = (count: number) =>
		Array.from({ length: count }, (_, index) => `${index + 1}`).join("\n");

	/// The painted frame of a scroll-viewport root: what the terminal shows, the
	/// viewport's allocated window and the content height the layout gave the
	/// viewport's single child.
	const frameOf = (root: any, columns: number, rows: number) => {
		const painted = renderLayoutFrame(root, columns, rows, () => {});
		const findScrollBox = (box: any): any => {
			if (!box) return undefined;
			if (box.scrollView) return box;
			for (const child of box.children ?? []) {
				const found = findScrollBox(child);
				if (found) return found;
			}
			return undefined;
		};
		const scrollBox = findScrollBox(painted.root);
		return {
			lines: visibleLines(painted.lines),
			window: scrollBox
				? {
						height: scrollBox.rect.height,
						contentHeight: scrollBox.children[0]?.rect.height ?? 0,
						imageRows: (scrollBox.scrollContentLines ?? []).length,
					}
				: null,
			painted,
		};
	};

	/// The placement geometry a painted image line carries after pi's crop pass:
	/// the source rectangle (`y`, `h`) and the rows it is painted over (`r`).
	const cropOf = (line: string) => {
		const controls = /\x1b_G([^;]*);/.exec(line)?.[1];
		const metadata = getKittyImageMetadata(line);
		if (controls === undefined || !metadata) return null;
		const value = (key: string) => {
			const found = new RegExp(`(?:^|,)${key}=(\\d+)(?:,|$)`).exec(controls);
			return found === undefined || found === null ? null : Number.parseInt(found[1]!, 10);
		};
		return {
			y: value("y"),
			h: value("h"),
			r: value("r"),
			registered: { columns: metadata.columns, rows: metadata.rows, widthPx: metadata.widthPx, heightPx: metadata.heightPx },
		};
	};

	const scenarios: any[] = [];

	// `wheel-scroll.ts:35-74` distinguishes fixed counts, event bursts,
	// velocity scaling, direction changes, and pauses using explicit times.
	{
		const observe = (accelerator: InstanceType<typeof WheelScrollAccelerator>, times: number[], directions?: number[]) =>
			times.map((time, index) => accelerator.next((directions?.[index] ?? 1) as -1 | 1, time));
		const fixed = observe(new WheelScrollAccelerator(3, true), [0, 10, 20, 1000]);
		const accelerated = observe(new WheelScrollAccelerator("auto", true), [0, 50, 100, 103, 123, 373]);
		const directionReset = observe(new WheelScrollAccelerator("auto", true), [0, 20, 40, 60, 500, 520], [1, 1, 1, -1, 1, 1]);
		const fractionalCarry = observe(new WheelScrollAccelerator("auto", true), [0, 40, 80, 120, 160]);
		scenarios.push({
			name: "wheel-accelerator",
			dimensions: { columns: 10, rows: 3 },
			inputs: {
				fixed: { lines: 3, times: [0, 10, 20, 1000] },
				auto: { lines: "auto", times: [0, 50, 100, 103, 123, 373] },
				directionReset: { lines: "auto", times: [0, 20, 40, 60, 500, 520], directions: [1, 1, 1, -1, 1, 1] },
				fractionalCarry: { lines: "auto", times: [0, 40, 80, 120, 160] },
			},
			expected: { fixed, accelerated, directionReset, fractionalCarry },
		});
	}

	// `scroll-view.ts:127-201` over a constrained frame (pi
	// `test/layout.test.ts:170-189`): the first frame follows the content end, an
	// upward delta leaves it and reports what it could not consume, a downward
	// delta reaches the end again and re-enables following.
	{
		const view = new ScrollView(new Text(numberedText(6), 0, 0), { follow: "end", primary: true });
		const first = frameOf(view, 10, 3);
		const steps: any[] = [
			{
				action: "initial",
				remaining: null,
				scrollTop: view.scrollTop,
				followingEnd: view.isFollowingEnd,
				viewportHeight: view.viewportHeight,
				lines: first.lines,
			},
		];
		for (const [action, requested] of [
			["scroll-by-minus-2", -2],
			["scroll-by-minus-3", -3],
			["scroll-by-plus-10", 10],
		] as Array<[string, number]>) {
			const remaining = view.scrollBy(requested);
			steps.push({
				action,
				remaining,
				scrollTop: view.scrollTop,
				followingEnd: view.isFollowingEnd,
				viewportHeight: view.viewportHeight,
				lines: frameOf(view, 10, 3).lines,
			});
		}
		scenarios.push({
			name: "follow-transitions",
			dimensions: { columns: 10, rows: 3 },
			inputs: {
				contentLines: 6,
				options: { follow: "end", primary: true },
				requests: [null, -2, -3, 10],
			},
			expected: { window: first.window, steps },
		});
	}

	// Appending to followed content moves the window with the new end (pi
	// `test/layout.test.ts:250-258`).
	{
		const text = new Text(numberedText(4), 0, 0);
		const view = new ScrollView(text, { follow: "end" });
		const first = frameOf(view, 6, 3);
		const before = {
			scrollTop: view.scrollTop,
			followingEnd: view.isFollowingEnd,
			viewportHeight: view.viewportHeight,
			lines: first.lines,
		};
		text.setText(numberedText(5));
		const grown = frameOf(view, 6, 3);
		const after = {
			scrollTop: view.scrollTop,
			followingEnd: view.isFollowingEnd,
			viewportHeight: view.viewportHeight,
			lines: grown.lines,
		};
		scenarios.push({
			name: "follow-append",
			dimensions: { columns: 6, rows: 3 },
			inputs: { contentLines: 4, appendedLines: 5, options: { follow: "end" } },
			expected: { window: first.window, before, after },
		});
	}

	// An absolute `scrollTo` with `disableFollow` holds the window at the last
	// row without resuming follow (pi `scroll-view.ts:127-146`, the
	// `followSuppressedAtEnd` field), so rows appended after it leave the window
	// where it was and only a plain scroll that reaches the end follows again.
	// The frozen `test/layout.test.ts` cases scroll by delta and never
	// distinguish the suppression from a followed end.
	{
		const text = new Text(numberedText(6), 0, 0);
		const view = new ScrollView(text, { follow: "end" });
		const first = frameOf(view, 10, 3);
		const observe = () => {
			const frame = frameOf(view, 10, 3);
			return {
				scrollTop: view.scrollTop,
				followingEnd: view.isFollowingEnd,
				viewportHeight: view.viewportHeight,
				contentHeight: frame.window?.contentHeight ?? 0,
				lines: frame.lines,
			};
		};
		const phases: any[] = [{ action: "initial", ...observe() }];
		const actions = [
			{ action: "scroll-to-1-with-disable-follow", target: 1, disableFollow: true },
			{ action: "scroll-to-last-row-with-disable-follow", target: 3, disableFollow: true },
		];
		for (const step of actions) {
			view.scrollTo(step.target, { disableFollow: step.disableFollow });
			phases.push({ action: step.action, ...observe() });
		}
		text.setText(numberedText(8));
		phases.push({ action: "appended-while-suppressed", ...observe() });
		const restoring = { action: "scroll-to-last-row", target: 5, disableFollow: false };
		view.scrollTo(restoring.target);
		phases.push({ action: restoring.action, ...observe() });
		scenarios.push({
			name: "scroll-to-disable-follow",
			dimensions: { columns: 10, rows: 3 },
			inputs: {
				contentLines: 6,
				appendedLines: 8,
				options: { follow: "end" },
				actions: [{ action: "initial", target: null, disableFollow: false }, ...actions,
					{ action: "appended-while-suppressed", target: null, disableFollow: false }, restoring],
			},
			expected: { window: first.window, phases },
		});
	}

	// Nested viewports (pi `test/layout.test.ts:346-353`): the inner viewport is
	// allocated its stack basis, an outer delta the inner cannot consume is what
	// the enclosing viewport receives, and `getScrollViewsAt`
	// (`layout.ts:439-449`) orders the viewports under a point deepest first.
	{
		const inner = new ScrollView(new Text(numberedText(6), 0, 0));
		const outer = new ScrollView(new VStack([{ component: inner, basis: 2 }, new Text("tail", 0, 0)]));
		const first = frameOf(outer, 10, 2);
		const before = {
			innerViewportHeight: inner.viewportHeight,
			innerScrollTop: inner.scrollTop,
			outerScrollTop: outer.scrollTop,
			outerViewportHeight: outer.viewportHeight,
			lines: first.lines,
		};
		const remaining = outer.scrollBy(10);
		const second = frameOf(outer, 10, 2);
		const after = { outerScrollTop: outer.scrollTop, remaining, lines: second.lines };
		const wide = frameOf(outer, 10, 3);
		const probes = [
			{ x: 0, y: 0 },
			{ x: 0, y: 2 },
			{ x: 10, y: 0 },
		];
		scenarios.push({
			name: "nested-scroll",
			dimensions: { columns: 10, rows: 2 },
			inputs: {
				inner: { contentLines: 6, viewportRows: 2 },
				outer: { innerBasis: 2, tail: "tail" },
				probeDimensions: { columns: 10, rows: 3 },
				probes,
			},
			expected: {
				window: first.window,
				before,
				after,
				viewportsAt: probes.map((probe) => ({
					...probe,
					depthFirst: getScrollViewsAt(wide.painted, probe.x, probe.y).map((view: any) =>
						view === inner ? "inner" : view === outer ? "outer" : "unknown",
					),
				})),
			},
		});
	}

	// A viewport crops an image placement at the window's edges (pi
	// `layout.ts:340-344` for the lower boundary, `layout.ts:359-373` for the
	// scrolled one) and keeps the rows it still shows.
	{
		const imageId = 124;
		registerKittyImageMetadata({ imageId, columns: 2, rows: 3, widthPx: 100, heightPx: 100 });
		const imageLine = encodeKitty("AAAA", { columns: 2, rows: 3, imageId, moveCursor: false });

		// Lower boundary: the transcript is allocated three rows below a dock row
		// and the placement starts on its third row (pi `test/layout.test.ts:131-148`).
		const transcript = new ScrollView({
			render: () => ["one", "two", imageLine, "", ""],
			invalidate: () => {},
		});
		const docked = renderLayoutFrame(
			new VStack([{ component: transcript, basis: 0, grow: 1 }, new Text("dock", 0, 0)]),
			20,
			4,
			() => {},
		);
		const lowerRow = docked.lines.findIndex((line) => cropOf(line) !== null);
		const lower = {
			scrollTop: transcript.scrollTop,
			viewportHeight: transcript.viewportHeight,
			placementRow: 2,
			cropRow: lowerRow,
			placedRows: 3,
			crop: cropOf(docked.lines[lowerRow] ?? ""),
		};

		// Scrolled: the placement sits on the first content row and the window
		// follows the end two rows further down, so its remaining row is painted
		// at the window's first row.
		const scrolled = new ScrollView({ render: () => [imageLine, "", "", "turns-4", "turns-5"], invalidate: () => {} }, { follow: "end" });
		const scrolledFrame = renderLayoutFrame(scrolled, 20, 3, () => {});
		const upperRow = scrolledFrame.lines.findIndex((line) => cropOf(line) !== null);
		const upper = {
			scrollTop: scrolled.scrollTop,
			viewportHeight: scrolled.viewportHeight,
			placementRow: 0,
			cropRow: upperRow,
			placedRows: 3,
			crop: cropOf(scrolledFrame.lines[upperRow] ?? ""),
		};

		scenarios.push({
			name: "scroll-image-crop",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				placement: { columns: 2, rows: 3, widthPx: 100, heightPx: 100 },
				lowerBoundary: { transcriptLines: 5, dock: "dock", viewportRows: 3 },
				scrolled: { transcriptLines: 5, viewportRows: 3, follow: "end" },
			},
			expected: { lower, upper },
		});
	}

	// A real alternate-screen wheel report over the nested viewport consumes two
	// rows there and chains its one-row remainder to the outer viewport. The
	// side-by-side viewport under the same frame remains stationary.
	{
		const terminal = new VirtualTerminal(30, 3);
		const previousKeybindings = getKeybindings();
		setKeybindings(new KeybindingsManager(TUI_KEYBINDINGS));
		try {
			const tui = new TuiAltScreen(terminal, undefined, undefined, { wheelScrollLines: 5 });
			const inner = new ScrollView(new Text(numberedText(4), 0, 0));
			const leftContent = new VStack([
				{ component: new Text("head", 0, 0), basis: 1 },
				{ component: inner, basis: 2 },
				{ component: new Text("tail", 0, 0), basis: 5 },
			]);
			const outer = new ScrollView(leftContent, { primary: true });
			const unrelated = new ScrollView(new Text(numberedText(10), 0, 0));
			tui.setLayoutRoot(new HStack([
				{ component: outer, basis: 15 },
				{ component: unrelated, basis: 15 },
			]));
			tui.start();
			await terminal.waitForRender();
			const before = { inner: inner.scrollTop, outer: outer.scrollTop, unrelated: unrelated.scrollTop };
			terminal.sendInput("\x1b[<65;2;2M");
			await terminal.waitForRender();
			const after = { inner: inner.scrollTop, outer: outer.scrollTop, unrelated: unrelated.scrollTop };
			assert.deepStrictEqual(before, { inner: 0, outer: 0, unrelated: 0 });
			assert.deepStrictEqual(after, { inner: 2, outer: 3, unrelated: 0 });
			tui.stop();
			scenarios.push({
				name: "nested-wheel-chaining",
				dimensions: { columns: 30, rows: 3 },
				inputs: { wheelReport: "\x1b[<65;2;2M", wheelDelta: 5, viewportRows: { inner: 2, outer: 3 } },
				expected: { before, after },
			});
		} finally {
			setKeybindings(previousKeybindings);
		}
	}

	// A real alternate-screen input stream proves Ctrl+Home/End run before the
	// focused editor, releases do not change the viewport, and the jump label
	// resumes follow-end when clicked.
	{
		const terminal = new VirtualTerminal(20, 4);
		const previousKeybindings = getKeybindings();
		setKeybindings(new KeybindingsManager(TUI_KEYBINDINGS));
		try {
			const tui = new TuiAltScreen(terminal, undefined, undefined, {
				scrollToEndIndicator: () => "Jump end",
			});
			const transcript = new ScrollView(new Text(numberedText(12), 0, 0), { follow: "end", primary: true });
		const editorInputs: string[] = [];
		const editor = {
			focused: false,
			render: () => ["editor"],
			invalidate: () => {},
			handleInput: (data: string) => editorInputs.push(data),
		};
			tui.setLayoutRoot(new VStack([
				{ component: transcript, basis: 0, grow: 1, minSize: 1 },
				{ component: editor, basis: 1, shrink: 0 },
			]));
			tui.setFocus(editor);
			tui.start();
			await terminal.waitForRender();
			const observe = () => ({
			scrollTop: transcript.scrollTop,
			followingEnd: transcript.isFollowingEnd,
			lines: terminal.getViewport().map((line: string) => line.trimEnd()),
		});
			const phases: any[] = [{ action: "initial", ...observe() }];
			terminal.sendInput("\x1bOH");
			terminal.sendInput("\x1b[F");
			terminal.sendInput("\x1b[57423u");
			terminal.sendInput("\x1b[5;5~");
			terminal.sendInput("\x1b[6;5~");
			await terminal.waitForRender();
			phases.push({ action: "unmodified-navigation-remains-with-editor", ...observe() });
			terminal.sendInput("\x1b[1;5H");
			await terminal.waitForRender();
			phases.push({ action: "ctrl-home", ...observe() });
			terminal.sendInput("\x1b[1;5F");
			await terminal.waitForRender();
			phases.push({ action: "ctrl-end", ...observe() });
			terminal.sendInput("\x1b[57423;5:3u");
			await terminal.waitForRender();
			phases.push({ action: "ctrl-home-release", ...observe() });
			terminal.sendInput("\x1b[<64;1;1M");
			await terminal.waitForRender();
			phases.push({ action: "wheel-up-indicator", ...observe() });
			terminal.sendInput("\x1b[<0;7;3M");
			await terminal.waitForRender();
			phases.push({ action: "indicator-click", ...observe() });
			assert.deepStrictEqual(editorInputs, ["\x1bOH", "\x1b[F", "\x1b[57423u", "\x1b[5;5~", "\x1b[6;5~"]);
			const phaseTop = (name: string) => phases.find((phase) => phase.action === name)?.scrollTop;
			assert.strictEqual(phaseTop("ctrl-home"), 0);
			assert.strictEqual(phaseTop("ctrl-end"), 9);
			assert.strictEqual(phaseTop("wheel-up-indicator"), 8);
			assert.strictEqual(phaseTop("indicator-click"), 9);
			tui.stop();
			scenarios.push({
				name: "alt-screen-navigation-indicator",
				dimensions: { columns: 20, rows: 4 },
				inputs: { navigation: ["\x1bOH", "\x1b[F", "\x1b[57423u", "\x1b[5;5~", "\x1b[6;5~", "\x1b[1;5H", "\x1b[1;5F", "\x1b[57423;5:3u"], wheelReport: "\x1b[<64;1;1M", indicatorPress: "\x1b[<0;7;3M", indicatorWidth: 9 },
				expected: { phases, editorInputs },
			});
		} finally {
			setKeybindings(previousKeybindings);
		}
	}

	// A child mounted without a layout root is one document behind the implicit
	// follow-end viewport (pi `tui-alt-screen.ts:262-270,1673-1680`), and the
	// viewport survives content growth, an explicit jump and a resize (pi
	// `test/tui-alt-screen.test.ts:63-90`).
	{
		const terminal = new VirtualTerminal(20, 4);
		const tui = new TuiAltScreen(terminal);
		const text = new Text(numberedText(10), 0, 0);
		tui.addChild(text);
		tui.start();
		await terminal.waitForRender();
		const observe = () => ({
			viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
			viewportTop: tui.viewportTop,
			isFollowingOutput: tui.isFollowingOutput,
		});
		const phases: any[] = [{ action: "initial", ...observe() }];
		tui.scrollBy(-4);
		await terminal.waitForRender();
		phases.push({ action: "scroll-by-minus-4", ...observe() });
		text.setText(numberedText(12));
		tui.requestRender();
		await terminal.waitForRender();
		phases.push({ action: "appended", ...observe() });
		tui.scrollToBottom();
		await terminal.waitForRender();
		phases.push({ action: "scroll-to-bottom", ...observe() });
		terminal.resize(20, 2);
		await terminal.waitForRender();
		phases.push({ action: "resize-to-2-rows", ...observe() });
		tui.scrollToTop();
		await terminal.waitForRender();
		phases.push({ action: "scroll-to-top", ...observe() });
		terminal.resize(20, 20);
		await terminal.waitForRender();
		phases.push({ action: "resize-to-20-rows", ...observe() });
		tui.stop();

		scenarios.push({
			name: "alt-screen-implicit-follow",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				documentLines: 10,
				appendedLines: 12,
				manualDelta: -4,
				resizes: [
					{ columns: 20, rows: 2 },
					{ columns: 20, rows: 20 },
				],
			},
			expected: { phases },
		});
	}

	// A primary scroll view without follow-end never shows the jump-to-end
	// indicator, even scrolled away from the end (pi
	// `test/tui-alt-screen.test.ts:215-229`).
	{
		const terminal = new VirtualTerminal(30, 3);
		const tui = new TuiAltScreen(terminal, undefined, undefined, {
			scrollToEndIndicator: () => " ↓ Jump to end ",
		});
		const transcript = new ScrollView(new Text("one\ntwo\nthree\nfour\nfive", 0, 0), { primary: true });
		tui.setLayoutRoot(transcript);
		tui.start();
		await terminal.waitForRender();
		const observe = () => ({
			scrollTop: transcript.scrollTop,
			followingEnd: transcript.isFollowingEnd,
			indicatorShown: terminal.getViewport().some((line: string) => line.includes("Jump to end")),
			lines: terminal.getViewport().map((line: string) => line.trimEnd()),
		});
		const phases: any[] = [{ action: "initial", ...observe() }];
		tui.scrollBy(1);
		await terminal.waitForRender();
		phases.push({ action: "scrolled-away-from-end", ...observe() });
		assert.strictEqual(phases[0].followingEnd, false);
		assert.ok(!phases[0].indicatorShown);
		assert.strictEqual(phases[1].scrollTop, 1);
		assert.ok(!phases[1].indicatorShown);
		tui.stop();
		scenarios.push({
			name: "alt-screen-indicator-no-follow-end",
			dimensions: { columns: 30, rows: 3 },
			inputs: { manualDelta: 1, indicatorLabel: " ↓ Jump to end " },
			expected: { phases },
		});
	}

	// A terminal that records its writes, so the clipboard (OSC 52) expansion
	// the auto-scroll selection produces is part of the recorded observation
	// (mirrors the frozen test's RecordingTerminal, :1727-1742).
	class RecordingTerminal extends VirtualTerminal {
		readonly events: Array<{ type: "write"; data: string }> = [];
		override write(data: string): void {
			this.events.push({ type: "write", data });
			super.write(data);
		}
	}

	/// The auto-scroll observation harness: one `TuiAltScreen` over a
	/// RecordingTerminal, the `autoScrollSelection` interval wrapped so every
	/// 50ms tick appends its exact observable state (wall-clock independent).
	const autoScrollObservation = async (
		lines: number,
		drag: (
			controls: { record: () => void; tickCount: () => number },
			terminal: RecordingTerminal,
			tui: any,
			text: any,
		) => Promise<void>,
	) => {
		const terminal = new RecordingTerminal(20, 4);
		const tui = new TuiAltScreen(terminal);
		const text = new Text(Array.from({ length: lines }, (_, index) => `line ${index + 1}`).join("\n"), 0, 0);
		tui.addChild(text);
		tui.start();
		await terminal.waitForRender();
		const trace: any[] = [];
		let tickCount = 0;
		const orig = tui.autoScrollSelection.bind(tui);
		// Method assignment on the instance: the class method reads
		// `this.autoScrollSelection`, so the wrapper sees every interval tick.
		tui.autoScrollSelection = () => {
			orig();
			tickCount += 1;
			trace.push({
				tick: tickCount,
				viewportTop: tui.viewportTop,
				focusRow: tui.selectionFocus?.row ?? null,
				focusCol: tui.selectionFocus?.col ?? null,
				selectionText: tui.getActiveSelectionText() ?? null,
			});
		};
		await drag({
			record: () => {
			// Drain the frame the tick requested before recording the pointer row:
			// render reads the live focus (which moves with the scroll), while the
			// recorded pointer row is the fixed report the drag stays at.
			void terminal.flush();
			trace.push({
				tick: tickCount,
				viewportTop: tui.viewportTop,
				pointerRow: Math.max(0, Math.min(3, tui.selectionDragPointer?.y ?? 0)),
				selectionText: tui.getActiveSelectionText() ?? null,
			});
			},
			tickCount: () => tickCount,
		}, terminal, tui, text);
		const clipboardWrites = terminal.events
			.filter((event) => event.data.includes("\x1b]52;c;"))
			.map((event) => event.data);
		tui.stop();
		return { initialViewportTop: lines - 4, trace, tickCount, clipboardWrites };
	};

	// A held drag above the viewport's visible top scrolls one line per 50ms
	// tick, extends the focus from the clipped pointer, and the release copies
	// the selected rows (frozen `test/tui-alt-screen.test.ts:1734-1761`).
	{
		const result = await autoScrollObservation(10, async ({ record }, terminal) => {
			terminal.sendInput("\x1b[<0;1;3M");
			terminal.sendInput("\x1b[<32;1;1M");
			await new Promise((resolve) => setTimeout(resolve, 130));
			await terminal.waitForRender();
			record();
			terminal.sendInput("\x1b[<0;1;1m");
			await terminal.waitForRender();
		});
		assert.ok(result.tickCount >= 1, `expected at least one auto-scroll tick, got ${result.tickCount}`);
		assert.ok(
			result.trace[0].viewportTop < result.initialViewportTop,
			`expected auto-scroll above row ${result.initialViewportTop}, got ${result.trace[0].viewportTop} (trace ${JSON.stringify(result.trace)})`,
		);
		// The copied text is base64 inside the OSC 52 sequence: assert on the
		// decoded payload (the frozen test decodes it the same way, :1750-1760).
		const decoded = result.clipboardWrites.map((data) =>
			Buffer.from(/\x1b\]52;c;([^\x07]*)\x07/.exec(data)?.[1] ?? "", "base64").toString("utf8"),
		);
		assert.ok(
			decoded.some((text) => text.includes("line 7")),
			`expected the OSC 52 write to contain the selected rows: ${JSON.stringify(decoded)}`,
		);
		scenarios.push({
			name: "alt-screen-auto-scroll-edge",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				documentLines: 10,
				press: { x: 1, y: 3 },
				drag: { x: 1, y: 1 },
				holdMs: 130,
				release: { x: 1, y: 1 },
			},
			expected: {
				initialViewportTop: result.initialViewportTop,
				trace: result.trace,
				tickCount: result.tickCount,
				clipboardWrites: result.clipboardWrites,
			},
		});
	}

	// Release cancels the interval immediately (no further tick after the
	// release even though the wrapper keeps counting), and a second drag back
	// to a direction-0 cell stops the interval on that motion itself
	// (frozen `clearInterval` semantics, :1308-1313).
	{
		const result = await autoScrollObservation(10, async ({ record, tickCount }, terminal) => {
			terminal.sendInput("\x1b[<0;1;3M");
			terminal.sendInput("\x1b[<32;1;1M");
			await new Promise((resolve) => setTimeout(resolve, 110));
			await terminal.waitForRender();
			record();
			const ticksBeforeRelease = tickCount();
			terminal.sendInput("\x1b[<0;1;1m");
			await terminal.waitForRender();
			await new Promise((resolve) => setTimeout(resolve, 100));
			const ticksAfterRelease = tickCount();
			record();
			assert.strictEqual(ticksAfterRelease, ticksBeforeRelease, "release must cancel the interval");
			terminal.sendInput("\x1b[<0;1;3M");
			terminal.sendInput("\x1b[<32;1;2M");
			await terminal.waitForRender();
			const ticksBeforeCenter = tickCount();
			await new Promise((resolve) => setTimeout(resolve, 80));
			const ticksAfterCenter = tickCount();
			record();
			assert.strictEqual(ticksAfterCenter, ticksBeforeCenter, "direction-0 motion must stop the interval");
			terminal.sendInput("\x1b[<0;1;1m");
			await terminal.waitForRender();
		});
		scenarios.push({
			name: "alt-screen-auto-scroll-cancel",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				documentLines: 10,
				firstDrag: { x: 1, y: 1 },
				release: { x: 1, y: 1 },
				secondDrag: { x: 1, y: 2 },
			},
			expected: {
				initialViewportTop: result.initialViewportTop,
				trace: result.trace,
				tickCount: result.tickCount,
				clipboardWrites: result.clipboardWrites,
			},
		});
	}

	// A document one scroll step from the end stops on the tick that hits the
	// limit: the limit tick is the last (the interval counter proves it).
	{
		const result = await autoScrollObservation(5, async ({ record }, terminal) => {
			// Row 4 is one line from the content end (rows 1-5 fill the viewport):
		// the first tick reaches the limit and stops the interval.
			terminal.sendInput("\x1b[<0;1;4M");
			terminal.sendInput("\x1b[<32;1;1M");
			await new Promise((resolve) => setTimeout(resolve, 200));
			await terminal.waitForRender();
			record();
			terminal.sendInput("\x1b[<0;1;1m");
			await terminal.waitForRender();
		});
		// The limit tick is the last scroll: a second fired tick stops without
		// moving (frozen `remaining === direction`, :1295), which is the proof
		// the interval no longer scrolls.
		assert.strictEqual(result.tickCount, 2, `expected the limit to fire one moving tick then stop, got ${result.tickCount} (trace ${JSON.stringify(result.trace)})`);
		assert.strictEqual(result.trace[0].viewportTop, 0, "the limit tick must reach the top");
		assert.strictEqual(result.trace[1].viewportTop, 0, "the post-limit tick must not scroll");
		scenarios.push({
			name: "alt-screen-auto-scroll-limit",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				documentLines: 5,
				press: { x: 1, y: 4 },
				drag: { x: 1, y: 1 },
				holdMs: 200,
				release: { x: 1, y: 1 },
			},
			expected: {
				initialViewportTop: result.initialViewportTop,
				trace: result.trace,
				tickCount: result.tickCount,
				clipboardWrites: result.clipboardWrites,
			},
		});
	}

	const sourceEndpoints = [
		"packages/tui/src/components/scroll-view.ts:ScrollView.scrollTo,ScrollView.scrollBy,ScrollView.updateLayout,ScrollView.scrollToStart,ScrollView.scrollToEnd",
		"packages/tui/src/layout.ts:renderLayoutFrame,getScrollViewsAt,cropKittyImageLine",
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.constructor,TuiAltScreen.handleViewportInput,TuiAltScreen.routeWheel,TuiAltScreen.compositeScrollToEndIndicator,TuiAltScreen.handleMouseEvent,TuiAltScreen.updateSelectionAutoScroll,TuiAltScreen.autoScrollSelection,TuiAltScreen.stopSelectionAutoScroll",
		"packages/tui/src/wheel-scroll.ts:WheelScrollAccelerator.constructor,WheelScrollAccelerator.setLines,WheelScrollAccelerator.next",
		"packages/tui/src/keybindings.ts:TUI_KEYBINDINGS,KeybindingsManager,setKeybindings,getKeybindings",
		"packages/tui/test/tui-alt-screen.test.ts:routes Home and End to the focused component and Ctrl+Home/End to the transcript,shows a clickable jump-to-end indicator on the transcript's last row while scrolled up",
		"packages/tui/test/wheel-scroll.test.ts:uses fixed line counts regardless of timing,scales auto mode with wheel velocity,does not accelerate bursts of events for a single notch,resets acceleration on direction changes and pauses,carries fractional lines between events",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.getViewport,VirtualTerminal.resize",
	];

	return { sourceEndpoints, scenarios };
}
