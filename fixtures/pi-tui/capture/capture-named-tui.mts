#!/usr/bin/env tsx
/** Independent named TUI capture (#947). Never imports Pike or historical fixtures.
 * PI_CHECKOUT must be a clean checkout at the registry's full revision.
 * Later tickets extend the cases below and add artifacts with the same envelope.
 */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const root = path.resolve(path.dirname(scriptPath), "..");
const registryPath = path.join(root, "baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/tui-baselines/1") throw new Error("invalid TUI baseline registry");
const baseline = process.env.PI_BASELINE ?? registry.default;
const selected = registry.baselines[baseline];
if (!selected) throw new Error(`unknown TUI baseline: ${baseline}`);
if (!selected.bundle.startsWith("bundles/") || selected.bundle.split("/").includes("..")) {
	throw new Error("capture requires a distinct named bundle");
}
const pi = process.env.PI_CHECKOUT ?? path.resolve(root, "../../../pi");
const revision = execFileSync("git", ["-C", pi, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (revision !== selected.revision) throw new Error(`pi checkout must be at ${selected.revision}, found ${revision}`);
if (execFileSync("git", ["-C", pi, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("TUI capture requires Linux x86-64/glibc");
}
// Pin capability inputs before importing pi; HOME is unused by these cases.
const disabled = ["TERM_PROGRAM", "TERM_PROGRAM_VERSION", "KITTY_WINDOW_ID", "WEZTERM_PANE", "TMUX", "TERMUX_VERSION", "PI_TUI_WRITE_LOG", "PI_HARDWARE_CURSOR", "COLORTERM"];
for (const variable of disabled) delete process.env[variable];
process.env.TERM = "xterm-256color";
const environment = {
	platform: process.platform, arch: process.arch, libc: "glibc", glibcVersion: glibc,
	node: process.version, TERM: process.env.TERM, disabledVariables: disabled,
};
const requirePi = createRequire(path.join(pi, "packages/tui/package.json"));
const xtermVersion = requirePi("@xterm/headless/package.json").version;
const packageMetadata = JSON.parse(readFileSync(path.join(pi, "packages/tui/package.json"), "utf8"));
if (xtermVersion !== packageMetadata.devDependencies["@xterm/headless"]) {
	throw new Error("installed xterm version differs from frozen pi package declaration");
}
Object.assign(environment, { dependencies: { "@xterm/headless": xtermVersion, tsx: requirePi("tsx/package.json").version } });
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const src = (relative: string) => pathToFileURL(path.join(pi, "packages/tui", relative)).href;
const { parseKey, isKeyRepeat, isKeyRelease, setKittyProtocolActive } = await import(src("src/keys.ts"));
const { visibleWidth, wrapTextWithAnsi, sliceByColumn, truncateToWidth, getOsc8LinkAtColumn } =
	await import(src("src/utils.ts"));
const { fuzzyMatch, fuzzyFilter } = await import(src("src/fuzzy.ts"));
const { Input } = await import(src("src/components/input.ts"));
const { Text } = await import(src("src/components/text.ts"));
const { TuiMainScreen } = await import(src("src/tui-main-screen.ts"));
const { CURSOR_MARKER } = await import(src("src/tui.ts"));
const { VirtualTerminal } = await import(src("test/virtual-terminal.ts"));
const { renderLatex } = await import(src("src/latex.ts"));
const { CombinedAutocompleteProvider } = await import(src("src/autocomplete.ts"));
const { Editor } = await import(src("src/components/editor.ts"));
const { defaultEditorTheme } = await import(pathToFileURL(path.join(pi, "packages/tui/test/test-themes.ts")).href);
const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

function envelope(sourceEndpoints: string[], scenarios: unknown[]) {
	return { baseline, revision, capturedAt, environment, sourceEndpoints, scenarios };
}

// Input parsing and actual Input editing/submission are independent observations.
setKittyProtocolActive(false);
const sequences = ["\x1b[D", "\x1b[97;5:2u", "\x1b[97;5:3u"];
const edits = ["a", "b", "\x1b[D", "c", "\r"];
const input = new Input();
const submitted: string[] = [];
input.onSubmit = (value: string) => submitted.push(value);
const values = edits.map((data) => { input.handleInput(data); return input.getValue(); });
const inputArtifact = envelope([
	"packages/tui/src/keys.ts:parseKey,isKeyRepeat,isKeyRelease",
	"packages/tui/src/components/input.ts:handleInput,getValue,onSubmit",
], [{
	name: "keys-and-input-editing", dimensions: { columns: 16, rows: 6 },
	inputs: { kittyActive: false, sequences, edits },
	expected: {
		keys: sequences.map((data) => ({
			id: parseKey(data) ?? null,
			eventType: isKeyRelease(data) ? "release" : isKeyRepeat(data) ? "repeat" : "press",
		})), values, submitted,
	},
}]);

// Include child mutation so a render label or one cached frame cannot stand in for output.
const textInputs = { text: "\x1b[31mhello\x1b[0m world", changedText: "\x1b[32mnext\x1b[0m text", paddingX: 1, paddingY: 0 };
const text = new Text(textInputs.text, textInputs.paddingX, textInputs.paddingY);
const first = [...text.render(12)];
text.setText(textInputs.changedText);
const componentArtifact = envelope(["packages/tui/src/components/text.ts:render,setText"], [{
	name: "styled-text-mutation", dimensions: { columns: 12, rows: 6 },
	inputs: textInputs, expected: { first, changed: [...text.render(12)] },
}]);

// Use the frozen VirtualTerminal as the renderer adapter. Feed its exact output to a
// second public xterm instance to inspect full cell attributes without private pi hooks.
class CaptureTerminal extends VirtualTerminal {
	readonly emulator: any;
	cursorVisible = true;
	constructor(columns: number, rows: number) {
		super(columns, rows);
		this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
	}
	override write(data: string) {
		if ([...data.matchAll(/\x1b\]8;[^;]*;([^\x07\x1b]*)/g)].some((match) => match[1])) throw new Error("OSC 8 cell capture needs link-aware evidence before adding linked scenarios");
		super.write(data); this.emulator.write(data);
	}
	override hideCursor() { super.hideCursor(); this.cursorVisible = false; }
	override showCursor() { super.showCursor(); this.cursorVisible = true; }
	async snapshot() {
		await this.flush();
		await new Promise<void>((resolve) => this.emulator.write("", resolve));
		const buffer = this.emulator.buffer.active;
		const screen = this.getViewport();
		const observedScreen = Array.from({ length: this.rows }, (_, row) => buffer.getLine(buffer.viewportY + row).translateToString(true));
		const observedCursor = this.getCursorPosition();
		if (JSON.stringify(screen) !== JSON.stringify(observedScreen) || observedCursor.x !== buffer.cursorX || observedCursor.y !== buffer.cursorY) {
			throw new Error("cell observer diverged from frozen VirtualTerminal; extend the public terminal adapter before recording this scenario");
		}
		const cells = Array.from({ length: this.rows }, (_, row) => Array.from({ length: this.columns }, (_, col) => {
			const cell = buffer.getLine(buffer.viewportY + row).getCell(col);
			// Palette indices map to canonical SGR color values; default colors remain empty.
			const color = (foreground: boolean) => {
				const mode = foreground ? cell.isFgPalette() : cell.isBgPalette();
				const rgb = foreground ? cell.isFgRGB() : cell.isBgRGB();
				if (rgb) {
					const rgbValue = foreground ? cell.getFgColor() : cell.getBgColor();
					return `${foreground ? 38 : 48};2;${rgbValue >> 16};${(rgbValue >> 8) & 255};${rgbValue & 255}`;
				}
				if (!mode) return "";
				const value = foreground ? cell.getFgColor() : cell.getBgColor();
				return value < 8 ? String((foreground ? 30 : 40) + value) : `${foreground ? 38 : 48};5;${value}`;
			};
			if (cell.isOverline()) throw new Error("overline cells need explicit C++ representation evidence before capture");
			return {
				grapheme: cell.getChars(), continuation: cell.getWidth() === 0,
				style: {
					bold: !!cell.isBold(), dim: !!cell.isDim(), italic: !!cell.isItalic(),
					underline: !!cell.isUnderline(), blink: !!cell.isBlink(), inverse: !!cell.isInverse(),
					hidden: !!cell.isInvisible(), strikethrough: !!cell.isStrikethrough(),
					fg_color: color(true), bg_color: color(false), hyperlink: "", hyperlink_params: "",
				},
			};
		}));
		return { screen, cells, cursor: { column: buffer.cursorX, row: buffer.cursorY }, cursorVisible: this.cursorVisible, scrollback: this.getScrollBuffer().slice(0, buffer.baseY) };
	}
}
const screenInputs = {
	frames: [["\x1b[1;31mred\x1b[0m", "tail"], ["\x1b[4;32mgo\x1b[0m", "tail"]],
	cursor: { column: 2, row: 1 }, showHardwareCursor: false,
};
const terminal = new CaptureTerminal(16, 6);
const renderer = new TuiMainScreen(terminal, screenInputs.showHardwareCursor);
let lines = screenInputs.frames[0];
const child = {
	focused: false,
	render: () => lines.map((line, row) => row === screenInputs.cursor.row
		? line.slice(0, screenInputs.cursor.column) + CURSOR_MARKER + line.slice(screenInputs.cursor.column)
		: line),
	invalidate: () => {},
};
renderer.addChild(child);
renderer.start();
renderer.setFocus(child);
renderer.renderNow();
const initial = await terminal.snapshot();
lines = screenInputs.frames[1];
renderer.renderNow();
const changed = await terminal.snapshot();
renderer.stop();
terminal.emulator.dispose();
const screenArtifact = envelope([
	"packages/tui/src/tui-main-screen.ts:TuiMainScreen.renderNow",
	"packages/tui/src/tui.ts:setFocus,CURSOR_MARKER",
	"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
	"packages/tui/package.json:@xterm/headless",
], [{ name: "styled-frame-and-focused-cursor", dimensions: { columns: 16, rows: 6 }, inputs: screenInputs, expected: { initial, changed } }]);


// Capability accounting observation: freeze the public index export set and ledger classifications.
// The committed docs/research ledger is hashed as the C++/Pike accounting artifact; export names are
// observed directly from the frozen index so a stale/partial ledger cannot pass silently.
const indexSource = readFileSync(path.join(pi, "packages/tui/src/index.ts"), "utf8");
const indexExports = [...indexSource.matchAll(/export\s+(?:type\s+)?\{([^}]+)\}\s+from\s+"([^"]+)"/gs)].flatMap((match) =>
	match[1].split(",").map((raw) => {
		let name = raw.trim().replace(/^type\s+/, "");
		if (!name) return "";
		if (name.includes(" as ")) name = name.split(" as ").at(-1)?.trim() ?? "";
		return name;
	}).filter(Boolean),
);
const ledgerPath = path.resolve(root, "../../docs/research/tui-v1.0.4-capability-ledger.json");
const ledger = JSON.parse(readFileSync(ledgerPath, "utf8"));
if (ledger.baseline !== baseline || ledger.revision !== revision) {
	throw new Error("capability ledger revision/baseline mismatch");
}
const exportRows = ledger.rows.filter((row: any) => row.member == null && !["product-entry", "diagnostic-capability", "internal-helper"].includes(row.kind));
const ledgerNames = exportRows.map((row: any) => row.export).sort();
const observedNames = [...indexExports].sort();
if (JSON.stringify(ledgerNames) !== JSON.stringify(observedNames)) {
	throw new Error("capability ledger export set diverges from frozen index");
}
if (exportRows.some((row: any) => row.classification === "included" && /Deferred/.test(JSON.stringify(row)))) {
	throw new Error("included capability rows must not be Deferred");
}
const capabilityArtifact = envelope([
	"packages/tui/src/index.ts:export-surface",
], [{
	name: "index-export-accounting",
	dimensions: { columns: 1, rows: 1 },
	inputs: {
		indexSha256: sha256(Buffer.from(indexSource)),
		exportCount: observedNames.length,
		ledgerPath: "docs/research/tui-v1.0.4-capability-ledger.json",
		ledgerSha256: sha256(readFileSync(ledgerPath)),
	},
	expected: {
		exports: observedNames,
		classifications: Object.fromEntries(exportRows.map((row: any) => [row.export, row.classification])),
		crossReferences: ["831", "811", "809", "749", "830"],
	},
}]);

// Public visible-width and dependent Text/Input placement (#956).
const narrowText = new Text("काक्ष", 0, 0);
const narrowInput = new Input();
narrowInput.setValue("काक्ष");
// Public Kitty insertion text and shortcut identity (#950). The keyboard
// protocol keeps the terminal's own character while the base-layout key still
// answers shortcut matching; both are observed for the same event and through
// the real Input insertion path. Never regenerate these expectations from Pike.
const kittySequences = [
	"\x1b[1092::97u",        // Cyrillic ef on the physical 'a' key
	"\x1b[1060:1040:97;2u",  // shifted Cyrillic es, base-layout 'a'
	"\x1b[49:33;2u",         // shift-modified '1' inserts '!'
	"\x1b[128512u",          // supplementary plane character
	"\x1b[1092::97;2u",      // repeat
	"\x1b[1092::97;3u",      // release
	"\x1b[1092::97;5u",      // ctrl combination
	"\x1b[1092::97;9u",      // super combination
];
setKittyProtocolActive(true);
const kittyInput = new Input();
const kittyInputValues = kittySequences.map((data) => { kittyInput.handleInput(data); return kittyInput.getValue(); });
const kittyArtifact = envelope([
	"packages/tui/src/keys.ts:parseKey,decodeKittyPrintable",
	"packages/tui/src/components/input.ts:handleInput,getValue",
], [{
	name: "kitty-insertion-text-and-shortcut-identity",
	dimensions: { columns: 24, rows: 6 },
	inputs: { kittyActive: true, sequences: kittySequences },
	expected: {
		// The parsed key's own field names are recorded too, so a Pike capture
		// cannot silently read a differently named printable-text field.
		keyShape: Object.keys(parseKey("\x1b[1092::97u") ?? {}).sort(),
		keys: kittySequences.map((data) => {
			const key = parseKey(data);
			return {
				id: key?.id ?? null,
				text: key?.text ?? null,
				eventType: isKeyRelease(data) ? "release" : isKeyRepeat(data) ? "repeat" : "press",
			};
		}),
		inputValues: kittyInputValues,
	},
}]);
setKittyProtocolActive(false);

const utilsWidthArtifact = envelope([
	"packages/tui/src/utils.ts:visibleWidth",
	"packages/tui/src/components/text.ts:render",
	"packages/tui/src/components/input.ts:render,setValue",
], [{
	name: "visible-width-and-narrow-placement",
	dimensions: { columns: 6, rows: 4 },
	inputs: {
		visibleWidth: [
			{ name: "ka", input: "का", output: visibleWidth("का") },
			{ name: "ksha", input: "क्ष", output: visibleWidth("क्ष") },
			{ name: "ka-newline-ksha", input: "का\nक्ष", output: visibleWidth("का\nक्ष") },
			{ name: "hello-newline-world", input: "hello\nworld", output: visibleWidth("hello\nworld") },
			{
				name: "ansi-newline-ansi",
				input: "\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m",
				output: visibleWidth("\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m"),
			},
			{ name: "emoji-combining-flag", input: "a😀b", output: visibleWidth("a😀b") },
		],
		narrowText: { text: "काक्ष", width: 2 },
		narrowInput: { value: "काक्ष", width: 6 },
	},
	expected: {
		visibleWidth: [
			{ name: "ka", input: "का", output: visibleWidth("का") },
			{ name: "ksha", input: "क्ष", output: visibleWidth("क्ष") },
			{ name: "ka-newline-ksha", input: "का\nक्ष", output: visibleWidth("का\nक्ष") },
			{ name: "hello-newline-world", input: "hello\nworld", output: visibleWidth("hello\nworld") },
			{
				name: "ansi-newline-ansi",
				input: "\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m",
				output: visibleWidth("\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m"),
			},
			{ name: "emoji-combining-flag", input: "a😀b", output: visibleWidth("a😀b") },
		],
		narrowText: { width2: [...narrowText.render(2)], width4: [...narrowText.render(4)] },
		narrowInput: { width6: [...narrowInput.render(6)] },
	},
}]);

// Public ANSI/OSC 8 boundary order for wrap, slice, truncate and link lookup (#957).
const belLink = (url: string) => `\x1b]8;;${url}\x07`;
const stLink = (url: string) => `\x1b]8;;${url}\x1b\\`;
const wrapCases = [
	{ name: "staged-control-after-content", input: "中文\x1b[31mABCDEFGHIJ", width: 4 },
	{ name: "staged-underline-after-content", input: "中文\x1b[4mABCDEFGH", width: 4 },
	{ name: "staged-hyperlink-after-content", input: `中文${belLink("u")}ABCDEFGH`, width: 4 },
	{ name: "st-terminated-link-across-break", input: `${stLink("u")}hello world${stLink("")}`, width: 7 },
	{ name: "link-cjk-styled-span", input: `ab ${belLink("u")}\x1b[31m中文\x1b[0m${belLink("")} cd`, width: 4 },
	{ name: "control-before-logical-newline", input: "abc\x1b[31m\ndef", width: 20 },
];
const truncateCases = [
	{ name: "fits-open-underline-pads-inside", input: "\x1b[4mabc", width: 8, ellipsis: "", pad: true },
	{ name: "pending-control-styles-dropped-text", input: "\x1b[4ma\x1b[31mbcdef", width: 4, ellipsis: "...", pad: false },
	{ name: "pending-link-styles-dropped-text", input: `\x1b[4ma${belLink("u")}bcdef`, width: 4, ellipsis: "...", pad: false },
	{ name: "st-terminated-link-closes-before-reset", input: `${stLink("u")}abcdefgh`, width: 4, ellipsis: "...", pad: false },
	{ name: "ellipsis-does-not-fit", input: "abcdef", width: 2, ellipsis: "中", pad: false },
	{ name: "ellipsis-clipped-to-fit", input: "abcdef", width: 2, ellipsis: "abc", pad: false },
	{ name: "ellipsis-clipped-away", input: "abcdef", width: 1, ellipsis: "中中", pad: false },
];
const sliceCases = [
	{ name: "slice-start-code-order", input: "\x1b[32mfoo\x1b[39m bar", start: 3, length: 4, strict: true },
	{ name: "slice-inside-open-link", input: `${belLink("u")}abcdefgh${belLink("")}`, start: 2, length: 3, strict: false },
	{ name: "slice-spanning-cjk-link", input: `${belLink("u")}a你bcd${belLink("")}`, start: 1, length: 3, strict: false },
];
const linkColumns = [
	{ name: "cjk-link-first-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 2 },
	{ name: "cjk-link-second-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 3 },
	{ name: "styled-link-cell", input: `\x1b[31m${belLink("https://x")}ab${belLink("")}\x1b[0m`, column: 0 },
	{ name: "cell-after-link-close", input: `${belLink("https://x")}ab${belLink("")}`, column: 3 },
];
const utilsAnsiArtifact = envelope([
	"packages/tui/src/utils.ts:wrapTextWithAnsi",
	"packages/tui/src/utils.ts:sliceByColumn",
	"packages/tui/src/utils.ts:truncateToWidth",
	"packages/tui/src/utils.ts:getOsc8LinkAtColumn",
], [{
	name: "ansi-boundary-order-and-hyperlinks",
	dimensions: { columns: 20, rows: 4 },
	inputs: { wrap: wrapCases, truncate: truncateCases, slice: sliceCases, linkColumns },
	expected: {
		wrap: wrapCases.map((entry) => ({ ...entry, output: wrapTextWithAnsi(entry.input, entry.width) })),
		truncate: truncateCases.map((entry) => ({
			...entry, output: truncateToWidth(entry.input, entry.width, entry.ellipsis, entry.pad),
		})),
		slice: sliceCases.map((entry) => ({
			...entry, output: sliceByColumn(entry.input, entry.start, entry.length, entry.strict),
		})),
		linkColumns: linkColumns.map((entry) => ({
			...entry, link: getOsc8LinkAtColumn(entry.input, entry.column) ?? null,
		})),
	},
}]);

// Fuzzy scoring, filtering and the lowercasing they observe (#958). Scored
// positions are pi's UTF-16 indices, so the row records both index spaces:
// byte-indexed scoring cannot reproduce these scores.
const fuzzyMatchRows = [
	{ name: "empty-query", query: "", text: "anything" },
	{ name: "ascii-run", query: "abc", text: "xxabcxx" },
	{ name: "consecutive-run", query: "abc", text: "aabbcc" },
	{ name: "nonmatch-order", query: "abc", text: "ac" },
	{ name: "sharp-s-query", query: "ss", text: "\u00df" },
	{ name: "sharp-s-text", query: "\u00df", text: "ss" },
	{ name: "accented-prefix", query: "a", text: "\u00e9a" },
	{ name: "ascii-prefix", query: "a", text: "xxa" },
	{ name: "supplementary-prefix", query: "a", text: "\u{1f600}a" },
	{ name: "dotted-capital-prefix", query: "a", text: "\u0130a" },
	{ name: "sharp-s-prefix", query: "a", text: "\u00dfa" },
	{ name: "combining-prefix", query: "a", text: "e\u0301a" },
	{ name: "accented-pair", query: "ab", text: "\u00e9ab" },
	{ name: "no-break-space-boundary", query: "b", text: "a\u00a0b" },
	{ name: "final-sigma-terminal", query: "\u03c3", text: "\u0391\u03a3" },
	{ name: "final-sigma-initial", query: "\u03c3", text: "\u03a3\u0391" },
	{ name: "sigma-alone", query: "\u03c3", text: "\u03a3" },
	{ name: "non-ascii-uppercase", query: "a", text: "\u00c9A" },
];
const fuzzyFilterRows = [
	{
		name: "utf16-rank-with-ties",
		items: ["xxa", "\u00e9a", "\u{1f600}a", "a", "aa"],
		query: "a",
	},
	{
		name: "non-ascii-membership",
		items: ["xxa", "\u00e9a", "\u{1f600}a", "SS", "\u00df"],
		query: "a",
	},
	{
		name: "final-sigma-filter",
		items: ["\u0391\u03a3", "\u03a3\u0391"],
		query: "\u03c3",
	},
	{ name: "no-break-space-token", items: ["a/b/c"], query: "a\u00a0c" },
	{ name: "no-break-space-trim", items: ["alpha", "beta"], query: "\u00a0alpha" },
];
const fuzzyArtifact = envelope(["packages/tui/src/fuzzy.ts:fuzzyMatch,fuzzyFilter"], [{
	name: "fuzzy-scoring-and-ranking",
	dimensions: { columns: 24, rows: 6 },
	inputs: {
		match: fuzzyMatchRows.map((row) => ({
			name: row.name,
			query: row.query,
			text: row.text,
			utf16Length: row.text.length,
			utf8Length: Buffer.byteLength(row.text),
		})),
		filter: fuzzyFilterRows.map((row) => ({
			name: row.name, items: row.items, query: row.query,
		})),
	},
	expected: {
		match: fuzzyMatchRows.map((row) => ({ name: row.name, ...fuzzyMatch(row.query, row.text) })),
		filter: fuzzyFilterRows.map((row) => ({
			name: row.name,
			output: fuzzyFilter(row.items, row.query, (item) => item),
		})),
	},
}]);

// Inline math grammar and the frozen failure value (#973).
const latexInline = [
  ["symbols-greek", "\\alpha\\beta\\gamma"],
  ["symbols-caps", "\\Gamma\\sum\\Omega"],
  ["symbols-relations", "a \\times b"],
  ["symbols-cdot", "a \\cdot b"],
  ["symbols-le", "a \\leq b"],
  ["symbols-infinity", "\\infty"],
  ["frac-half", "\\frac{1}{2}"],
  ["frac-letters", "\\frac{a}{b}"],
  ["frac-expression", "\\frac{a+b}{c-d}"],
  ["frac-scripts", "\\frac{x^{2}}{2}"],
  ["frac-nested", "\\frac{\\frac{1}{2}}{3}"],
  ["frac-bare-arguments", "\\frac1{2}"],
  ["group-nested", "{{x}^{2}}^{3}"],
  ["group-nested-command", "{\\alpha{\\beta\\gamma}}"],
  ["root-square", "\\sqrt{x+1}"],
  ["root-square-word", "\\sqrt{2}"],
  ["root-cube", "\\sqrt[3]{8}"],
  ["root-fourth", "\\sqrt[4]{16}"],
  ["root-degree", "\\sqrt[n]{x}"],
  ["root-nested", "\\sqrt{\\sqrt{2}}"],
  ["accent-hat", "\\hat{a}"],
  ["accent-bar", "\\bar{b}"],
  ["accent-vec", "\\vec{v}"],
  ["accent-overline", "\\overline{a}"],
  ["accent-multi", "\\widehat{ab}"],
  ["script-sup", "x^{2}"],
  ["script-sub", "x_{n+1}"],
  ["script-order-sup-first", "x^{2}_{i+1}"],
  ["script-order-sub-first", "x_{3}^{2}"],
  ["script-normalized", "x^{a = b}"],
  ["script-letter-run", "x^{ab}"],
  ["operator-limit", "\\lim_{n\\to\\infty}"],
  ["operator-sum", "\\sum_{i=1}^{n} i"],
  ["operator-named", "\\sin x"],
  ["operator-named-adjacent", "\\sin\\cos"],
  ["operator-limits-modifier", "\\lim\\limits_{n}x"],
  ["blackboard", "\\mathbb{R}"],
  ["negation", "\\not\\subset"],
  ["delimiters", "\\left( x \\right)"],
  ["plain-wrapper", "\\mathrm{d}x"],
  ["boxed", "\\boxed{x}"],
  ["equation", "\\begin{equation} a + b \\end{equation}"],
];
const latexFailing = [
  ["unknown-command", "\\bogus"],
  ["dangling-superscript", "x^"],
  ["unclosed-group", "x^{"],
  ["frac-one-argument", "\\frac{1}"],
  ["unbalanced-close", "a}b"],
  ["bare-backslash", "\\"],
  ["dangling-negation", "\\not"],
  ["unclosed-optional-root", "\\sqrt["],
  ["text-without-argument", "\\text"],
  ["unknown-environment", "\\begin{foo} x \\end{foo}"],
];
const latexDisplay = [
  ["frac-half", "\\frac{1}{2}"],
  ["sum-limits", "\\sum_{i=1}^{n}"],
  ["nested-frac", "\\frac{\\frac{1}{2}}{3}"],
];
// `undefined` is the frozen failure value; the artifact records it as JSON null.
const observeLatex = (source: string, options?: { display?: boolean }) => {
  const output = renderLatex(source, options);
  return output === undefined ? null : output;
};
const latexRows = (rows: [string, string][]) => rows.map(([name, source]) => ({ name, source }));
const latexArtifact = envelope(["packages/tui/src/latex.ts:renderLatex,RenderLatexOptions"], [
  {
    name: "inline-grammar", dimensions: { columns: 40, rows: 8 },
    inputs: { inline: latexRows(latexInline) },
    expected: { inline: latexRows(latexInline).map((row) => ({ ...row, output: observeLatex(row.source) })) },
  },
  {
    name: "inline-failure-value", dimensions: { columns: 40, rows: 8 },
    inputs: { failing: latexRows(latexFailing) },
    expected: { failing: latexRows(latexFailing).map((row) => ({ ...row, output: observeLatex(row.source) })) },
  },
  {
    // #974 owns the display layout; the observation is frozen here so that
    // ticket replays it rather than capturing a new baseline.
    name: "display-option", dimensions: { columns: 40, rows: 8 },
    inputs: { display: latexRows(latexDisplay) },
    expected: { display: latexRows(latexDisplay).map((row) => ({ ...row, output: observeLatex(row.source, { display: true }) })) },
  },
]);

// Completion context: which code points separate a completion token from the
// prose before it, and what prefix the provider completes and applies.
const completionRoot = mkdtempSync(path.join(tmpdir(), "pi-capture-completion-"));
const writeFixture = (relative: string, contents: string): void => {
	const target = path.join(completionRoot, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	writeFileSync(target, contents);
};

const completionCommands = [
	{
		name: "model",
		description: "Switch model",
		getArgumentCompletions: (argumentPrefix: string) =>
			argumentPrefix.startsWith("g") ? [{ value: "gpt", label: "gpt" }] : [],
	},
	{ name: "settings", description: "Open settings" },
];

const separatorProbes = [
	"\t", "\n", "\v", "\f", "\r", " ", "\u00a0", "\u00b7", "\u1680", "\u2000", "\u2001",
	"\u2002", "\u2003", "\u2004", "\u2005", "\u2006", "\u2007", "\u2008", "\u2009",
	"\u200a", "\u2028", "\u2029", "\u202f", "\u205f", "\u3000", "\ufeff",
	"\uff0c", "\uff0e", "\uff1a", "\uff1b", "\uff01", "\uff1f", "\uff08", "\uff09",
	"\uff3b", "\uff3d", "\uff5b", "\uff5d", "\u201c", "\u201d", "\u2018", "\u2019",
	"\u2026", "\u2014", "\u3002", "\u3001", "\u3003", "\u3008", "\u3009", "\u300c", "\u300d", "\u300e", "\u300f",
	"\u300a", "\u300b", "\u3010", "\u3011", "\u3014", "\u3015", "\u3016", "\u3017",
	"\u3018", "\u3019", "\u301a", "\u301b", "\u301c", "\u301d", "\u301e", "\u301f",
	"\u3030", "\u303d", "\u30a0", "\u30fb", "\ufe45", "\ufe46", "\uff61", "\uff62",
	"\uff63", "\uff64", "\uff65", "\u{16FE2}",
];
const delimiterProbes = ['"', "'", "="];
const tokenProbes = ["a", "Z", "7", "_", "-", "é", "ß", "文", "あ", "ア", "한", "ㄅ", "\u{20bb7}", "々", "Ａ"];

const classificationProbes = [
	...separatorProbes.map((character) => ({ character, kind: "separator" as const })),
	...delimiterProbes.map((character) => ({ character, kind: "pathDelimiter" as const })),
	...tokenProbes.map((character) => ({ character, kind: "token" as const })),
];
for (const probe of classificationProbes) {
	writeFixture(`boundary/${probe.character}说明.md`, "probe");
}
writeFixture("boundary/说明.md", "boundary");

writeFixture("cjk-path/中文/文档.txt", "text");
writeFixture("cjk-path/文档/说明.md", "text");
writeFixture("empty-prefix/说明.md", "text");
writeFixture("quoted/my folder/main.ts", "text");
writeFixture("quoted/资料，归档/说明.md", "text");
writeFixture("wrapped/src/main.cc", "text");
writeFixture("wrapped/(group)/layout.cc", "text");
writeFixture("wrapped/[slug]/page.tsx", "text");

const captureSuggestions = (
	provider: CombinedAutocompleteProvider,
	line: string,
	cursorCol: number,
	force: boolean,
) => provider.getSuggestions([line], 0, cursorCol, { signal: new AbortController().signal, force });

const completionCases: Array<{
	name: string;
	base: string;
	line: string;
	cursorCol?: number;
	force: boolean;
}> = [
	{ name: "cjk-local-file", base: "cjk-path", line: "./中文/文", force: true },
	{ name: "cjk-local-dir-forced", base: "cjk-path", line: "./中文/", force: true },
	{ name: "cjk-local-dir-natural", base: "cjk-path", line: "./中文/", force: false },
	{ name: "cjk-bare-dir-natural", base: "cjk-path", line: "文档/", force: false },
	{ name: "empty-prefix-ideographic-comma", base: "empty-prefix", line: "查看，", force: false },
	{ name: "empty-prefix-ideographic-space", base: "empty-prefix", line: "查看　", force: false },
	{ name: "empty-prefix-space", base: "empty-prefix", line: "查看 ", force: false },
	{ name: "empty-prefix-empty-line", base: "empty-prefix", line: "", force: false },
	{ name: "quote-space-directory", base: "quoted", line: "my", force: true },
	{ name: "quote-cjk-punctuation-directory", base: "quoted", line: "资料", force: true },
	{ name: "quoted-unclosed-token", base: "quoted", line: "查看，\"资料，归档/说", force: false },
	{
		name: "quoted-token-with-tail",
		base: "quoted",
		line: "查看，\"资料，归档/说\"后文",
		cursorCol: "查看，\"资料，归档/说".length,
		force: false,
	},
	{ name: "wrapper-open", base: "wrapped", line: "see (src/ma", force: true },
	{ name: "wrapper-closed", base: "wrapped", line: "(group)/la", force: true },
	{ name: "wrapper-bracket", base: "wrapped", line: "see [slug]/pa", force: true },
	{ name: "wrapper-quoted", base: "quoted", line: "see (\"my folder/ma", force: true },
	{ name: "slash-after-ascii-space", base: "empty-prefix", line: " /set", force: false },
	{ name: "slash-after-ideographic-space", base: "empty-prefix", line: "　/set", force: false },
	{ name: "slash-after-no-break-space", base: "empty-prefix", line: " /set", force: false },
	{ name: "argument-after-ideographic-space", base: "empty-prefix", line: "　/model g", force: false },
];
for (const probe of classificationProbes) {
	completionCases.push({
		name: `boundary-${probe.kind}-u${probe.character.codePointAt(0)!.toString(16)}`,
		base: "boundary",
		line: `${probe.character}说`,
		force: true,
	});
}

const completionObservation = [];
for (const testCase of completionCases) {
	const provider = new CombinedAutocompleteProvider(
		completionCommands,
		path.join(completionRoot, testCase.base),
		null,
	);
	const cursorCol = testCase.cursorCol ?? testCase.line.length;
	const result = await captureSuggestions(provider, testCase.line, cursorCol, testCase.force);
	const applied = result ? provider.applyCompletion([testCase.line], 0, cursorCol, result.items[0]!, result.prefix) : null;
	completionObservation.push({
		name: testCase.name,
		base: testCase.base,
		prefix: result?.prefix ?? null,
		values: result ? result.items.map((item) => item.value) : null,
		appliedLines: applied?.lines ?? null,
		appliedCursorCol: applied?.cursorCol ?? null,
	});
}

const quotedProvider = new CombinedAutocompleteProvider(completionCommands, path.join(completionRoot, "quoted"), null);
const quotedStart = (await captureSuggestions(quotedProvider, "资料", 2, true))!;
const quotedApplied = quotedProvider.applyCompletion(["资料"], 0, 2, quotedStart.items[0]!, quotedStart.prefix);
const quotedContinued = await captureSuggestions(
	quotedProvider,
	quotedApplied.lines[0]!,
	quotedApplied.cursorCol,
	true,
);
completionObservation.push({
	name: "quoted-directory-continuation",
	base: "quoted",
	prefix: quotedContinued?.prefix ?? null,
	values: quotedContinued ? quotedContinued.items.map((item) => item.value) : null,
	appliedLines: quotedApplied.lines,
	appliedCursorCol: quotedApplied.cursorCol,
});

const triggerProvider = new CombinedAutocompleteProvider(completionCommands, path.join(completionRoot, "empty-prefix"), null);
const forcedTriggerCases = [
	{ name: "slash-command-ascii-space", line: " /model" },
	{ name: "slash-command-ideographic-space", line: "　/model" },
	{ name: "slash-command-no-break-space", line: " /model" },
	{ name: "slash-command-trailing-space", line: "　/model " },
	{ name: "slash-command-argument", line: "　/model x" },
	{ name: "bare-text", line: "hello" },
];
const forcedTriggerObservation = forcedTriggerCases.map((triggerCase) => ({
	name: triggerCase.name,
	line: triggerCase.line,
	triggers: triggerProvider.shouldTriggerFileCompletion([triggerCase.line], 0, triggerCase.line.length),
}));

rmSync(completionRoot, { recursive: true, force: true });

const completionArtifact = envelope([
	"packages/tui/src/utils.ts:autocompleteSeparatorRegex,autocompleteBoundaryRegex",
	"packages/tui/src/autocomplete.ts:CombinedAutocompleteProvider.getSuggestions,extractPathPrefix,extractAtPrefix,applyCompletion,shouldTriggerFileCompletion",
], [{
	name: "completion-contexts",
	dimensions: { columns: 80, rows: 24 },
	inputs: {
		classification: classificationProbes.map((probe) => ({ character: probe.character, kind: probe.kind })),
		cases: completionCases.map((testCase) => ({
			name: testCase.name,
			base: testCase.base,
			line: testCase.line,
			cursorCol: testCase.cursorCol ?? testCase.line.length,
			force: testCase.force,
		})),
		continuation: { name: "quoted-directory-continuation", base: "quoted", line: "资料", cursorCol: 2, force: true },
		forcedTriggers: forcedTriggerCases.map((triggerCase) => ({
			name: triggerCase.name,
			line: triggerCase.line,
			cursorCol: triggerCase.line.length,
		})),
	},
	expected: {
		cases: completionObservation,
		forcedTriggers: forcedTriggerObservation,
	},
}]);

const editorTriggerCases = [
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

const editorTriggerObservation = [];
for (const triggerCase of editorTriggerCases) {
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
	editorTriggerObservation.push({
		name: triggerCase.name,
		input: triggerCase.input,
		text: editor.getText(),
		requests,
	});
}

const editorTriggerArtifact = envelope([
	"packages/tui/src/components/editor.ts:Editor.insertCharacter,handleTabCompletion,setAutocompleteProvider",
	"packages/tui/src/autocomplete.ts:AutocompleteProvider.getSuggestions",
	"packages/tui/src/components/editor.ts:Editor",
	"packages/tui/test/virtual-terminal.ts:VirtualTerminal",
], [{
	name: "editor-completion-triggers",
	dimensions: { columns: 80, rows: 24 },
	inputs: editorTriggerCases.map((triggerCase) => ({ name: triggerCase.name, input: triggerCase.input })),
	expected: { cases: editorTriggerObservation },
}]);

// No output is touched before source guards and all observations complete.
const bundle = path.join(root, selected.bundle);
mkdirSync(bundle, { recursive: true });
const artifacts = [
	["input.json", "input", inputArtifact],
	["component.json", "component", componentArtifact],
	["screen-state.json", "screen-state", screenArtifact],
	["capability-ledger.json", "capability-ledger", capabilityArtifact],
	["utils-width.json", "utils-width", utilsWidthArtifact],
	["keys-kitty-text.json", "keys-kitty-text", kittyArtifact],
	["utils-ansi.json", "utils-ansi", utilsAnsiArtifact],
	["fuzzy.json", "fuzzy", fuzzyArtifact],
	["latex.json", "latex", latexArtifact],
	["autocomplete.json", "autocomplete", completionArtifact],
	["editor-autocomplete.json", "editor-autocomplete", editorTriggerArtifact],
] as const;
const records = artifacts.map(([file, family, artifact]) => {
	const bytes = Buffer.from(JSON.stringify(artifact, null, 2) + "\n");
	writeFileSync(path.join(bundle, file), bytes);
	return { path: file, family, bytes: bytes.length, sha256: sha256(bytes), sourceEndpoints: artifact.sourceEndpoints };
});
const manifest = {
	schema: "pike/tui-evidence/1", baseline, revision, capturedAt, environment,
	generator: { path: "capture/capture-named-tui.mts", sha256: sha256(readFileSync(scriptPath)) },
	artifacts: records,
};
const manifestBytes = Buffer.from(JSON.stringify(manifest, null, 2) + "\n");
writeFileSync(path.join(bundle, "manifest.json"), manifestBytes);
selected.manifestSha256 = sha256(manifestBytes);
writeFileSync(registryPath, JSON.stringify(registry, null, 2) + "\n");
console.log(`captured ${records.length} independent TUI families for ${baseline} at ${revision}`);
