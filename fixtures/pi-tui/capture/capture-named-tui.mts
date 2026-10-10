#!/usr/bin/env tsx
/** Independent named TUI capture (#947). Never imports Pike or historical fixtures.
 * PI_CHECKOUT must be a clean checkout at the registry's full revision.
 * Later tickets extend the cases below and add artifacts with the same envelope.
 */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
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
const { visibleWidth } = await import(src("src/utils.ts"));
const { Input } = await import(src("src/components/input.ts"));
const { Text } = await import(src("src/components/text.ts"));
const { TuiMainScreen } = await import(src("src/tui-main-screen.ts"));
const { CURSOR_MARKER } = await import(src("src/tui.ts"));
const { VirtualTerminal } = await import(src("test/virtual-terminal.ts"));
const { renderLatex } = await import(src("src/latex.ts"));
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

// No output is touched before source guards and all observations complete.
const bundle = path.join(root, selected.bundle);
mkdirSync(bundle, { recursive: true });
const artifacts = [
	["input.json", "input", inputArtifact],
	["component.json", "component", componentArtifact],
	["screen-state.json", "screen-state", screenArtifact],
	["capability-ledger.json", "capability-ledger", capabilityArtifact],
	["utils-width.json", "utils-width", utilsWidthArtifact],
	["latex.json", "latex", latexArtifact],
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
