import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureSelectList(piCheckout: string) {
	const source = (relative: string) =>
		pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const { SelectList } = await import(source("src/components/select-list.ts"));
	const { visibleWidth } = await import(source("src/utils.ts"));
	const identity = (text: string) => text;
	const theme = {
		selectedPrefix: identity,
		selectedText: identity,
		description: identity,
		scrollInfo: identity,
		noMatch: identity,
	};
	const observe = (list: InstanceType<typeof SelectList>, width: number) => {
		const rows = [...list.render(width)];
		return {
			selected: list.getSelectedItem()?.value ?? null,
			rows,
			visibleWidths: rows.map((row) => visibleWidth(row)),
		};
	};
	const sourceEndpoints = [
		"packages/tui/src/components/select-list.ts:constructor,render,handleInput,getSelectedItem",
		"packages/tui/src/keybindings.ts:getKeybindings",
		"packages/tui/src/utils.ts:visibleWidth,truncateToWidth",
	];

	const callbackEvents: string[] = [];
	const cycle = new SelectList(
		[
			{ value: "alpha", label: "Alpha" },
			{ value: "beta", label: "Beta" },
			{ value: "gamma", label: "Gamma" },
		],
		5,
		theme,
	);
	cycle.onSelectionChange = (item) => callbackEvents.push(`change:${item.value}`);
	cycle.onSelect = (item) => callbackEvents.push(`select:${item.value}`);
	const cycleInitial = observe(cycle, 24);
	cycle.handleInput("\x1b[A");
	const cycleUp = observe(cycle, 24);
	cycle.handleInput("\x1b[B");
	cycle.handleInput("\x1b[B");
	const cycleWrapped = observe(cycle, 24);
	cycle.handleInput("\r");

	const windowList = new SelectList(
		Array.from({ length: 9 }, (_, index) => ({
			value: `item-${index + 1}`,
			label: `Item ${index + 1}`,
		})),
		5,
		theme,
	);
	const windowInitial = observe(windowList, 24);
	for (let index = 0; index < 6; index++) windowList.handleInput("\x1b[B");
	const windowMiddle = observe(windowList, 24);
	for (let index = 0; index < 2; index++) windowList.handleInput("\x1b[B");
	const windowEnd = observe(windowList, 24);

	const descriptionList = new SelectList(
		[
			{ value: "a", label: "Alpha", description: "First descriptive row" },
			{ value: "b", label: "Beta", description: "Second descriptive row" },
		],
		5,
		theme,
	);
	const descriptions = [40, 41, 60].map((width) => ({ width, ...observe(descriptionList, width) }));

	const narrowList = new SelectList(
		[{ value: "wide", label: "界界界界", description: "hidden when narrow" }],
		5,
		theme,
	);
	const narrow = observe(narrowList, 10);

	const bubblingList = new SelectList(
		[
			{ value: "first", label: "First" },
			{ value: "second", label: "Second" },
		],
		5,
		theme,
	);
	const bubbleInputs = ["j", "k", "\x1b[6~", "/", "x"];
	const bubbling = bubbleInputs.map((input) => {
		const before = observe(bubblingList, 24);
		bubblingList.handleInput(input);
		return { input, before: before.selected, after: observe(bubblingList, 24).selected };
	});

	return {
		sourceEndpoints,
		scenarios: [
			{
				name: "select-list-cycling-and-selection",
				dimensions: { columns: 24, rows: 8 },
				inputs: { keys: ["up", "down", "down", "enter"], items: ["alpha", "beta", "gamma"] },
				expected: { initial: cycleInitial, up: cycleUp, wrapped: cycleWrapped, callbackEvents },
			},
			{
				name: "select-list-centered-five-row-window",
				dimensions: { columns: 24, rows: 7 },
				inputs: { itemCount: 9, maxVisible: 5, downPresses: 6, thenDownPresses: 2 },
				expected: { initial: windowInitial, middle: windowMiddle, end: windowEnd },
			},
			{
				name: "select-list-description-width-boundaries",
				dimensions: { columns: 60, rows: 4 },
				inputs: { widths: [40, 41, 60] },
				expected: { widths: descriptions },
			},
			{
				name: "select-list-narrow-wide-glyph-truncation",
				dimensions: { columns: 10, rows: 2 },
				inputs: { width: 10, label: "界界界界", description: "hidden when narrow" },
				expected: narrow,
			},
			{
				name: "select-list-unbound-key-forwarding",
				dimensions: { columns: 24, rows: 3 },
				inputs: { keys: bubbleInputs },
				expected: { observations: bubbling },
			},
		],
	};
}
