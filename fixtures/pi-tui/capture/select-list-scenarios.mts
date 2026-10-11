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
		"packages/tui/src/components/select-list.ts:constructor,render,handleInput,getSelectedItem,setFilter,setSelectedIndex",
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

	const filterItems = [
		{ value: "Alpha", label: "Visible only" },
		{ value: "Straße", label: "Other" },
		{ value: "alphabet", label: "Prefix candidate" },
		{ value: "axlxp", label: "alp label-only trap" },
		{ value: "unrelated", label: "Alpha label-only" },
		{ value: "İSTANBUL", label: "Dotted-I candidate" },
		{ value: "ΟΣΟΣ", label: "Greek candidate" },
		{ value: "𐐀lpha", label: "Supplementary candidate" },
	];
	const filterOperations = [
		{ name: "select-before-filter", kind: "select", index: 2 },
		{ name: "value-prefix-not-label-or-subsequence", kind: "filter", value: "alp" },
		{ name: "select-second-prefix-result", kind: "select", index: 1 },
		{ name: "same-filter-resets-selection", kind: "filter", value: "alp" },
		{ name: "single-prefix-result", kind: "filter", value: "alphabet" },
		{ name: "clear-filter-resets-selection", kind: "filter", value: "" },
		{ name: "sharp-s-is-not-case-fold-expanded", kind: "filter", value: "STRASSE" },
		{ name: "sharp-s-lowercase-prefix", kind: "filter", value: "straße" },
		{ name: "dotted-i-expanded-prefix", kind: "filter", value: "İS" },
		{ name: "dotted-i-does-not-lose-combining-dot", kind: "filter", value: "istan" },
		{ name: "dotted-i-short-prefix", kind: "filter", value: "i" },
		{ name: "uppercase-greek-prefix-final-sigma-context", kind: "filter", value: "ΟΣ" },
		{ name: "internal-greek-sigma-prefix", kind: "filter", value: "οσ" },
		{ name: "whole-greek-value-final-sigma-context", kind: "filter", value: "ΟΣΟΣ" },
		{ name: "supplementary-lowercase-prefix", kind: "filter", value: "𐐨L" },
	];
	const filterList = new SelectList(filterItems, 5, theme);
	const filterEvents: string[] = [];
	filterList.onSelectionChange = (item) => filterEvents.push(`change:${item.value}`);
	// Enumerate identities only through public selection APIs. These items
	// have distinct values; clamping to the final item terminates enumeration.
	const observeFiltered = () => {
		const snapshot = observe(filterList, 60);
		const values: string[] = [];
		for (let index = 0; index < filterItems.length; index++) {
			filterList.setSelectedIndex(index);
			const item = filterList.getSelectedItem();
			if (!item || values.at(-1) === item.value) break;
			values.push(item.value);
		}
		filterList.setSelectedIndex(Math.max(0, values.indexOf(snapshot.selected ?? "")));
		return { ...snapshot, filteredValues: values };
	};
	const filters = filterOperations.map((operation) => {
		if (operation.kind === "filter") filterList.setFilter(operation.value);
		else filterList.setSelectedIndex(operation.index);
		return { name: operation.name, ...observeFiltered() };
	});

	const normalizedDescriptionItems = [
		{ value: "nbsp", label: "NBSP", description: "\u00a0 hello \u00a0" },
		{ value: "ideographic", label: "Wide space", description: "\u3000 wide words \u3000" },
		{ value: "bom", label: "BOM", description: "\ufeff hello \ufeff" },
		{ value: "crlf", label: "CRLF", description: "\u00a0first\r\n\r\nsecond\u3000" },
		{ value: "empty", label: "Whitespace only", description: "\u3000\u00a0\ufeff\r\n" },
	];
	const normalizedDescriptionList = new SelectList(normalizedDescriptionItems, 5, theme);
	const normalizedDescriptions = [0, 1, 2, 3, 4].map((index) => {
		normalizedDescriptionList.setSelectedIndex(index);
		return observe(normalizedDescriptionList, 80);
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
				name: "select-list-value-prefix-filter-and-selection-reset",
				dimensions: { columns: 60, rows: 7 },
				inputs: { items: filterItems, operations: filterOperations, maxVisible: 5 },
				expected: { observations: filters, callbackEvents: filterEvents },
			},
			{
				name: "select-list-unicode-description-normalization",
				dimensions: { columns: 80, rows: 5 },
				inputs: { items: normalizedDescriptionItems, selectedIndices: [0, 1, 2, 3, 4] },
				expected: { observations: normalizedDescriptions },
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
