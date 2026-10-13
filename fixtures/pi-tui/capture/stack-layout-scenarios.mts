import { pathToFileURL } from "node:url";
import path from "node:path";

/// Independent pi-v1.0.4 observation of viewport stack layout (#998).
///
/// Every value below comes from pi's own modules: `VStack` for the unbounded
/// stack composition, `renderLayoutFrame` for a constrained frame, pi's own
/// `visibleLines` convention (`test/layout.test.ts:10-12`: strip terminal
/// sequences and trailing padding) for what the terminal shows, and the
/// bottom-up cursor extraction of `TuiBase.extractCursorPosition`
/// (`src/tui.ts:1442-1460`). The C++ replay reads the artifact through
/// `scripts/tui/evidence.py`; nothing here is derived from the C++ side.
export async function captureStackLayout(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { VStack } = await import(source("src/components/v-stack.ts"));
	const { HStack } = await import(source("src/components/h-stack.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { renderLayoutFrame } = await import(source("src/layout.ts"));
	const { stripTerminalSequences, visibleWidth } = await import(source("src/utils.ts"));
	const { CURSOR_MARKER } = await import(source("src/tui.ts"));

	const visibleLines = (lines: readonly string[]) => lines.map((line) => stripTerminalSequences(line).trimEnd());

	/// A leaf that renders exactly the given lines and marks one of them with
	/// pi's cursor marker, so the frame's leaf crop and cursor translation are
	/// observable without a real editor.
	const markerLeaf = (lines: string[], cursorLine = -1) => ({
		render: (_width: number) =>
			lines.map((line, index) => (index === cursorLine ? `${line}${CURSOR_MARKER}` : line)),
		invalidate: () => {},
	});

	interface Rect {
		x: number;
		y: number;
		width: number;
		height: number;
		lineOffset: number;
	}
	const rectOf = (box: any): Rect => ({
		x: box.rect.x,
		y: box.rect.y,
		width: box.rect.width,
		height: box.rect.height,
		lineOffset: box.lineOffset ?? 0,
	});

	/// `TuiBase.extractCursorPosition` over the painted frame, plus the marker
	/// stripped from the visible lines it was extracted from.
	const cursorOf = (lines: string[]) => {
		for (let row = lines.length - 1; row >= 0; row--) {
			const markerIndex = lines[row]!.indexOf(CURSOR_MARKER);
			if (markerIndex === -1) continue;
			const column = visibleWidth(lines[row]!.slice(0, markerIndex));
			lines[row] = lines[row]!.slice(0, markerIndex) + lines[row]!.slice(markerIndex + CURSOR_MARKER.length);
			return { row, column };
		}
		return null;
	};

	const frame = (root: any, columns: number, rows: number) => {
		const painted = renderLayoutFrame(root, columns, rows, () => {});
		const lines = painted.lines.slice();
		const cursor = cursorOf(lines);
		return {
			children: painted.root.children.map(rectOf),
			lines: visibleLines(lines),
			cursor,
		};
	};

	const scenarios: any[] = [];

	// Unbounded stack (`v-stack.ts:10-30`): no available size, so basis/min/max
	// decide the allocation, gaps are inserted between visible entries only, and
	// short children are padded to their allocation.
	{
		const gapStack = new VStack(
			[
				new Text("one", 0, 0),
				{ component: new Text("hidden", 0, 0), visible: () => false },
				new Text("two", 0, 0),
			],
			{ gap: 1 },
		);
		const clampStack = new VStack([
			{ component: new Text("a", 0, 0), basis: 1, minSize: 2 },
			{ component: new Text("b1\nb2\nb3", 0, 0), maxSize: 2 },
			{ component: new Text("c", 0, 0), basis: 3 },
		]);
		scenarios.push({
			name: "standalone-unbounded",
			dimensions: { columns: 10, rows: 8 },
			inputs: {
				gap: 1,
				invisibleEntry: true,
				clamps: { basisBelowMinimum: ["a", 1, 2], maxSize: ["b1", "b2", "b3", 2], basisAboveContent: ["c", 3] },
			},
			expected: {
				gapStack: gapStack.render(10).map((line) => line.trimEnd()),
				clampStack: clampStack.render(10).map((line) => line.trimEnd()),
			},
		});
	}

	// Constrained frame (`layout.ts:170-196`): the allocation is the available
	// height, growth fills the flexible entry, and the frame is exactly the
	// viewport (pi `test/layout.test.ts:13-27`).
	{
		const root = new VStack([
			{ component: new Text("top", 0, 0), basis: 1, shrink: 0 },
			{ component: new Text("body", 0, 0), basis: 0, grow: 1 },
		]);
		scenarios.push({
			name: "frame-vertical-grow",
			dimensions: { columns: 10, rows: 4 },
			inputs: { entries: [{ text: "top", basis: 1, grow: 0, shrink: 0 }, { text: "body", basis: 0, grow: 1 }] },
			expected: frame(root, 10, 4),
		});
	}

	// Shrink (`stack.ts:135-153`): the difference is taken back by
	// `shrink * max(1, size)` weight, saturating at `minSize`, so a 3-row entry
	// with `shrink: 0` keeps its rows (pi `test/layout.test.ts:88-102`).
	{
		const root = new VStack([
			{ component: new Text("a1\na2\na3", 0, 0), shrink: 1, minSize: 1 },
			{ component: new Text("b1\nb2\nb3", 0, 0), shrink: 0 },
		]);
		scenarios.push({
			name: "frame-vertical-shrink",
			dimensions: { columns: 10, rows: 4 },
			inputs: {
				entries: [
					{ text: "a1\na2\na3", shrink: 1, minSize: 1 },
					{ text: "b1\nb2\nb3", shrink: 0 },
				],
			},
			expected: frame(root, 10, 4),
		});
	}

	// Nested stack with an auto basis (`layout.ts:174-176` measures the dock's
	// intrinsic height, including its children's `minSize`) and a flexible
	// transcript with `basis: 0` that is never measured (pi
	// `test/layout.test.ts:104-131`).
	{
		const dock = new VStack([
			new Text("top1\ntop2\ntop3", 0, 0),
			{ component: new Text("selector", 0, 0), minSize: 3 },
			new Text("below", 0, 0),
			{ component: new Text("footer", 0, 0), minSize: 1 },
		]);
		const root = new VStack([
			{ component: new Text("body", 0, 0), basis: 0, grow: 1, minSize: 1 },
			{ component: dock, basis: "auto", minSize: 1 },
		]);
		scenarios.push({
			name: "frame-nested-minimums",
			dimensions: { columns: 10, rows: 9 },
			inputs: {
				entries: [
					{ text: "body", basis: 0, grow: 1, minSize: 1 },
					{
						dock: [
							{ text: "top1\ntop2\ntop3" },
							{ text: "selector", minSize: 3 },
							{ text: "below" },
							{ text: "footer", minSize: 1 },
						],
						basis: "auto",
						minSize: 1,
					},
				],
			},
			expected: frame(root, 10, 9),
		});
	}

	// The cursor rule of `layout.ts:117-123`: a leaf whose lines overflow its
	// allocation is cropped upward to keep the marker row, and the marker is
	// reported at its translated position.
	{
		const root = new VStack([
			{ component: markerLeaf(["a1", "a2", "a3", "a4"], 3), shrink: 1, minSize: 1 },
			{ component: new Text("b1\nb2", 0, 0), shrink: 0 },
		]);
		scenarios.push({
			name: "frame-cursor-crop",
			dimensions: { columns: 10, rows: 3 },
			inputs: {
				entries: [
					{ lines: ["a1", "a2", "a3", "a4"], cursorLine: 3, shrink: 1, minSize: 1 },
					{ text: "b1\nb2", shrink: 0 },
				],
			},
			expected: frame(root, 10, 3),
		});
	}

	// A responsive entry (`stack.ts:14` `visible`) is evaluated against the
	// frame viewport, and a carried cross-axis `align` does not change the
	// vertical allocation.
	{
		const root = (align: string) =>
			new VStack(
				[
					{ component: new Text("head", 0, 0), basis: 1, shrink: 0 },
					{
						component: new Text("detail", 0, 0),
						basis: 0,
						grow: 1,
						minSize: 1,
						visible: (viewport: { height: number }) => viewport.height >= 5,
					},
					{ component: new Text("foot", 0, 0), basis: 1, shrink: 0 },
				],
				{ align: align as "stretch" },
			);
		scenarios.push({
			name: "frame-responsive-visibility",
			dimensions: { columns: 10, rows: 2 },
			inputs: { visibleAboveHeight: 5, aligns: ["stretch", "center"] },
			expected: {
				shortStretch: frame(root("stretch"), 10, 2),
				tallStretch: frame(root("stretch"), 10, 5),
				tallCenter: frame(root("center"), 10, 5),
			},
		});
	}

	// The composed product shape of `chat-viewport.ts:31-45`: a transcript with
	// `basis: 0` above a dock whose editor keeps `minSize` while the completion
	// menu appears and disappears, so the editor and both footer rows stay at
	// the bottom of the viewport (the live property of #831).
	{
		const dockOf = (editorLines: string[], menuVisible: boolean) => {
			const dock = new VStack([
				{ component: new Text(editorLines.join("\n"), 0, 0), shrink: 1, minSize: 3 },
				{
					component: new Text("menu1\nmenu2\nmenu3", 0, 0),
					shrink: 1,
					minSize: 0,
					visible: () => menuVisible,
				},
				{ component: new Text("footer-1\nfooter-2", 0, 0), basis: 2, grow: 0, shrink: 0 },
			]);
			return dock;
		};
		const rootOf = (editorLines: string[], menuVisible: boolean) =>
			new VStack([
				{ component: new Text("t1\nt2\nt3\nt4\nt5\nt6\nt7\nt8", 0, 0), basis: 0, grow: 1, shrink: 1, minSize: 1 },
				{ component: dockOf(editorLines, menuVisible), basis: "auto", grow: 0, shrink: 1, minSize: 1 },
			]);
		const growth = (name: string, editorLines: string[], menuVisible: boolean) => ({
			name,
			editorLines: editorLines.length,
			menuVisible,
			...frame(rootOf(editorLines, menuVisible), 8, 12),
		});
		scenarios.push({
			name: "frame-dock-growth",
			dimensions: { columns: 8, rows: 12 },
			inputs: {
				transcript: { lines: 8, basis: 0, grow: 1, shrink: 1, minSize: 1 },
				dock: { editor: { shrink: 1, minSize: 3 }, menu: { shrink: 1, minSize: 0 }, footer: { basis: 2 } },
			},
			expected: {
				editorAtMinimum: growth("editor-at-minimum", ["e1", "e2", "e3"], false),
				editorGrown: growth("editor-grown", ["e1", "e2", "e3", "e4", "e5", "e6"], false),
				menuOpen: growth("menu-open", ["e1", "e2", "e3", "e4", "e5", "e6"], true),
				overConstrained: growth(
					"over-constrained",
					["e1", "e2", "e3", "e4", "e5", "e6", "e7", "e8", "e9"],
					true,
				),
				shrunkBack: growth("shrunk-back", ["e1", "e2", "e3"], false),
			},
		});
	}

	// Horizontal stack (`h-stack.ts:5-43`, the `layout.ts` hstack node
	// `layout.ts:198-247`): the requested width is the main axis, so widths are
	// allocated against it, every nonzero allocation renders the child again at
	// exactly that width (`widths[index] === 0 ? [] : render(widths[index])`), and
	// the shared rows are composed left to right with `compositeTuiLine`
	// (`tui.ts:415-444`). Every child below is a probing leaf that clips its lines
	// to the width it is rendered at — the C++ shape of `Text(text, 0, 0)` — and
	// the widths it is actually rendered at are part of the observation, so an
	// entry that is measured but never rendered, and a numeric basis that is never
	// measured, are both visible.
	{
		const probe = (lines: string[]) => {
			const widths: number[] = [];
			const component = {
				render: (width: number) => {
					widths.push(width);
					// A leaf that respects its width, like pi's `Text`: the C++ conversion
					// rejects a component whose lines exceed the width it was rendered at,
					// so an over-wide line would leave the observation unreplayable.
					return lines.map((line) => line.slice(0, width));
				},
				invalidate: () => {},
			};
			return { component, widths };
		};

		const alignRuns: Record<string, unknown> = {};
		for (const align of ["stretch", "start", "center", "end"] as const) {
			const children = [probe(["left"]), probe(["x", "y", "z"]), probe(["hidden"]), probe(["rt"])];
			const stack = new HStack(
				[
					{ component: children[0]!.component, basis: 4, shrink: 0 },
					{ component: children[1]!.component, basis: 2, grow: 1, shrink: 0 },
					{ component: children[2]!.component, basis: 0, shrink: 0 },
					{ component: children[3]!.component, basis: 5, shrink: 1, minSize: 1 },
				],
				{ align, gap: 1 },
			);
			alignRuns[align] = {
				lines: visibleLines(stack.render(12)),
				renderedAt: children.map((child) => child.widths),
			};
		}

		// Grow (`stack.ts:114-134`) fills the flexible entry up to `maxSize`, and a
		// zero-width entry consumes its `gap` column without being rendered.
		const growChildren = [probe(["a"]), probe(["bb"]), probe([""])];
		const grow = new HStack(
			[
				{ component: growChildren[0]!.component, basis: 0, grow: 1, shrink: 0 },
				{ component: growChildren[1]!.component, basis: 2, shrink: 0 },
				{ component: growChildren[2]!.component, basis: 0, shrink: 0 },
			],
			{ gap: 1 },
		);

		// `clampSize` (`stack.ts:93-98`) keeps the initial allocation inside
		// `[minSize, maxSize]` before growth is distributed.
		const clampChildren = [probe(["a"]), probe(["bb"])];
		const clamps = new HStack(
			[
				{ component: clampChildren[0]!.component, basis: 0, grow: 1, minSize: 3 },
				{ component: clampChildren[1]!.component, maxSize: 2, shrink: 0 },
			],
			{},
		);

		// A single zero-width entry allocates nothing, so the stack has no rows.
		const zeroOnlyChild = probe(["z"]);
		const zeroOnly = new HStack([{ component: zeroOnlyChild.component, basis: 0, shrink: 0 }], {});

		scenarios.push({
			name: "hstack-unbounded",
			dimensions: { columns: 12, rows: 6 },
			inputs: {
				width: 12,
				aligns: ["stretch", "start", "center", "end"],
				entries: [
					{ lines: ["left"], basis: 4, shrink: 0 },
					{ lines: ["x", "y", "z"], basis: 2, grow: 1, shrink: 0 },
					{ lines: ["hidden"], basis: 0, shrink: 0 },
					{ lines: ["rt"], basis: 5, shrink: 1, minSize: 1 },
				],
				grow: { lines: ["a", "bb", ""], entries: ["basis:0,grow:1", "basis:2,shrink:0", "basis:0,shrink:0"], width: 10 },
				clamps: { lines: ["a", "bb"], entries: ["basis:0,grow:1,minSize:3", "maxSize:2,shrink:0"], width: 8 },
				zeroOnly: { lines: ["z"], entries: ["basis:0,shrink:0"], width: 5 },
			},
			expected: {
				aligns: alignRuns,
				grow: { lines: visibleLines(grow.render(10)), renderedAt: growChildren.map((child) => child.widths) },
				clamps: { lines: visibleLines(clamps.render(8)), renderedAt: clampChildren.map((child) => child.widths) },
				zeroOnly: { lines: visibleLines(zeroOnly.render(5)), renderedAt: [zeroOnlyChild.widths] },
			},
		});
	}

	// Constrained horizontal allocation: unnoticed `basis` entries shrink by
	// `shrink * max(1, size)` weight (`stack.ts:135-153`) against the frame width
	// minus gaps, an absent basis is measured at the frame width
	// (`layout.ts:200-202`), and every child height is measured at its own
	// allocated width (`layout.ts:204-206`). The whole laid-out tree is observed
	// recursively, because an HStack child of a `VStack` root allocates its own
	// children.
	{
		const probe = (lines: string[]) => {
			const widths: number[] = [];
			const component = {
				render: (width: number) => {
					widths.push(width);
					// A leaf that respects its width, like pi's `Text`: the C++ conversion
					// rejects a component whose lines exceed the width it was rendered at,
					// so an over-wide line would leave the observation unreplayable.
					return lines.map((line) => line.slice(0, width));
				},
				invalidate: () => {},
			};
			return { component, widths };
		};
		const tree = (box: any): any => ({ ...rectOf(box), children: (box.children ?? []).map(tree) });
		const frameTree = (root: any, columns: number, rows: number) => {
			const painted = renderLayoutFrame(root, columns, rows, () => {});
			const lines = painted.lines.slice();
			const cursor = cursorOf(lines);
			return { tree: tree(painted.root), lines: visibleLines(lines), cursor };
		};

		const unequalOf = (width: number) => {
			const children = [probe(["lft"]), probe(["rgt"])];
			const root = new HStack(
				[
					{ component: children[0]!.component, basis: 6, shrink: 1, minSize: 1 },
					{ component: children[1]!.component, basis: 6, shrink: 1, minSize: 1 },
				],
				{ gap: 1 },
			);
			return {
				...frameTree(root, width, 1),
				renderedAt: children.map((child) => child.widths),
			};
		};

		scenarios.push({
			name: "hstack-frame-unequal",
			dimensions: { columns: 12, rows: 1 },
			inputs: {
				entries: [
					{ lines: ["lft"], basis: 6, shrink: 1, minSize: 1 },
					{ lines: ["rgt"], basis: 6, shrink: 1, minSize: 1 },
				],
				gap: 1,
				widths: [12, 8],
			},
			expected: { wide: unequalOf(12), narrow: unequalOf(8) },
		});

		const alignOf = (align: "stretch" | "start" | "center" | "end") => {
			const root = new HStack(
				[
					{ component: markerLeaf(["a1"]), basis: 4, shrink: 0 },
					{ component: markerLeaf(["b1", "b2"]), basis: 4, shrink: 0 },
				],
				{ align },
			);
			return frameTree(root, 8, 4);
		};

		scenarios.push({
			name: "hstack-frame-align",
			dimensions: { columns: 8, rows: 4 },
			inputs: {
				entries: [
					{ lines: ["a1"], basis: 4, shrink: 0 },
					{ lines: ["b1", "b2"], basis: 4, shrink: 0 },
				],
				aligns: ["stretch", "start", "center", "end"],
			},
			expected: {
				stretch: alignOf("stretch"),
				start: alignOf("start"),
				center: alignOf("center"),
				end: alignOf("end"),
			},
		});

		const autoBasisChildren = [probe(["ab"])];
		const widthChildWidths: number[] = [];
		const autoBasis = new HStack(
			[
				{
					component: {
						render: (width: number) => {
							widthChildWidths.push(width);
							return ["x".repeat(width)];
						},
						invalidate: () => {},
					},
					basis: "auto",
				},
				{ component: autoBasisChildren[0]!.component, basis: "auto", shrink: 1 },
			],
			{ gap: 1 },
		);
		scenarios.push({
			name: "hstack-frame-auto-basis",
			dimensions: { columns: 6, rows: 1 },
			inputs: {
				entries: [{ render: "'x' repeated width times", basis: "auto" }, { lines: ["ab"], basis: "auto", shrink: 1 }],
				gap: 1,
			},
			expected: {
				...frameTree(autoBasis, 6, 1),
				renderedAt: [widthChildWidths, autoBasisChildren[0]!.widths],
			},
		});
	}

	// A nested horizontal stack: the responsive predicate sees the frame
	// viewport (`stack.ts:14`), the `VStack` child is measured at its allocated
	// width, and the leaf crop keeps the cursor row (`layout.ts:117-123`). The
	// same component tree is re-framed after mutation, so the mutation is
	// observed rather than rebuilt.
	{
		let innerLines = ["m1", "m2"];
		let innerCursor = 1;
		const innerLeaf = {
			render: (_width: number) =>
				innerLines.map((line, index) => (index === innerCursor ? `${line}${CURSOR_MARKER}` : line)),
			invalidate: () => {},
			setLines: (lines: string[], cursor: number) => {
				innerLines = lines;
				innerCursor = cursor;
			},
		};
		const sideLeaf = markerLeaf(["s1"]);
		const inner = new VStack([{ component: innerLeaf, minSize: 2 }]);
		const root = new HStack(
			[
				{ component: inner, basis: 5, shrink: 0 },
				{
					component: sideLeaf,
					basis: 3,
					shrink: 1,
					minSize: 3,
					visible: (viewport: { width: number }) => viewport.width >= 10,
				},
			],
			{ gap: 1 },
		);
		const tree = (box: any): any => ({ ...rectOf(box), children: (box.children ?? []).map(tree) });
		const frameTree = (columns: number, rows: number) => {
			const painted = renderLayoutFrame(root, columns, rows, () => {});
			const lines = painted.lines.slice();
			const cursor = cursorOf(lines);
			return { tree: tree(painted.root), lines: visibleLines(lines), cursor };
		};

		const wide = frameTree(10, 3);
		const narrow = frameTree(8, 3);
		innerLeaf.setLines(["m1", "m2", "m3", "m4"], 3);
		const grown = frameTree(10, 3);
		root.removeChild(sideLeaf);
		const removed = frameTree(10, 3);
		root.addChild(markerLeaf(["s9"]), { basis: "auto" });
		const readded = frameTree(10, 3);

		scenarios.push({
			name: "hstack-nested",
			dimensions: { columns: 10, rows: 3 },
			inputs: {
				inner: { lines: ["m1", "m2"], cursorLine: 1, minSize: 2 },
				side: { lines: ["s1"], basis: 3, shrink: 1, minSize: 3, visibleAboveWidth: 10 },
				mutations: ["wide", "narrow", "grow-inner-lines", "remove-side", "add-auto-basis"],
			},
			expected: { wide, narrow, grown, removed, readded },
		});
	}

	// An HStack inside a VStack: the parent allocates the main axis, the child
	// stack allocates the frame width it was given, and both compose into the same
	// rows (the shape product panes use for side-by-side regions).
	{
		const inVStackOf = (columns: number) => {
			const row = new HStack(
				[
					{ component: markerLeaf(["left"]), basis: 6, shrink: 1, minSize: 1 },
					{ component: markerLeaf(["right"]), basis: 6, shrink: 1, minSize: 1 },
				],
				{ gap: 1 },
			);
			const root = new VStack([
				{ component: row, basis: 0, grow: 1, shrink: 1, minSize: 1 },
				{ component: markerLeaf(["f1"]), basis: 2, shrink: 0 },
			]);
			const painted = renderLayoutFrame(root, columns, 4, () => {});
			const lines = painted.lines.slice();
			const cursor = cursorOf(lines);
			const tree = (box: any): any => ({ ...rectOf(box), children: (box.children ?? []).map(tree) });
			return { tree: tree(painted.root), lines: visibleLines(lines), cursor };
		};
		scenarios.push({
			name: "hstack-in-vstack",
			dimensions: { columns: 14, rows: 4 },
			inputs: {
				entries: [
					{ lines: ["left", "right"], basis: 6, shrink: 1, minSize: 1 },
					{ lines: ["f1"], basis: 2, shrink: 0 },
				],
				widths: [14, 10],
			},
			expected: { wide: inVStackOf(14), narrow: inVStackOf(10) },
		});
	}

	const sourceEndpoints = [
		"packages/tui/src/components/stack.ts:allocateStackSizes,visibleStackEntries,Stack.addChild,StackEntryOptions",
		"packages/tui/src/components/v-stack.ts:VStack.render",
		"packages/tui/src/components/h-stack.ts:HStack.render",
		"packages/tui/src/layout.ts:renderLayoutFrame,layoutComponent,LayoutBox",
		"packages/tui/src/tui.ts:TuiBase.extractCursorPosition,compositeTuiLine,CURSOR_MARKER",
	];

	return { sourceEndpoints, scenarios };
}
