import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureInputEditingScenarios(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Input } = await import(source("src/components/input.ts"));
	const { KeybindingsManager, TUI_KEYBINDINGS, setKeybindings } = await import(source("src/keybindings.ts"));
	const { setKittyProtocolActive } = await import(source("src/keys.ts"));

	setKittyProtocolActive(false);

	const sourceEndpoints = [
		"packages/tui/src/components/input.ts:handleInput,getValue,setValue,render,handleMouse,invalidate",
		"packages/tui/src/word-navigation.ts:findWordBackward,findWordForward",
		"packages/tui/src/undo-stack.ts:UndoStack.push,pop",
		"packages/tui/src/kill-ring.ts:KillRing.push,rotate,peek",
		"packages/tui/src/keybindings.ts:KeybindingsManager",
	];

	const state = (inp: any) => ({
		value: inp.getValue(),
		cursor: inp.cursor,
		lastAction: inp.lastAction,
		isInPaste: inp.isInPaste,
		undoLength: inp.undoStack.length,
		killRingLength: inp.killRing.length,
	});

	const scenarios: any[] = [];

	// Scenario 1: input-grapheme-deletion-and-boundaries
	// Exercises insertCharacter, handleBackspace, handleForwardDelete, cursor, value, getValue, lastAction
	{
		const inp = new Input();
		for (const ch of ["h", "é", "😀"]) inp.handleInput(ch);
		const typed = state(inp);
		inp.handleInput("\x7f"); // backspace -> deletes 😀
		const backspace = state(inp);
		inp.handleInput("\x1b[D"); // left -> cursor before é
		inp.handleInput("\x1b[3~"); // delete -> deletes é
		const forwardDelete = state(inp);
		inp.handleInput("\x1b[D"); // left -> cursor at 0
		inp.handleInput("\x7f"); // backspace at 0 (boundary no-op)
		const backspaceAtStart = state(inp);
		inp.handleInput("\x05"); // end -> cursor at 1
		inp.handleInput("\x1b[3~"); // delete at end (boundary no-op)
		const deleteAtEnd = state(inp);

		scenarios.push({
			name: "input-grapheme-deletion-and-boundaries",
			dimensions: { columns: 20, rows: 1 },
			inputs: {
				chars: ["h", "é", "😀"],
				backspaceKey: "backspace",
				deleteKey: "delete",
			},
			expected: {
				typed,
				backspace,
				forwardDelete,
				backspaceAtStart,
				deleteAtEnd,
			},
		});
	}

	// Scenario 2: input-line-kill-and-yank-rotation
	// Exercises deleteToLineStart, deleteToLineEnd, yank, yankPop, kill ring accumulation and rotation
	{
		const inp = new Input();
		for (const ch of "FIRST") inp.handleInput(ch);
		inp.handleInput("\x15"); // ctrl+u at end -> deletes "FIRST" to ring
		const firstKill = { ...state(inp), ring: [...(inp as any).killRing.ring] };

		inp.handleInput("x"); // breaks kill accumulation
		for (const ch of "SECOND") inp.handleInput(ch);
		inp.handleInput("\x15"); // ctrl+u at end -> deletes "xSECOND" to ring
		const secondKill = { ...state(inp), ring: [...(inp as any).killRing.ring] };

		inp.handleInput("tail");
		inp.handleInput("\x01"); // home
		for (const ch of "head ") inp.handleInput(ch); // value is "head tail", cursor at 5
		inp.handleInput("\x0b"); // ctrl+k deleteToLineEnd -> deletes "tail" to ring
		const thirdKill = { ...state(inp), ring: [...(inp as any).killRing.ring] };

		inp.handleInput("\x19"); // ctrl+y yank -> inserts most recent kill ("tail")
		const yankRecent = state(inp);
		inp.handleInput("\x1by"); // alt+y yankPop -> rotates, replaces with "xSECOND"
		const yankPop1 = state(inp);
		inp.handleInput("\x1by"); // alt+y yankPop -> rotates, replaces with "FIRST"
		const yankPop2 = state(inp);

		scenarios.push({
			name: "input-line-kill-and-yank-rotation",
			dimensions: { columns: 30, rows: 1 },
			inputs: {
				first: "FIRST",
				second: "SECOND",
				deleteToLineStartKey: "ctrl+u",
				deleteToLineEndKey: "ctrl+k",
				yankKey: "ctrl+y",
				yankPopKey: "alt+y",
			},
			expected: {
				firstKill,
				secondKill,
				thirdKill,
				yankRecent,
				yankPop1,
				yankPop2,
			},
		});
	}

	// Scenario 3: input-word-navigation-and-deletion
	// Exercises moveWordBackwards, moveWordForwards, deleteWordBackwards, deleteWordForward
	{
		const inp = new Input();
		inp.setValue("éclair 世界,foo_bar 😀done");
		inp.handleInput("\x05"); // end
		const stepsBack: any[] = [{ value: inp.getValue(), cursor: (inp as any).cursor }];
		for (let i = 0; i < 6; i++) {
			inp.handleInput("\x1bb"); // alt+b
			stepsBack.push({ value: inp.getValue(), cursor: (inp as any).cursor });
		}

		const stepsFwd: any[] = [{ value: inp.getValue(), cursor: (inp as any).cursor }];
		for (let i = 0; i < 6; i++) {
			inp.handleInput("\x1bf"); // alt+f
			stepsFwd.push({ value: inp.getValue(), cursor: (inp as any).cursor });
		}

		inp.handleInput("\x05"); // end
		inp.handleInput("\x17"); // ctrl+w deleteWordBackward -> deletes "done"
		const deleteWordBack = {
			value: inp.getValue(),
			cursor: (inp as any).cursor,
			ring: [...(inp as any).killRing.ring],
		};

		inp.handleInput("\x01"); // home
		inp.handleInput("x"); // type to break kill accumulation
		inp.handleInput("\x01"); // home
		inp.handleInput("\x1bd"); // alt+d deleteWordForward -> deletes "xéclair"
		const deleteWordFwd = {
			value: inp.getValue(),
			cursor: (inp as any).cursor,
			ring: [...(inp as any).killRing.ring],
		};

		scenarios.push({
			name: "input-word-navigation-and-deletion",
			dimensions: { columns: 35, rows: 1 },
			inputs: {
				initial: "éclair 世界,foo_bar 😀done",
				moveWordBackwardKey: "alt+b",
				moveWordForwardKey: "alt+f",
				deleteWordBackwardKey: "ctrl+w",
				deleteWordForwardKey: "alt+d",
			},
			expected: {
				stepsBack,
				stepsFwd,
				deleteWordBack,
				deleteWordFwd,
			},
		});
	}

	// Scenario 4: input-undo-coalescing-and-snapshots
	// Exercises pushUndo, undo, lastAction, insertCharacter
	{
		const inp = new Input();
		for (const ch of "hello") inp.handleInput(ch);
		const typedHello = state(inp);
		inp.handleInput(" ");
		const typedSpace = state(inp);
		for (const ch of "world") inp.handleInput(ch);
		const typedWorld = state(inp);

		inp.handleInput("\x1b[45;5u"); // undo world
		const undo1 = state(inp);
		inp.handleInput("\x1b[45;5u"); // undo space
		const undo2 = state(inp);

		for (const ch of "fresh") inp.handleInput(ch);
		const typedFresh = state(inp);
		inp.handleInput("\x1b[45;5u"); // undo fresh
		const undo3 = state(inp);

		scenarios.push({
			name: "input-undo-coalescing-and-snapshots",
			dimensions: { columns: 25, rows: 1 },
			inputs: {
				words: ["hello", " ", "world"],
				undoKey: "ctrl+-",
				edit: "fresh",
			},
			expected: {
				typedHello,
				typedSpace,
				typedWorld,
				undo1,
				undo2,
				typedFresh,
				undo3,
			},
		});
	}

	// Scenario 5: input-escape-cancel-and-invalidate
	// Exercises onEscape, invalidate, handleInput
	{
		const inp = new Input();
		let escapeCount = 0;
		inp.onEscape = () => {
			escapeCount++;
		};

		for (const ch of "active") inp.handleInput(ch);
		const beforeEscape = state(inp);

		inp.handleInput("\x1b"); // escape
		const afterEscape = { ...state(inp), escapeCount };

		inp.handleInput("\x03"); // ctrl+c
		const afterCtrlC = { ...state(inp), escapeCount };

		inp.invalidate();
		const afterInvalidate = { ...state(inp), escapeCount };

		scenarios.push({
			name: "input-escape-cancel-and-invalidate",
			dimensions: { columns: 20, rows: 1 },
			inputs: {
				initial: "active",
				escapeKey: "escape",
				ctrlCKey: "ctrl+c",
			},
			expected: {
				beforeEscape,
				afterEscape,
				afterCtrlC,
				afterInvalidate,
			},
		});
	}

	// Scenario 6: input-paste-buffering-and-normalization
	// Exercises handlePaste, isInPaste, onSubmit (paste buffering, newline stripping, tabs to spaces, atomic undo)
	{
		const inp = new Input();
		let submitted: string[] = [];
		inp.onSubmit = (val: string) => {
			submitted.push(val);
		};

		for (const ch of "prefix_") inp.handleInput(ch);
		const prePaste = state(inp);

		// Chunk 1 of bracketed paste: mid-paste buffering
		inp.handleInput("\x1b[200~part1\r\n");
		const midPaste = { ...state(inp), submitted: [...submitted] };

		// Chunk 2: completion of paste
		inp.handleInput("part2\nthird\tcol\x1b[201~");
		const postPaste = { ...state(inp), submitted: [...submitted] };

		// Undo once restores pre-paste state atomically
		inp.handleInput("\x1b[45;5u"); // undo
		const afterUndo = { ...state(inp), submitted: [...submitted] };

		scenarios.push({
			name: "input-paste-buffering-and-normalization",
			dimensions: { columns: 40, rows: 1 },
			inputs: {
				prefix: "prefix_",
				chunk1: "\x1b[200~part1\r\n",
				chunk2: "part2\nthird\tcol\x1b[201~",
				undoKey: "ctrl+-",
			},
			expected: {
				prePaste,
				midPaste,
				postPaste,
				afterUndo,
			},
		});
	}

	// Scenario 7: input-rebound-submit-and-fallthrough
	// Exercises onSubmit, handleInput, keybindings rebound submit vs raw LF
	{
		const km = new KeybindingsManager(TUI_KEYBINDINGS, { "tui.input.submit": ["ctrl+x"] });
		setKeybindings(km);

		const inp = new Input();
		let submitted: string[] = [];
		inp.onSubmit = (val: string) => {
			submitted.push(val);
		};

		for (const ch of "command") inp.handleInput(ch);
		inp.handleInput("\r"); // enter: does not submit and does not insert
		const afterEnter = { val: inp.getValue(), submitted: [...submitted] };

		inp.handleInput("\x18"); // ctrl+x: submit
		const afterCtrlX = { val: inp.getValue(), submitted: [...submitted] };

		inp.handleInput("\n"); // raw LF: submits per pi specification
		const afterLf = { val: inp.getValue(), submitted: [...submitted] };

		setKeybindings(new KeybindingsManager(TUI_KEYBINDINGS));

		scenarios.push({
			name: "input-rebound-submit-and-fallthrough",
			dimensions: { columns: 20, rows: 1 },
			inputs: {
				command: "command",
				reboundSubmit: "ctrl+x",
			},
			expected: {
				afterEnter,
				afterCtrlX,
				afterLf,
			},
		});
	}

	return { sourceEndpoints, scenarios };
}
