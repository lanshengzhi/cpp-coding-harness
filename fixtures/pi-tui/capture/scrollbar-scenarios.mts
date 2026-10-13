import { pathToFileURL } from "node:url";
import path from "node:path";

/// Independent pi-v1.0.4 observation of scrollbar presentation (#1001).
///
/// Every value below comes from pi's own modules: `ScrollView` for the scrollbar
/// modes (`isScrollbarVisible`/`isScrollbarActive`/`setScrollbar`/
/// `getContentWidth` and the transient hide timer), `renderLayoutFrame` for the
/// painted frame (pi's `visibleLines` convention, `test/layout.test.ts:10-12`:
/// strip terminal sequences and trailing padding), a real `TuiAltScreen` over
/// pi's `VirtualTerminal` for the hover the frozen `handleMouseEvent`,
/// `routeWheel`, `FOCUS_OUT` and `stop()` drive, and pi's `stripTerminalSequences`
/// for the style probes. The C++ replay reads the artifact through
/// `scripts/tui/evidence.py`; nothing here is derived from the C++ side.
export async function captureScrollbar(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { renderLayoutFrame } = await import(source("src/layout.ts"));
	const { stripTerminalSequences } = await import(source("src/utils.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	const visibleLines = (lines: readonly string[]) => lines.map((line) => stripTerminalSequences(line).trimEnd());
	const numberedText = (count: number) =>
		Array.from({ length: count }, (_, index) => `line ${index + 1}`).join("\n");

	/// The painted frame of a scroll-viewport root: what the terminal shows, the
	/// viewport's allocated window, the width its child was laid out at, and the
	/// height the child reported.
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
		const child = scrollBox?.children?.[0];
		return {
			lines: visibleLines(painted.lines),
			window: scrollBox
				? {
						width: scrollBox.rect.width,
						height: scrollBox.rect.height,
						childWidth: child ? child.rect.width : 0,
						childHeight: child ? child.rect.height : 0,
					}
				: null,
			painted,
		};
	};

	/// One observed phase of a viewport: the painted rows plus the viewport's own
	/// public state.
	const phaseOf = (view: any, columns: number, rows: number) => {
		const frame = frameOf(view, columns, rows);
		return {
			lines: frame.lines,
			window: frame.window,
			scrollbar: view.scrollbar,
			isScrollbarVisible: view.isScrollbarVisible,
			isScrollbarActive: view.isScrollbarActive,
			scrollTop: view.scrollTop,
			viewportHeight: view.viewportHeight,
		};
	};

	const scenarios: any[] = [];

	// The three modes, the reserved content column and a runtime mode change
	// (pi `scroll-view.ts:52-55,74-93`, `layout.ts:138`, and
	// `test/layout.test.ts:295-307` for the re-wrap).
	{
		const view = new ScrollView(new Text("123456", 0, 0));
		const hidden = {
			contentWidth: view.getContentWidth(6),
			contentWidthAtOne: view.getContentWidth(1),
			...phaseOf(view, 6, 2),
		};
		view.setScrollbar("always");
		const always = {
			contentWidth: view.getContentWidth(6),
			contentWidthAtOne: view.getContentWidth(1),
			...phaseOf(view, 6, 2),
		};
		view.setScrollbar("hidden");
		const restored = { contentWidth: view.getContentWidth(6), ...phaseOf(view, 6, 2) };
		scenarios.push({
			name: "scrollbar-modes-and-reservation",
			dimensions: { columns: 6, rows: 2 },
			inputs: { content: "123456", modes: ["hidden", "always", "hidden"] },
			expected: { hidden, always, restored },
		});
	}

	// The transient timeline of an `auto` bar: hidden on the first frame,
	// revealed by scroll activity with a proportional thumb, expanded while the
	// pointer is active, hidden again by the timer, and back at the ends (pi
	// `scroll-view.ts:97-123,150-190`, `layout.ts:280-331`, and pi's
	// `test/layout.test.ts:193-244`).
	{
		const sourceLines = ["abcd界", "abcde2", "abcde3", "abcde4", "abcde5", "abcde6", "abcde7", "abcde8"];
		const contentBackground = "\x1b[42m";
		const trackColor = "\x1b[38;5;2m";
		const thumbColor = "\x1b[38;5;1m";
		const scrollbarTrackStyle = (text: string) => `${trackColor}${text}\x1b[39m`;
		const scrollbarThumbStyle = (text: string) => `${thumbColor}${text}\x1b[39m`;
		const content = new Text(sourceLines.join("\n"), 0, 0, (text) => `${contentBackground}${text}\x1b[49m`);
		const view = new ScrollView(content, {
			scrollbar: "auto",
			scrollbarTrackStyle,
			scrollbarThumbStyle,
			scrollbarHideDelayMs: 10,
		});
		/// Which rows carry the track colour, the thumb colour, and the reset
		/// prefix pi re-emits with the background it preserved under an overlay
		/// glyph (pi `test/layout.test.ts:266-269`).
		const styles = (lines: readonly string[]) => ({
			track: lines.map((line) => line.includes(trackColor)),
			thumb: lines.map((line) => line.includes(thumbColor)),
			preservedBackgroundReemit: lines.map((line) => line.includes(`\x1b[0m\x1b]8;;\x07${contentBackground}`)),
		});
		const first = phaseOf(view, 6, 4);
		const requests = [2];
		view.scrollBy(requests[0]!);
		const scrolled = { ...phaseOf(view, 6, 4), ...styles(frameOf(view, 6, 4).painted.lines) };
		view.setScrollbarActive(true);
		const hovered = { ...phaseOf(view, 6, 4), ...styles(frameOf(view, 6, 4).painted.lines) };
		view.setScrollbarActive(false);
		await new Promise((resolve) => setTimeout(resolve, 30));
		const expired = phaseOf(view, 6, 4);
		view.scrollToEnd();
		const atEnd = phaseOf(view, 6, 4);
		view.scrollToStart();
		const atStart = phaseOf(view, 6, 4);

		// Followed content that fits never shows a bar, while `always` keeps its
		// reserved column and a full-height thumb.
		const followedContent = new Text(sourceLines.join("\n"), 0, 0);
		const followed = new ScrollView(followedContent, {
			follow: "end",
			scrollbar: "auto",
			scrollbarTrackStyle,
			scrollbarThumbStyle,
		});
		const followedBefore = phaseOf(followed, 6, 4);
		followedContent.setText(`${sourceLines.join("\n")}\nabcde9`);
		const followedAfter = phaseOf(followed, 6, 4);
		const fitting = new ScrollView(new Text("1\n2", 0, 0), { scrollbar: "auto", scrollbarThumbStyle });
		const fittingBefore = phaseOf(fitting, 6, 4);
		fitting.scrollBy(1);
		const fittingAfter = phaseOf(fitting, 6, 4);
		const alwaysFitting = new ScrollView(new Text("1\n2", 0, 0), { scrollbar: "always", scrollbarThumbStyle });
		const alwaysFittingPhase = phaseOf(alwaysFitting, 6, 4);
		// An `always` bar has reserved its column, so it must not re-emit the
		// content background after its own reset prefix (pi
		// `test/layout.test.ts:263-269`).
		const alwaysOverflowing = new ScrollView(content, {
			scrollbar: "always",
			scrollbarTrackStyle,
			scrollbarThumbStyle,
		});
		const alwaysOverflowingPhase = phaseOf(alwaysOverflowing, 6, 4);
		const alwaysOverflowingRaw = frameOf(alwaysOverflowing, 6, 4).painted.lines;
		const alwaysOverflowingReset = alwaysOverflowingRaw.map((line) => {
			const scrollbarStyleIndex = Math.max(line.lastIndexOf(trackColor), line.lastIndexOf(thumbColor));
			return line.lastIndexOf("\x1b[0m\x1b]8;;\x07", scrollbarStyleIndex) > line.lastIndexOf(contentBackground);
		});

		scenarios.push({
			name: "scrollbar-transient-timeline",
			dimensions: { columns: 6, rows: 4 },
			inputs: {
				contentLines: sourceLines,
				styles: { trackColor, thumbColor, contentBackground },
				options: { scrollbar: "auto", scrollbarHideDelayMs: 10 },
				requests,
				activeToggle: [true, false],
				hideWaitMs: 30,
				followedLines: 8,
				appendedLines: 9,
			},
			expected: {
				first,
				scrolled,
				hovered,
				expired,
				atEnd,
				atStart,
				followedBefore,
				followedAfter,
				fittingBefore,
				fittingAfter,
				alwaysFitting: alwaysFittingPhase,
				alwaysOverflowing: alwaysOverflowingPhase,
				alwaysOverflowingReset,
			},
		});
	}

	// Thumb proportions for a twenty-row track, the empty-content branch, a
	// one-row track and a single-column viewport (pi `layout.ts:280-307`).
	{
		const thumbHeightFor = (contentHeight: number) => {
			const sized = new ScrollView(
				new Text(Array.from({ length: contentHeight }, () => "x").join("\n"), 0, 0),
				{ scrollbar: "auto", scrollbarThumbStyle: (text) => text },
			);
			frameOf(sized, 6, 20);
			sized.scrollBy(1);
			const lines = frameOf(sized, 6, 20).lines;
			return {
				thumbRows: lines.filter((line) => line.endsWith("┃")).length,
				trackRows: lines.filter((line) => line.endsWith("│")).length,
			};
		};
		const empty = new ScrollView({ render: () => [], invalidate: () => {} }, { scrollbar: "always" });
		const emptyFrame = frameOf(empty, 4, 3);
		const tiny = new ScrollView(new Text(Array.from({ length: 8 }, () => "abcde").join("\n"), 0, 0), {
			scrollbar: "always",
		});
		const tinyFrame = frameOf(tiny, 3, 1);
		const singleColumn = new ScrollView(new Text("abcde", 0, 0), { scrollbar: "always" });
		const singleColumnFrame = frameOf(singleColumn, 1, 2);
		scenarios.push({
			name: "scrollbar-geometry-edges",
			dimensions: { columns: 6, rows: 20 },
			inputs: { thumbContentHeights: [21, 40, 100, 400], emptyRows: 3, tinyContentLines: 8, singleColumnWidth: 1 },
			expected: {
				thumbs: [21, 40, 100, 400].map((contentHeight) => ({
					contentHeight,
					...thumbHeightFor(contentHeight),
				})),
				empty: { lines: emptyFrame.lines, window: emptyFrame.window },
				tiny: { lines: tinyFrame.lines, window: tinyFrame.window },
				singleColumn: {
					lines: singleColumnFrame.lines,
					window: singleColumnFrame.window,
					contentWidth: singleColumn.getContentWidth(1),
				},
			},
		});
	}

	// The overlay splice keeps only the background underneath the glyph (pi
	// `layout.ts:249-279` and `test/layout.test.ts:248-274`).
	{
		const background = "\x1b[42m";
		const borderForeground = "\x1b[31m";
		const content = {
			render: (width: number) =>
				Array.from(
					{ length: 8 },
					() => `${background}${"x".repeat(width - 1)}${borderForeground}│\x1b[39m\x1b[49m`,
				),
			invalidate: () => {},
		};
		const view = new ScrollView(content, {
			scrollbar: "auto",
			scrollbarTrackStyle: (text) => text,
			scrollbarThumbStyle: (text) => text,
		});
		frameOf(view, 6, 4);
		view.scrollBy(1);
		const painted = frameOf(view, 6, 4).painted.lines;
		scenarios.push({
			name: "scrollbar-overlay-preserves-background",
			dimensions: { columns: 6, rows: 4 },
			inputs: { contentBackground: background, borderForeground, contentLines: 8, request: 1 },
			expected: {
				lines: visibleLines(painted),
				keepsBackground: painted.map((line) => line.includes(background)),
				dropsBorderForeground: painted.map((line) => !line.includes(borderForeground)),
				preservedBackgroundReemit: painted.map(
					(line) => line.includes(`\x1b[0m\x1b]8;;\x07${background}`),
				),
			},
		});
	}

	// The hover a real alternate screen derives from pointer reports: a wheel
	// over the track reveals the transient bar and scrolls, a motion report on
	// the track keeps it, leaving the track lets the zero-delay timer hide it,
	// focus loss and `stop()` drop the hover (pi `tui-alt-screen.ts:894-947,
	// 996,1037-1066,678` and `test/tui-alt-screen.test.ts:140-176`).
	{
		const terminal = new VirtualTerminal(20, 4);
		const tui = new TuiAltScreen(terminal);
		const transcript = new ScrollView(new Text(numberedText(10), 0, 0), {
			follow: "end",
			primary: true,
			scrollbar: "auto",
			scrollbarHideDelayMs: 0,
		});
		tui.setLayoutRoot(transcript);
		tui.start();
		const observe = () => ({
			viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
			isScrollbarVisible: transcript.isScrollbarVisible,
			isScrollbarActive: transcript.isScrollbarActive,
			scrollTop: transcript.scrollTop,
			viewportHeight: transcript.viewportHeight,
		});
		await terminal.waitForRender();
		const phases: any[] = [{ action: "initial", ...observe() }];
		const reports = [
			{ action: "wheel-on-track", data: "\x1b[<64;20;1M" },
			{ action: "motion-on-track", data: "\x1b[<35;20;1M" },
			{ action: "motion-off-track", data: "\x1b[<35;19;1M" },
			{ action: "focus-out", data: "\x1b[O" },
			{ action: "motion-on-track-again", data: "\x1b[<35;20;1M" },
		];
		for (const report of reports) {
			terminal.sendInput(report.data);
			await terminal.waitForRender();
			phases.push({ action: report.action, ...observe() });
		}
		tui.stop();
		phases.push({ action: "stopped", ...observe() });
		scenarios.push({
			name: "scrollbar-alt-screen-hover",
			dimensions: { columns: 20, rows: 4 },
			inputs: {
				documentLines: 10,
				options: { follow: "end", primary: true, scrollbar: "auto", scrollbarHideDelayMs: 0 },
				reports,
			},
			expected: { phases },
		});
	}

	const sourceEndpoints = [
		"packages/tui/src/components/scroll-view.ts:ScrollView.getContentWidth,ScrollView.setScrollbar,ScrollView.setScrollbarActive,ScrollView.isScrollbarVisible,ScrollView.isScrollbarActive,ScrollView.scrollBy,ScrollView.scrollToEnd,ScrollView.scrollToStart",
		"packages/tui/src/layout.ts:renderLayoutFrame",
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.getScrollbarTargetAt,TuiAltScreen.updateScrollbarHover,TuiAltScreen.stopScrollbarHover,TuiAltScreen.routeWheel",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.getViewport,VirtualTerminal.sendInput,VirtualTerminal.waitForRender",
	];

	return { sourceEndpoints, scenarios };
}
