import { pathToFileURL } from "node:url";
import path from "node:path";

// Editor autocomplete trigger observations for route batch B7 (issue #1215):
// which inputs request suggestions (with which force flag and prefix text)
// through the frozen Editor's public input seam. Extracted from the historical
// capture-named-tui.mts generator (pi #947) so the audit lane can recapture the
// editor-autocomplete.json family at the frozen pi-v1.3.0 baseline; the
// historical generator file is manifest-pinned and stays untouched.
export async function captureEditorAutocompleteTriggers(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Editor } = await import(source("src/components/editor.ts"));
	const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	const { defaultEditorTheme } = await import(source("test/test-themes.ts"));
	const sourceEndpoints = [
		"packages/tui/src/components/editor.ts:Editor.insertCharacter,handleTabCompletion,setAutocompleteProvider",
		"packages/tui/src/autocomplete.ts:AutocompleteProvider.getSuggestions",
		"packages/tui/src/components/editor.ts:Editor",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
	];
	const triggerCases = [
		{ name: "at-after-ideographic-comma", input: "查看，@" },
		{ name: "at-after-ideographic-space", input: "　@" },
		{ name: "at-after-no-break-space", input: " @" },
		{ name: "at-after-space", input: " @" },
		{ name: "at-after-wrapper", input: "(@" },
		{ name: "at-after-cjk-letter", input: "查看@" },
		{ name: "at-after-ascii-letter", input: "user@" },
		{ name: "at-after-katakana-letter", input: "カ@" },
		{ name: "slash-command-name", input: "/set" },
		{ name: "slash-command-after-space", input: " /set" },
		{ name: "cjk-letter-inside-attachment", input: "@说" },
		{ name: "tab-after-ideographic-comma", input: "查看，\t" },
		{ name: "tab-after-cjk-letter", input: "查看\t" },
		{ name: "tab-inside-slash-command", input: "/set\t" },
	];
	const observation = [];
	for (const triggerCase of triggerCases) {
		const editor = new Editor(new TuiMainScreen(new VirtualTerminal(80, 24)), defaultEditorTheme);
		const requests: Array<{ force: boolean; text: string }> = [];
		editor.setAutocompleteProvider({
			getSuggestions: async (lines, _cursorLine, cursorCol, options) => {
				requests.push({ force: options?.force === true, text: (lines[0] || "").slice(0, cursorCol) });
				return null;
			},
			applyCompletion: (lines, cursorLine, cursorCol, item, prefix) => ({
				lines,
				cursorLine,
				cursorCol: cursorCol - prefix.length + item.value.length,
			}),
			shouldTriggerFileCompletion: () => true,
		});
		for (const character of triggerCase.input) editor.handleInput(character);
		await new Promise((resolve) => setTimeout(resolve, 60));
		await new Promise((resolve) => setImmediate(resolve));
		observation.push({
			name: triggerCase.name,
			input: triggerCase.input,
			text: editor.getText(),
			requests,
		});
	}
	const scenarios = [
		{
			name: "editor-completion-triggers",
			dimensions: { columns: 80, rows: 24 },
			inputs: triggerCases.map((triggerCase) => ({ name: triggerCase.name, input: triggerCase.input })),
			expected: { cases: observation },
		},
	];
	return { sourceEndpoints, scenarios };
}
