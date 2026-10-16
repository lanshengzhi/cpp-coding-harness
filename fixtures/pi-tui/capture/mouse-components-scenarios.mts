import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent mouse editing and list/settings selection observations from
/// frozen pi-v1.0.4 (#1003): the press/click cursor and row mapping of `Input`,
/// `Editor`, `SelectList`, and `SettingsList`, their hover and wheel rules, the
/// submenu/search delegation paths, and the alternate-screen focus inheritance
/// of a delegating host. Expectations are observed from the frozen checkout
/// only; nothing here is derived from Pike.
export async function captureMouseComponents(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Input } = await import(source("src/components/input.ts"));
	const { Editor } = await import(source("src/components/editor.ts"));
	const { SelectList } = await import(source("src/components/select-list.ts"));
	const { SettingsList } = await import(source("src/components/settings-list.ts"));
	const { Container } = await import(source("src/tui.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	const { visibleWidth } = await import(source("src/utils.ts"));

	const identity = (text: string) => text;
	const selectTheme = {
		selectedPrefix: identity,
		selectedText: identity,
		description: identity,
		scrollInfo: identity,
		noMatch: identity,
	};
	const settingsTheme = {
		label: identity,
		value: identity,
		description: identity,
		cursor: "> ",
		hint: identity,
	};
	const editorTheme = { borderColor: identity, selectList: selectTheme };

	const mouse = (
		type: string,
		x: number,
		y: number,
		width = 80,
		height = 10,
		overrides: Record<string, unknown> = {},
	) => ({
		type,
		button: "left",
		x,
		y,
		screenX: x,
		screenY: y,
		width,
		height,
		shift: false,
		alt: false,
		ctrl: false,
		...(type === "click" ? { clickCount: 1 } : {}),
		...overrides,
	});

	/// Rows as the toolkit renders them with terminal styling removed, so a
	/// dangling cell style or a different reverse-video reset cannot decide a
	/// comparison. The reverse-video cursor cell keeps its width.
	const plainRows = (rows: readonly string[]) =>
		rows.map((row) => row.replace(/\x1b_pi:c\x07/g, "").replace(/\x1b\[[0-9;]*m/g, "").replace(/\s+$/, ""));

	/// The rendered column of the fake cursor: the visible width before the
	/// reverse-video cell marker (`input.ts` render).
	const cursorColumnOf = (row: string) => {
		const index = row.indexOf("\x1b[7m");
		return index < 0 ? -1 : visibleWidth(row.slice(0, index));
	};

	const resultOf = (value: unknown) => {
		if (value === undefined || value === null) return null;
		const result = value as Record<string, unknown>;
		return {
			handled: result.handled === true,
			focus: result.focus === true,
			render: typeof result.render === "boolean" ? result.render : null,
		};
	};

	// Input: a left press on the rendered row places the cursor; every other
	// gesture stays unhandled so the renderer's screen selection owns it.
	const inputCases = [
		{ label: "value-column-two", x: 4, y: 0, width: 20, height: 1, type: "press", button: "left" },
		{ label: "prompt-column-zero", x: 2, y: 0, width: 20, height: 1, type: "press", button: "left" },
		{ label: "past-line-end", x: 20, y: 0, width: 20, height: 1, type: "press", button: "left" },
		{ label: "right-button", x: 4, y: 0, width: 20, height: 1, type: "press", button: "right" },
		{ label: "release", x: 4, y: 0, width: 20, height: 1, type: "release", button: "left" },
		{ label: "move", x: 4, y: 0, width: 20, height: 1, type: "move", button: "none" },
		{ label: "wheel", x: 4, y: 0, width: 20, height: 1, type: "wheel", button: "left" },
		{ label: "second-row", x: 4, y: 1, width: 20, height: 1, type: "press", button: "left" },
	];
	const inputObservation = inputCases.map((inputCase) => {
		const input = new Input({ prompt: "> " });
		input.setValue("hello");
		input.render(20);
		const result = input.handleMouse(
			mouse(inputCase.type, inputCase.x, inputCase.y, 20, 1, { button: inputCase.button }) as never,
		);
		const afterClick = input.render(20);
		input.handleInput("X");
		return {
			...inputCase,
			prompt: "> ",
			value: "hello",
			result: resultOf(result),
			cursorColumn: cursorColumnOf(afterClick[0] ?? ""),
			row: plainRows(afterClick)[0],
			valueAfterType: input.getValue(),
		};
	});

	// Input: the column arithmetic follows the grapheme widths, and every later
	// press is offset by the start column of the window the previous press left
	// scrolled. Each case lists its presses in order; the row and cursor cell of
	// every step are recorded, and typing happens after the last one.
	const inputColumnCases = [
		{ label: "cjk-inside-first-grapheme", value: "你好", width: 20, height: 1, presses: [4] },
		{ label: "cjk-inside-second-grapheme", value: "你好", width: 20, height: 1, presses: [5] },
		{ label: "cjk-past-end", value: "你好", width: 20, height: 1, presses: [6] },
		{ label: "scrolled-column-zero", value: "abcdefghij", width: 8, height: 1, presses: [7, 2] },
		{ label: "scrolled-column-one", value: "abcdefghij", width: 8, height: 1, presses: [7, 3] },
		{ label: "scrolled-past-end", value: "abcdefghij", width: 8, height: 1, presses: [7, 7] },
		{ label: "scrolled-wide-grapheme", value: "一二三四五六七八", width: 8, height: 1, presses: [20, 4] },
	];
	const inputColumnObservation = inputColumnCases.map((inputCase) => {
		const input = new Input({ prompt: "> " });
		input.setValue(inputCase.value);
		const steps = inputCase.presses.map((x) => {
			input.render(inputCase.width);
			const result = input.handleMouse(mouse("press", x, 0, inputCase.width, 1) as never);
			const afterClick = input.render(inputCase.width);
			return {
				x,
				result: resultOf(result),
				row: plainRows(afterClick)[0],
				cursorColumn: cursorColumnOf(afterClick[0] ?? ""),
			};
		});
		input.handleInput("X");
		return { ...inputCase, prompt: "> ", y: 0, steps, valueAfterType: input.getValue() };
	});

	// SelectList: press selects the visible row, the synthesized click activates
	// the pressed row, and both answer with the frozen result shape.
	const plainList = new SelectList(
		[
			{ value: "a", label: "A" },
			{ value: "b", label: "B" },
			{ value: "c", label: "C" },
			{ value: "d", label: "D" },
			{ value: "e", label: "E" },
		],
		3,
		selectTheme,
	);
	let plainSelected: string | undefined;
	plainList.onSelect = (item) => {
		plainSelected = item.value;
	};
	const plainPress = plainList.handleMouse(mouse("press", 1, 2, 40, 3) as never);
	const plainPressSelection = plainList.getSelectedItem()?.value ?? null;
	const plainClick = plainList.handleMouse(mouse("click", 1, 2, 40, 3) as never);
	const listPressClick = {
		maxVisible: 3,
		items: ["a", "b", "c", "d", "e"],
		press: { x: 1, y: 2, width: 40, height: 3, result: resultOf(plainPress), selected: plainPressSelection },
		click: { x: 1, y: 2, width: 40, height: 3, result: resultOf(plainClick), selected: plainSelected ?? null },
	};

	// SelectList: after a wheel notch the visible range is centered on the new
	// selection, hover never changes it, and a press/click maps through that
	// range instead of the raw row.
	const scrolledRowObservation = [0, 4].map((row) => {
		const list = new SelectList(
			Array.from({ length: 12 }, (_, index) => ({ value: `item-${index}`, label: `Item ${index}` })),
			5,
			selectTheme,
		);
		const changes: string[] = [];
		let selected: string | undefined;
		list.onSelectionChange = (item) => changes.push(item.value);
		list.onSelect = (item) => {
			selected = item.value;
		};
		list.setSelectedIndex(5);
		const wheel = list.handleMouse({ ...mouse("wheel", 1, row), wheelDelta: 1 } as never);
		const afterWheel = {
			result: resultOf(wheel),
			selected: list.getSelectedItem()?.value ?? null,
			changes: [...changes],
		};
		const rows = plainRows(list.render(80));
		const hover = [0, 1, 2, 3, 4, row].map((y) => {
			const result = list.handleMouse({ ...mouse("move", 1, y), button: "none" } as never);
			return { y, result: resultOf(result), rows: plainRows(list.render(80)) };
		});
		const press = list.handleMouse(mouse("press", 1, row) as never);
		list.render(80);
		const click = list.handleMouse(mouse("click", 1, row) as never);
		return {
			row,
			afterWheel,
			rows,
			hover,
			press: { result: resultOf(press), selected: list.getSelectedItem()?.value ?? null, changes: [...changes] },
			click: { result: resultOf(click), selected: selected ?? null, changes: [...changes] },
		};
	});

	// SettingsList: a press selects a row and the click activates it, cycling a
	// value control through the change sink.
	const settingsChanges: Array<{ id: string; value: string }> = [];
	const plainSettings = new SettingsList(
		[
			{ id: "mode", label: "Mode", currentValue: "one", values: ["one", "two"] },
			{ id: "other", label: "Other", currentValue: "off", values: ["off", "on"] },
			{ id: "third", label: "Third", currentValue: "low", values: ["low", "high"] },
			{ id: "fourth", label: "Fourth", currentValue: "x", values: ["x", "y"] },
		],
		3,
		settingsTheme,
		(id: string, value: string) => settingsChanges.push({ id, value }),
		() => {},
	);
	const settingsPress = plainSettings.handleMouse(mouse("press", 1, 2, 40, 5) as never);
	const settingsClick = plainSettings.handleMouse(mouse("click", 1, 2, 40, 5) as never);
	const settingsActivation = {
		maxVisible: 3,
		items: ["mode", "other", "third", "fourth"],
		press: { x: 1, y: 2, width: 40, height: 5, result: resultOf(settingsPress) },
		click: { x: 1, y: 2, width: 40, height: 5, result: resultOf(settingsClick) },
		changes: settingsChanges,
	};

	// SettingsList with search enabled: the search row and its separator shift
	// every item row by two.
	const settingsScrolledObservation = [0, 4].map((row) => {
		const changes: Array<{ id: string; value: string }> = [];
		const list = new SettingsList(
			Array.from({ length: 12 }, (_, index) => ({
				id: `item-${index}`,
				label: `Item ${index}`,
				description: `Description ${index}`,
				currentValue: "off",
				values: ["off", "on"],
			})),
			5,
			settingsTheme,
			(id: string, value: string) => changes.push({ id, value }),
			() => {},
			{ enableSearch: true },
		);
		list.selectItem("item-5");
		const wheel = list.handleMouse({ ...mouse("wheel", 1, row + 2), wheelDelta: 1 } as never);
		const afterWheel = { result: resultOf(wheel), rows: plainRows(list.render(80)) };
		const hover = [0, 1, 2, 3, 4, row].map((y) => {
			const result = list.handleMouse({ ...mouse("move", 1, y + 2), button: "none" } as never);
			return { y: y + 2, result: resultOf(result), rows: plainRows(list.render(80)) };
		});
		const press = list.handleMouse(mouse("press", 1, row + 2) as never);
		const rowsAfterPress = plainRows(list.render(80));
		const click = list.handleMouse(mouse("click", 1, row + 2) as never);
		return {
			row,
			rowOffset: 2,
			afterWheel,
			hover,
			press: { result: resultOf(press), rows: rowsAfterPress },
			click: { result: resultOf(click), rows: plainRows(list.render(80)), changes: [...changes] },
		};
	});

	// SettingsList with search enabled: the search row forwards to the search
	// Input (the settings list keeps the target and the focus), the separator
	// row answers nothing, and the first item row is one below the separator.
	const searchChanges: Array<{ id: string; value: string }> = [];
	const searchList = new SettingsList(
		Array.from({ length: 4 }, (_, index) => ({
			id: `item-${index}`,
			label: `Item ${index}`,
			currentValue: "off",
			values: ["off", "on"],
		})),
		5,
		settingsTheme,
		(id: string, value: string) => searchChanges.push({ id, value }),
		() => {},
		{ enableSearch: true },
	);
	searchList.handleInput("i");
	searchList.handleInput("t");
	const filteredRows = plainRows(searchList.render(80));
	const searchRowResult = searchList.handleMouse(mouse("press", 3, 0, 80, 6) as never);
	const searchRowRows = searchList.render(80);
	const searchSeparator = searchList.handleMouse(mouse("press", 3, 1, 80, 6) as never);
	const searchFirstItemPress = searchList.handleMouse(mouse("press", 3, 2, 80, 6) as never);
	const firstItemRows = plainRows(searchList.render(80));
	const searchFirstItemClick = searchList.handleMouse(mouse("click", 3, 2, 80, 6) as never);
	const changesAfterFirstItem = [...searchChanges];
	// Typing after the search-row press edits the search input at its clicked
	// cursor instead of replacing it.
	searchList.handleInput("X");
	const settingsSearch = {
		rows: filteredRows,
		typedQuery: "it",
		searchRow: {
			x: 3,
			y: 0,
			width: 80,
			height: 6,
			result: resultOf(searchRowResult),
			cursorColumnBeforeClick: cursorColumnOf(searchList.render(80)[0] ?? ""),
			afterClick: plainRows(searchRowRows)[0],
			cursorColumnAfterClick: cursorColumnOf(searchRowRows[0] ?? ""),
			queryAfterType: searchList.searchInput?.getValue?.() ?? null,
			edited: "X",
		},
		separatorRow: { x: 3, y: 1, width: 80, height: 6, result: resultOf(searchSeparator) },
		firstItemRow: {
			x: 3,
			y: 2,
			width: 80,
			height: 6,
			press: resultOf(searchFirstItemPress),
			rowsAfterPress: firstItemRows,
			click: resultOf(searchFirstItemClick),
			changesAfterClick: changesAfterFirstItem,
		},
	};

	// Alternate screen: a delegating host keeps keyboard focus when its nested
	// Input is clicked, and the click positions that Input's cursor.
	class InputOverlay extends Container {
		readonly input = new Input();

		constructor() {
			super();
			this.addChild(this.input);
		}

		handleInput(data: string): void {
			this.input.handleInput(data);
		}
	}

	const overlayObservation = [];
	for (const column of [5, 3]) {
		const terminal = new VirtualTerminal(20, 4);
		const tui = new TuiAltScreen(terminal);
		const overlay = new InputOverlay();
		overlay.input.setValue("hi");
		const focusedBefore = tui.getFocusedComponent() === overlay;
		tui.start();
		tui.showOverlay(overlay, { anchor: "top-left", width: 20 });
		await terminal.waitForRender();
		terminal.sendInput(`\x1b[<0;${column};1M`);
		terminal.sendInput(`\x1b[<0;${column};1m`);
		terminal.sendInput("!");
		await terminal.waitForRender();
		overlayObservation.push({
			column,
			hostFocusedBeforeOverlay: focusedBefore,
			value: overlay.input.getValue(),
			hostFocusedAfterClick: tui.getFocusedComponent() === overlay,
			screen: terminal.getViewport().map((line) => line.trimEnd()),
		});
		tui.stop();
	}

	// Alternate screen: a click in an open settings submenu selects its row,
	// closes the submenu, keeps the settings list focused, and keys reach the
	// visible list again.
	class SubmenuHost extends Container {
		readonly list = new SelectList(
			[
				{ value: "first", label: "First" },
				{ value: "second", label: "Second" },
			],
			5,
			selectTheme,
		);

		constructor(done: (value?: string) => void) {
			super();
			this.list.onSelect = (item) => done(item.value);
			this.list.onCancel = () => done();
			this.addChild(this.list);
		}

		handleInput(data: string): void {
			this.list.handleInput(data);
		}
	}

	const submenuTerminal = new VirtualTerminal(30, 6);
	const submenuTui = new TuiAltScreen(submenuTerminal);
	const submenuChanges: Array<{ id: string; value: string }> = [];
	const submenuSettings = new SettingsList(
		[
			{ id: "theme", label: "Theme", currentValue: "first", submenu: (_value, done) => new SubmenuHost(done) },
			{ id: "other", label: "Other", currentValue: "off", values: ["off", "on"] },
		],
		5,
		settingsTheme,
		(id: string, value: string) => submenuChanges.push({ id, value }),
		() => {},
	);
	submenuTui.addChild(submenuSettings);
	submenuTui.setFocus(submenuSettings);
	submenuTui.start();
	await submenuTerminal.waitForRender();
	const submenuScreens = [submenuTerminal.getViewport().map((line) => line.trimEnd())];
	submenuTerminal.sendInput("\r");
	await submenuTerminal.waitForRender();
	submenuScreens.push(submenuTerminal.getViewport().map((line) => line.trimEnd()));
	const submenuOpen = submenuSettings.submenuComponent != null;
	submenuTerminal.sendInput("\x1b[<0;3;2M");
	submenuTerminal.sendInput("\x1b[<0;3;2m");
	await submenuTerminal.waitForRender();
	submenuScreens.push(submenuTerminal.getViewport().map((line) => line.trimEnd()));
	const changesAfterClick = [...submenuChanges];
	const focusedAfterClick = submenuTui.getFocusedComponent() === submenuSettings;
	const closedAfterClick = submenuSettings.submenuComponent == null;
	submenuTerminal.sendInput("\x1b[B");
	submenuTerminal.sendInput("\r");
	await submenuTerminal.waitForRender();
	submenuScreens.push(submenuTerminal.getViewport().map((line) => line.trimEnd()));
	const settingsSubmenu = {
		keys: ["\r", "\x1b[B", "\r"],
		click: { press: "\x1b[<0;3;2M", release: "\x1b[<0;3;2m" },
		submenuOpen,
		changesAfterClick,
		focusedAfterClick,
		closedAfterClick,
		changesAfterKeys: [...submenuChanges],
		screens: submenuScreens,
	};
	submenuTui.stop();

	// Alternate screen: the editor positions its cursor through the dispatch of
	// a synthesized click and keeps the keyboard focus.
	const editorTerminal = new VirtualTerminal(20, 6);
	const editorTui = new TuiAltScreen(editorTerminal);
	const editor = new Editor(editorTui, editorTheme);
	editor.setText("hello");
	editorTui.addChild(editor);
	editorTui.start();
	await editorTerminal.waitForRender();
	const editorScreens = [editorTerminal.getViewport().map((line) => line.trimEnd())];
	editorTerminal.sendInput("\x1b[<0;3;2M");
	editorTerminal.sendInput("\x1b[<0;3;2m");
	await editorTerminal.waitForRender();
	const cursorAfterClick = { ...editor.getCursor() };
	editorTerminal.sendInput("X");
	await editorTerminal.waitForRender();
	editorScreens.push(editorTerminal.getViewport().map((line) => line.trimEnd()));
	const editorAltScreen = {
		text: "hello",
		columns: 20,
		rows: 6,
		click: { press: "\x1b[<0;3;2M", release: "\x1b[<0;3;2m" },
		resolvedClick: { x: 2, y: 1, width: 20, height: 3 },
		cursorAfterClick,
		textAfterType: editor.getText(),
		focused: editorTui.getFocusedComponent() === editor,
		screens: editorScreens,
	};
	editorTui.stop();

	// Editor: the click row bounds are the rendered content rows, and a click
	// past a wrapped segment's end stays on that segment's last grapheme.
	const editorDirectCases = [
		{ label: "top-border-row", text: "hello", width: 20, x: 3, y: 0 },
		{ label: "content-row", text: "hello", width: 20, x: 3, y: 1 },
		{ label: "past-content-end", text: "hello", width: 20, x: 3, y: 2 },
		{ label: "bottom-border-row", text: "hello", width: 20, x: 3, y: 3 },
		{ label: "wrapped-first-segment-past-end", text: "abcdefghij", width: 6, x: 6, y: 1 },
		{ label: "wrapped-second-segment", text: "abcdefghij", width: 6, x: 2, y: 2 },
		{ label: "cjk-wide-grapheme", text: "世界", width: 20, x: 3, y: 1 },
		{ label: "cjk-past-end", text: "世界", width: 20, x: 5, y: 1 },
		{ label: "padded-content", text: "hello", width: 20, x: 4, y: 1, paddingX: 2 },
	];
	const editorDirectObservation = editorDirectCases.map((editorCase) => {
		const editorInstance = new Editor(new TuiAltScreen(new VirtualTerminal(80, 24)), editorTheme);
		editorInstance.setPaddingX(editorCase.paddingX ?? 0);
		editorInstance.setText(editorCase.text);
		const rows = editorInstance.render(editorCase.width);
		const cursorBefore = { ...editorInstance.getCursor() };
		const result = editorInstance.handleMouse(
			mouse("click", editorCase.x, editorCase.y, editorCase.width, rows.length) as never,
		);
		const cursorAfterClick = { ...editorInstance.getCursor() };
		editorInstance.handleInput("X");
		return {
			...editorCase,
			paddingX: editorCase.paddingX ?? 0,
			prompt: null,
			rows,
			result: resultOf(result),
			cursorBefore,
			cursorAfterClick,
			textAfterType: editorInstance.getText(),
		};
	});

	// Editor: press, drag, and release stay unhandled so the renderer's
	// screen-level selection copies the dragged text instead of moving the
	// cursor.
	const dragTerminal = new VirtualTerminal(20, 6);
	const copied: string[] = [];
	const dragTui = new TuiAltScreen(dragTerminal, undefined, undefined, {
		copySelection: async (text: string) => {
			copied.push(text);
			return true;
		},
	});
	const dragEditor = new Editor(dragTui, editorTheme);
	dragEditor.setText("hello world");
	dragTui.addChild(dragEditor);
	dragTui.start();
	await dragTerminal.waitForRender();
	const dragCursorBefore = { ...dragEditor.getCursor() };
	dragTerminal.sendInput("\x1b[<0;1;2M");
	dragTerminal.sendInput("\x1b[<32;5;2M");
	dragTerminal.sendInput("\x1b[<0;5;2m");
	await dragTerminal.waitForRender();
	const editorDrag = {
		text: "hello world",
		press: "\x1b[<0;1;2M",
		drag: "\x1b[<32;5;2M",
		release: "\x1b[<0;5;2m",
		copied,
		cursorBefore: dragCursorBefore,
		cursorAfter: { ...dragEditor.getCursor() },
		textAfterDrag: dragEditor.getText(),
		screen: dragTerminal.getViewport().map((line) => line.trimEnd()),
	};
	dragTui.stop();

	return {
		sourceEndpoints: [
			"packages/tui/src/components/input.ts:handleMouse,render,handleInput,setValue,getValue",
			"packages/tui/src/components/editor.ts:handleMouse,render,setText,setPaddingX,handleInput,getCursor,getText",
			"packages/tui/src/components/select-list.ts:handleMouse,render,setSelectedIndex,getSelectedItem",
			"packages/tui/src/components/settings-list.ts:handleMouse,render,selectItem,getSelectedItem,handleInput",
			"packages/tui/src/tui.ts:Container.handleMouse,dispatchMouseEvent",
			"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.dispatchMouseEvent,TuiAltScreen.getFocusedComponent",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.sendInput,VirtualTerminal.getViewport",
		],
		scenarios: [
			{
				name: "input-press-cursor-and-unhandled-gestures",
				dimensions: { columns: 20, rows: 1 },
				inputs: {
					prompt: "> ",
					value: "hello",
					width: 20,
					typed: "X",
					cases: inputCases,
				},
				expected: { cases: inputObservation },
			},
			{
				name: "input-press-maps-grapheme-widths-and-scrolled-columns",
				dimensions: { columns: 20, rows: 1 },
				inputs: {
					prompt: "> ",
					typed: "X",
					cases: inputColumnCases,
				},
				expected: { cases: inputColumnObservation },
			},
			{
				name: "select-list-press-selects-and-click-activates",
				dimensions: { columns: 40, rows: 3 },
				inputs: { items: ["a", "b", "c", "d", "e"], maxVisible: 3 },
				expected: { pressClick: listPressClick },
			},
			{
				name: "select-list-wheel-hover-and-scrolled-rows",
				dimensions: { columns: 80, rows: 5 },
				inputs: { itemCount: 12, maxVisible: 5, selectedIndex: 5, wheelDelta: 1, rows: [0, 4] },
				expected: { rows: scrolledRowObservation },
			},
			{
				name: "settings-list-press-selects-and-click-activates",
				dimensions: { columns: 40, rows: 5 },
				inputs: { items: ["mode", "other", "third", "fourth"], maxVisible: 3 },
				expected: { pressClick: settingsActivation },
			},
			{
				name: "settings-list-wheel-hover-and-scrolled-rows",
				dimensions: { columns: 80, rows: 6 },
				inputs: { itemCount: 12, maxVisible: 5, selectedItem: "item-5", wheelDelta: 1, rows: [0, 4] },
				expected: { rows: settingsScrolledObservation },
			},
			{
				name: "settings-list-search-row-forwards-and-separator-is-inert",
				dimensions: { columns: 80, rows: 6 },
				inputs: { itemCount: 4, maxVisible: 5, typedQuery: "it", edited: "X", searchClick: { x: 3, y: 0 } },
				expected: { search: settingsSearch },
			},
			{
				name: "alternate-screen-nested-input-click-keeps-host-focus",
				dimensions: { columns: 20, rows: 4 },
				inputs: { overlayWidth: 20, overlayHeight: 4, value: "hi", columns: [5, 3], typed: "!" },
				expected: { overlays: overlayObservation },
			},
			{
				name: "settings-submenu-click-closes-and-keeps-host-focus",
				dimensions: { columns: 30, rows: 6 },
				inputs: { submenuItems: ["first", "second"], settings: ["theme", "other"] },
				expected: { submenu: settingsSubmenu },
			},
			{
				name: "editor-alternate-screen-click-positions-cursor",
				dimensions: { columns: 20, rows: 6 },
				inputs: { text: "hello", typed: "X" },
				expected: { editor: editorAltScreen },
			},
			{
				name: "editor-click-bounds-wrapped-segments-and-wide-graphemes",
				dimensions: { columns: 20, rows: 24 },
				inputs: {
					typed: "X",
					cases: editorDirectCases.map(({ label, text, width, x, y, paddingX }) => ({
						label,
						text,
						width,
						x,
						y,
						paddingX: paddingX ?? 0,
					})),
				},
				expected: { cases: editorDirectObservation },
			},
			{
				name: "editor-drag-selects-and-copies-instead-of-moving-the-cursor",
				dimensions: { columns: 20, rows: 6 },
				inputs: { text: "hello world" },
				expected: { drag: editorDrag },
			},
		],
	};
}
