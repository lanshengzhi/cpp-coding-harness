import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureEditorOperations(piCheckout: string) {
 const packageRoot = path.join(piCheckout, "packages/tui");
 const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
 const { Editor } = await import(source("src/components/editor.ts"));
 const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
 const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
 const { defaultEditorTheme } = await import(source("test/test-themes.ts"));
 const { KeybindingsManager, TUI_KEYBINDINGS, setKeybindings } = await import(source("src/keybindings.ts"));
 const { setKittyProtocolActive } = await import(source("src/keys.ts"));
 setKittyProtocolActive(false);
 const sourceEndpoints = [
  "packages/tui/src/components/editor.ts:handleInput,setText,getText,getExpandedText,getCursor,addToHistory",
  "packages/tui/src/word-navigation.ts:findWordBackward,findWordForward",
  "packages/tui/src/undo-stack.ts:UndoStack.push,pop",
  "packages/tui/src/kill-ring.ts:KillRing.push,rotate",
  "packages/tui/src/keybindings.ts:KeybindingsManager.setUserBindings",
  "packages/tui/test/virtual-terminal.ts:VirtualTerminal",
 ];
 const makeEditor = (width = 40) => new Editor(new TuiMainScreen(new VirtualTerminal(width, 8)), defaultEditorTheme);
 const state = (editor: InstanceType<typeof Editor>) => {
  const lines = editor.getLines();
  const cursor = editor.getCursor();
  const line = lines[cursor.line] ?? "";
  return {
   text: editor.getText(), expanded: editor.getExpandedText(), lines,
   cursor: { line: cursor.line, utf16Column: cursor.col, before: line.slice(0, cursor.col), after: line.slice(cursor.col) },
  };
 };
 const scenarios: any[] = [];
 {
  const editor = makeEditor(); const changes: string[] = []; editor.onChange = (text: string) => changes.push(text);
  const keys = ["h", "é", "😀", " ", "界", "x"];
  for (const key of keys) editor.handleInput(key);
  const typed = state(editor);
  editor.handleInput("\x1b[45;5u"); const undoFirst = state(editor);
  editor.handleInput("\x1b[45;5u"); const undoSecond = state(editor);
  editor.handleInput("z"); const edited = state(editor);
  editor.handleInput("\x1b[45;5u"); const undoAfterEdit = state(editor);
  scenarios.push({ name: "coalesced-unicode-edit-after-undo", dimensions: { columns: 40, rows: 8 }, inputs: { keys, undo: "ctrl+-", edit: "z" }, expected: { typed, undoFirst, undoSecond, edited, undoAfterEdit, changes } });
 }
 {
  const editor = makeEditor(); editor.setText("éclair 世界,foo_bar 😀done");
  const initial = state(editor); editor.handleInput("\x01");
  const wordPositions = [state(editor)];
  for (let i = 0; i < 7; ++i) { editor.handleInput("\x1b[1;5C"); wordPositions.push(state(editor)); }
  editor.handleInput("\x05"); const lineEnd = state(editor);
  editor.handleInput("\x01");
  editor.handleInput("\x1d"); editor.handleInput("o"); const jumpForward = state(editor);
  editor.handleInput("\x1b\x1d"); editor.handleInput("é"); const jumpBackward = state(editor);
  scenarios.push({ name: "unicode-word-navigation-and-jump", dimensions: { columns: 40, rows: 8 }, inputs: { text: initial.text, home: "ctrl+a", wordRight: "ctrl+right", repetitions: 7, end: "ctrl+e", jumpForward: "ctrl+]", jumpBackward: "ctrl+alt+]", forwardTarget: "o", backwardTarget: "é" }, expected: { wordPositions, lineEnd, jumpForward, jumpBackward } });
 }
 {
  const editor = makeEditor(); const changes: string[] = []; editor.onChange = (text: string) => changes.push(text);
  editor.setText("FIRST"); editor.handleInput("\x17"); const firstKill = state(editor);
  editor.setText("SECOND"); editor.handleInput("\x17"); const secondKill = state(editor);
  editor.setText(""); editor.handleInput("\x19"); const yankLatest = state(editor);
  editor.handleInput("\x1by"); const yankOlder = state(editor);
  editor.handleInput("x"); const afterBreak = state(editor);
  editor.handleInput("\x1by"); const rejectedPop = state(editor);
  scenarios.push({ name: "kill-ring-accumulation-and-yank-rotation", dimensions: { columns: 40, rows: 8 }, inputs: { kills: ["FIRST", "SECOND"], killBackward: "ctrl+w", yank: "ctrl+y", yankPop: "alt+y", breaker: "x" }, expected: { firstKill, secondKill, yankLatest, yankOlder, afterBreak, rejectedPop, changes } });
 }
 {
  const editor = makeEditor(); const changes: string[] = []; editor.onChange = (text: string) => changes.push(text);
  const payload = "😀猫" + "x".repeat(1000); const submitted: string[] = []; editor.onSubmit = (text: string) => submitted.push(text);
  editor.setText("AB"); editor.handleInput("\x01"); editor.handleInput("\x1b[C");
  editor.handleInput("\x1b[200~" + payload + "\x1b[201~"); const pasted = state(editor);
  editor.handleInput("\x1b[D"); const left = state(editor);
  editor.handleInput("\x1b[C"); const right = state(editor);
  editor.handleInput("\x7f"); const deleted = state(editor);
  editor.handleInput("\x1b[45;5u"); const undo = state(editor);
  editor.handleInput("\r");
  scenarios.push({ name: "collapsed-paste-atomic-undo-submit", dimensions: { columns: 40, rows: 8 }, inputs: { initial: "AB", payload, operations: ["paste", "left", "right", "backspace", "undo", "submit"] }, expected: { pasted, left, right, deleted, undo, submitted, changes } });
 }
 {
  const editor = makeEditor(); const changes: string[] = []; editor.onChange = (text: string) => changes.push(text);
  editor.addToHistory("first"); editor.addToHistory("second"); editor.setText("draft😀");
  editor.handleInput("\x1b[D"); const draft = state(editor);
  editor.handleInput("\x1b[A"); const movedToStart = state(editor);
  editor.handleInput("\x1b[A"); const recalled = state(editor);
  editor.handleInput("\x1b[B"); const restored = state(editor);
  editor.handleInput("\x1b[45;5u"); const undo = state(editor);
  scenarios.push({ name: "history-draft-and-undo", dimensions: { columns: 40, rows: 8 }, inputs: { history: ["first", "second"], draft: "draft😀", left: "left", previous: "up", next: "down", undo: "ctrl+-" }, expected: { draft, movedToStart, recalled, restored, undo, changes } });
 }
 {
  const editor = makeEditor(); const manager = new KeybindingsManager(TUI_KEYBINDINGS, { "tui.editor.undo": "alt+u" }); setKeybindings(manager);
  const changes: string[] = []; editor.onChange = (text: string) => changes.push(text);
  editor.handleInput("a"); editor.handleInput("b"); const typed = state(editor);
  editor.handleInput("\x1b[45;5u"); const oldBinding = state(editor);
  editor.handleInput("\x1bu"); const rebound = state(editor);
  setKeybindings(new KeybindingsManager(TUI_KEYBINDINGS));
  scenarios.push({ name: "undo-shortcut-rebinding", dimensions: { columns: 40, rows: 8 }, inputs: { keys: ["a", "b"], oldUndo: "ctrl+-", reboundUndo: "alt+u" }, expected: { typed, oldBinding, rebound, changes } });
 }
 return { sourceEndpoints, scenarios };
}
