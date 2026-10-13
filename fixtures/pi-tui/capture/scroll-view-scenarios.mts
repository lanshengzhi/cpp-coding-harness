import { pathToFileURL } from "node:url";
import path from "node:path";

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
	const { VStack } = await import(source("src/components/v-stack.ts"));
	const { getScrollViewsAt, renderLayoutFrame } = await import(source("src/layout.ts"));
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

	const sourceEndpoints = [
		"packages/tui/src/components/scroll-view.ts:ScrollView.scrollTo,ScrollView.scrollBy,ScrollView.updateLayout,ScrollView.scrollToStart,ScrollView.scrollToEnd",
		"packages/tui/src/layout.ts:renderLayoutFrame,getScrollViewsAt,cropKittyImageLine",
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.getPrimaryScrollView,TuiAltScreen.viewportTop,TuiAltScreen.scrollBy,TuiAltScreen.scrollToTop,TuiAltScreen.scrollToBottom,TuiAltScreen.doRender",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.getViewport,VirtualTerminal.resize",
	];

	return { sourceEndpoints, scenarios };
}
