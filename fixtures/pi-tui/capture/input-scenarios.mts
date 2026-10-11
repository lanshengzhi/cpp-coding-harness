import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureInputScenarios(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Input } = await import(source("src/components/input.ts"));
	const { KeybindingsManager, TUI_KEYBINDINGS, setKeybindings } = await import(source("src/keybindings.ts"));
	const { setKittyProtocolActive } = await import(source("src/keys.ts"));
	const { visibleWidth } = await import(source("src/utils.ts"));

	const sourceEndpoints = [
		"packages/tui/src/components/input.ts:render,handleInput,handlePaste,getValue,onSubmit",
		"packages/tui/src/keybindings.ts:KeybindingsManager",
		"packages/tui/src/keys.ts:setKittyProtocolActive",
	];

	// Clean CURSOR_MARKER (\x1b_pi:c\x07) from rendered lines for toolkit text comparison
	const cleanMarker = (line: string) => line.replace(/\x1b_pi:c\x07/g, "");
	// Cursor columns are observed from the hardware-cursor marker position,
	// not hardcoded: pi emits CURSOR_MARKER immediately before the fake
	// reverse-video cursor cell (input.ts render), so its visible position
	// in the raw line is the rendered cursor column.
	const cursorColumnOf = (rawLine: string) => {
		const markerIndex = rawLine.indexOf("\x1b_pi:c\x07");
		if (markerIndex < 0) return 0;
		return visibleWidth(rawLine.slice(0, markerIndex));
	};

	// Scenario 1: placeholder reversal and custom styling
	setKittyProtocolActive(false);
	const input1a = new Input({ prompt: "> ", placeholder: "Search..." });
	input1a.focused = true;
	const raw1a = input1a.render(16)[0] ?? "";
	const line1a = cleanMarker(raw1a);

	const input1b = new Input({
		prompt: "Find: ",
		placeholder: "Filter items",
		placeholderStyle: (text: string) => `\x1b[2m${text}\x1b[22m`,
	});
	input1b.focused = true;
	const raw1b = input1b.render(20)[0] ?? "";
	const line1b = cleanMarker(raw1b);

	const input1c = new Input({ prompt: "> ", placeholder: "Search..." });
	input1c.focused = true;
	for (const ch of "abc") input1c.handleInput(ch);
	const raw1c = input1c.render(16)[0] ?? "";
	const line1c = cleanMarker(raw1c);

	// Scenario 2: raw LF submission & multiline paste
	setKittyProtocolActive(false);
	const km = new KeybindingsManager(TUI_KEYBINDINGS, { "tui.input.submit": ["ctrl+x"] });
	setKeybindings(km);

	const input2a = new Input();
	const submitted2a: string[] = [];
	input2a.onSubmit = (val: string) => submitted2a.push(val);
	for (const ch of "hello") input2a.handleInput(ch);
	input2a.handleInput("\r");
	const afterCrSubmitted = [...submitted2a];
	input2a.handleInput("\n");
	const afterLfSubmitted = [...submitted2a];

	// Kitty protocol shift+enter mapping
	setKittyProtocolActive(true);
	const input2b = new Input();
	const submitted2b: string[] = [];
	input2b.onSubmit = (val: string) => submitted2b.push(val);
	for (const ch of "kitty") input2b.handleInput(ch);
	input2b.handleInput("\n");
	const afterKittyLfSubmitted = [...submitted2b];

	// Multiline paste
	setKittyProtocolActive(false);
	setKeybindings(new KeybindingsManager(TUI_KEYBINDINGS, {}));
	const input2c = new Input();
	const submitted2c: string[] = [];
	input2c.onSubmit = (val: string) => submitted2c.push(val);
	input2c.handleInput("\x1b[200~first\r\nsecond\nthird\tval\x1b[201~");
	const pasteValue = input2c.getValue();

	// Scenario 3: narrow Unicode scroll and cursor
	const input3 = new Input({ prompt: "> " });
	input3.setValue("éclair 世界 foo");
	input3.focused = true;
	// Home
	input3.handleInput("\x01");
	const rawHome = input3.render(12)[0] ?? "";
	const stepHome = cleanMarker(rawHome);
	// Right 6
	for (let i = 0; i < 6; i++) input3.handleInput("\x1b[C");
	const rawRight6 = input3.render(12)[0] ?? "";
	const stepRight6 = cleanMarker(rawRight6);
	// End
	input3.handleInput("\x05");
	const rawEnd = input3.render(12)[0] ?? "";
	const stepEnd = cleanMarker(rawEnd);

	const scenarios = [
		{
			name: "input-placeholder-reversal-and-custom-styling",
			dimensions: { columns: 20, rows: 3 },
			inputs: {
				defaultPlaceholder: "Search...",
				styledPlaceholder: "Filter items",
				styledPrompt: "Find: ",
			},
			expected: {
				defaultEmpty: {
					line: line1a,
					visibleWidth: visibleWidth(line1a),
					cursorColumn: cursorColumnOf(raw1a),
				},
				styledEmpty: {
					line: line1b,
					visibleWidth: visibleWidth(line1b),
					cursorColumn: cursorColumnOf(raw1b),
				},
				populated: {
					line: line1c,
					visibleWidth: visibleWidth(line1c),
					cursorColumn: cursorColumnOf(raw1c),
				},
			},
		},
		{
			name: "input-raw-lf-submission-and-rebound-submit",
			dimensions: { columns: 20, rows: 2 },
			inputs: {
				reboundSubmitKey: "ctrl+x",
				pastedText: "first\r\nsecond\nthird\tval",
			},
			expected: {
				reboundCrSubmitted: afterCrSubmitted,
				reboundLfSubmitted: afterLfSubmitted,
				kittyLfSubmitted: afterKittyLfSubmitted,
				pastedValue: pasteValue,
				pastedSubmitted: submitted2c,
			},
		},
		{
			name: "input-narrow-unicode-scroll-and-cursor",
			dimensions: { columns: 12, rows: 1 },
			inputs: {
				text: "éclair 世界 foo",
				width: 12,
			},
			expected: {
				stepHome: {
					line: stepHome,
					visibleWidth: visibleWidth(stepHome),
					cursorColumn: cursorColumnOf(rawHome),
				},
				stepRight6: {
					line: stepRight6,
					visibleWidth: visibleWidth(stepRight6),
					cursorColumn: cursorColumnOf(rawRight6),
				},
				stepEnd: {
					line: stepEnd,
					visibleWidth: visibleWidth(stepEnd),
					cursorColumn: cursorColumnOf(rawEnd),
				},
			},
		},
	];

	return { sourceEndpoints, scenarios };
}
