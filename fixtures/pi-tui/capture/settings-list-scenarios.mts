import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureSettingsList(piCheckout: string) {
	const source = (relative: string) =>
		pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const { SettingsList } = await import(source("src/components/settings-list.ts"));
	const { visibleWidth } = await import(source("src/utils.ts"));

	const identity = (text: string) => text;
	const formatHint = (text: string) => {
		if (text.includes("Type to search")) {
			return "  Type to search · enter/space to change · escape/ctrl+c to cancel";
		}
		if (text.includes("to change")) {
			return "  enter/space to change · escape/ctrl+c to cancel";
		}
		return text;
	};
	const theme = {
		label: identity,
		value: identity,
		description: identity,
		cursor: "→ ",
		hint: formatHint,
	};

	const observe = (list: InstanceType<typeof SettingsList>, width: number) => {
		const rows = [...list.render(width)];
		return {
			rows,
			visibleWidths: rows.map((row) => visibleWidth(row)),
		};
	};

	const sourceEndpoints = [
		"packages/tui/src/components/settings-list.ts:constructor,render,renderMainList,handleInput,updateValue,selectItem,activateItem,closeSubmenu,applyFilter,addHintLine,getDisplayItems,getVisibleRange",
		"packages/tui/src/keybindings.ts:getKeybindings",
		"packages/tui/src/utils.ts:visibleWidth,truncateToWidth,wrapTextWithAnsi",
		"packages/tui/src/fuzzy.ts:fuzzyFilter",
		"packages/tui/src/components/input.ts:constructor,render,handleInput,getValue",
	];

	// 1. Cycling through value controls, wrapping, space confirmation, and cancel
	const cycleChanges: Array<{ id: string; value: string }> = [];
	let cycleCancels = 0;
	const cycleItems = [
		{ id: "theme", label: "Theme", currentValue: "dark", values: ["dark", "light"], description: "Color theme" },
		{ id: "editor", label: "Editor", currentValue: "vim", values: ["vim", "emacs", "nano"] },
		{ id: "wrap", label: "Word Wrap", currentValue: "off", values: ["off", "on"] },
	];
	const cycleList = new SettingsList(
		cycleItems,
		5,
		theme,
		(id, value) => cycleChanges.push({ id, value }),
		() => {
			cycleCancels++;
		},
	);
	const cycleInitial = observe(cycleList, 50);
	cycleList.handleInput("\x1b[A"); // Up arrow wraps to bottom item ("wrap")
	const cycleWrappedUp = observe(cycleList, 50);
	cycleList.handleInput(" "); // Space confirms and cycles "wrap" from "off" to "on"
	const cycleAfterSpace = observe(cycleList, 50);
	cycleList.handleInput("\x1b[A"); // Up arrow to "editor"
	cycleList.handleInput("\r"); // Enter confirms and cycles "editor" from "vim" to "emacs"
	const cycleAfterEnter = observe(cycleList, 50);
	cycleList.handleInput("\x1b[B"); // Down arrow to "wrap"
	cycleList.handleInput("\x1b[B"); // Down arrow wraps to top item ("theme")
	cycleList.handleInput("\x1b"); // Esc calls onCancel

	// 2. Centered five-row windowing and scroll indicators
	const windowChanges: Array<{ id: string; value: string }> = [];
	const windowItems = Array.from({ length: 9 }, (_, index) => ({
		id: `setting-${index + 1}`,
		label: `Setting ${index + 1}`,
		currentValue: `val-${index + 1}`,
		values: [`val-${index + 1}`, `alt-${index + 1}`],
	}));
	const windowList = new SettingsList(
		windowItems,
		5,
		theme,
		(id, value) => windowChanges.push({ id, value }),
		() => {},
	);
	const windowInitial = observe(windowList, 50);
	for (let i = 0; i < 6; i++) windowList.handleInput("\x1b[B");
	const windowMiddle = observe(windowList, 50);
	for (let i = 0; i < 2; i++) windowList.handleInput("\x1b[B");
	const windowEnd = observe(windowList, 50);

	// 3. Description wrapping, label alignment, value column layout and hints
	const descriptionItems = [
		{
			id: "opt1",
			label: "First Option",
			currentValue: "enabled",
			description: "Short description line",
		},
		{
			id: "opt2",
			label: "Very Long Setting Option Label Exceeding Width",
			currentValue: "custom-value-data",
			description: "Multi-line description text that must wrap cleanly across lines.\nSecond explicit paragraph of description with additional information.",
		},
	];
	const descriptionList = new SettingsList(
		descriptionItems,
		5,
		theme,
		() => {},
		() => {},
	);
	const descriptions = [40, 60, 80].map((width) => ({
		width,
		...observe(descriptionList, width),
	}));

	// 4. Search filtering, fuzzy matching, space confirmation behavior, and empty search
	const searchChanges: Array<{ id: string; value: string }> = [];
	const searchItems = [
		{ id: "term", label: "Terminal Bell", currentValue: "audible", values: ["audible", "visual", "none"] },
		{ id: "theme", label: "Color Theme", currentValue: "dark", values: ["dark", "light"] },
		{ id: "telemetry", label: "Telemetry Report", currentValue: "off", values: ["off", "on"] },
		{ id: "wrap", label: "Word Wrap", currentValue: "off", values: ["off", "on"] },
	];
	const searchList = new SettingsList(
		searchItems,
		5,
		theme,
		(id, value) => searchChanges.push({ id, value }),
		() => {},
		{ enableSearch: true },
	);
	const searchInitial = observe(searchList, 60);
	// Type "term"
	for (const ch of "term") searchList.handleInput(ch);
	const searchFiltered = observe(searchList, 60);
	// Space while search input is non-empty must NOT cycle value; it types space into query
	searchList.handleInput(" ");
	const searchAfterSpaceNonEmpty = observe(searchList, 60);
	// Backspace to clear search
	for (let i = 0; i < 5; i++) searchList.handleInput("\x7f");
	const searchCleared = observe(searchList, 60);
	// Space while search input is empty DOES cycle value
	searchList.handleInput(" ");
	const searchAfterSpaceEmpty = observe(searchList, 60);
	// Type search query that matches nothing
	for (const ch of "zzzzz") searchList.handleInput(ch);
	const searchNoMatch = observe(searchList, 60);

	// 5. Submenu opening, delegation, done callback with selectedValue and navigateTo
	const submenuChanges: Array<{ id: string; value: string }> = [];
	let openedSubmenuWith: string | null = null;
	const submenuItems = [
		{
			id: "model",
			label: "Model Selector",
			currentValue: "gpt-4o",
			submenu: (currentValue: string, done: (val?: string, opts?: { navigateTo?: string }) => void) => {
				openedSubmenuWith = currentValue;
				return {
					render: (width: number) => [
						`[Submenu: choosing model, current=${currentValue}, width=${width}]`,
					],
					handleInput: (key: string) => {
						if (key === "\r") {
							done("claude-3-5-sonnet", { navigateTo: "provider" });
						} else if (key === "\x1b") {
							done();
						}
					},
					invalidate: () => {},
				};
			},
		},
		{
			id: "provider",
			label: "API Provider",
			currentValue: "openai",
			values: ["openai", "anthropic"],
		},
	];
	const submenuList = new SettingsList(
		submenuItems,
		5,
		theme,
		(id, value) => submenuChanges.push({ id, value }),
		() => {},
	);
	const submenuInitial = observe(submenuList, 60);
	// Enter opens submenu
	submenuList.handleInput("\r");
	const submenuOpenRender = observe(submenuList, 60);
	// Enter in submenu calls done("claude-3-5-sonnet", { navigateTo: "provider" })
	// This closes submenu, updates model value, navigates to "provider", and activates "provider" (cycling it to "anthropic")!
	submenuList.handleInput("\r");
	const submenuAfterDoneAndNavigate = observe(submenuList, 60);

	// 6. updateValue and selectItem mutations
	const mutateItems = [
		{ id: "alpha", label: "Alpha", currentValue: "initial-a", values: ["initial-a", "next-a"] },
		{ id: "beta", label: "Beta", currentValue: "initial-b", values: ["initial-b", "next-b"] },
	];
	const mutateList = new SettingsList(
		mutateItems,
		5,
		theme,
		() => {},
		() => {},
	);
	mutateList.updateValue("beta", "updated-b");
	mutateList.selectItem("beta");
	const mutatedObservation = observe(mutateList, 50);

	return {
		sourceEndpoints,
		scenarios: [
			{
				name: "settings-list-cycling-and-selection",
				dimensions: { columns: 50, rows: 8 },
				inputs: {
					items: cycleItems.map((i) => ({ id: i.id, label: i.label, values: i.values })),
					keys: ["up", "space", "up", "enter", "down", "down", "escape"],
				},
				expected: {
					initial: cycleInitial,
					wrappedUp: cycleWrappedUp,
					afterSpace: cycleAfterSpace,
					afterEnter: cycleAfterEnter,
					changes: cycleChanges,
					cancels: cycleCancels,
				},
			},
			{
				name: "settings-list-centered-five-row-window",
				dimensions: { columns: 50, rows: 7 },
				inputs: { itemCount: 9, maxVisible: 5, downPresses: 6, thenDownPresses: 2 },
				expected: { initial: windowInitial, middle: windowMiddle, end: windowEnd },
			},
			{
				name: "settings-list-description-and-hint",
				dimensions: { columns: 60, rows: 8 },
				inputs: { widths: [40, 60, 80] },
				expected: { widths: descriptions },
			},
			{
				name: "settings-list-search-filtering-and-input",
				dimensions: { columns: 60, rows: 10 },
				inputs: { items: searchItems.map((i) => ({ id: i.id, label: i.label })) },
				expected: {
					initial: searchInitial,
					filtered: searchFiltered,
					afterSpaceNonEmpty: searchAfterSpaceNonEmpty,
					cleared: searchCleared,
					afterSpaceEmpty: searchAfterSpaceEmpty,
					noMatch: searchNoMatch,
					changes: searchChanges,
				},
			},
			{
				name: "settings-list-submenu-navigation-and-lifecycle",
				dimensions: { columns: 60, rows: 6 },
				inputs: { items: [{ id: "model" }, { id: "provider" }] },
				expected: {
					openedWith: openedSubmenuWith,
					initial: submenuInitial,
					openSubmenu: submenuOpenRender,
					afterDoneAndNavigate: submenuAfterDoneAndNavigate,
					changes: submenuChanges,
				},
			},
			{
				name: "settings-list-update-value-and-select-item",
				dimensions: { columns: 50, rows: 5 },
				inputs: { updateId: "beta", newValue: "updated-b", selectId: "beta" },
				expected: { observation: mutatedObservation },
			},
		],
	};
}
