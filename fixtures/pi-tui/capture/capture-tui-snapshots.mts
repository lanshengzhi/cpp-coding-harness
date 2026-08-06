#!/usr/bin/env tsx
/**
 * Regenerates the committed pi-tui differential snapshots (issue #386, the
 * pi-tui completion gate).
 *
 * Imports the frozen pi-tui sources (`packages/tui/src/`) and writes the
 * committed JSON snapshots under `fixtures/pi-tui/`:
 *
 *   - `input-decode.json`   — the input-decoding corpus (fork-A evidence):
 *     escape sequence → key identifier/event type across the legacy,
 *     modifyOtherKeys, and Kitty tables, plus bracketed-paste framing and
 *     chunk-split boundaries. The kitty-mode re-readings of ambiguous legacy
 *     sequences are recorded per entry (`modeDependent`); the C++ decoder is
 *     single-table and matches the legacy column (see the fixture README).
 *   - `keybindings.json`    — the assembled default keybinding table
 *     (pi `TUI_KEYBINDINGS`, `packages/tui/src/keybindings.ts`).
 *   - `image-encoder.json`  — terminal-image encoder bytes (Kitty/iTerm2
 *     the OSC 8 `hyperlink` helper, and `imageFallback` output under a
 *     deterministic home directory and capability cache.
 *   - `utils.json`          — width/truncate/wrap/slice/strip outputs of
 *     pi `utils.ts` for a fixed corpus.
 *   - `fuzzy.json`          — `fuzzyMatch`/`fuzzyFilter` outputs for a fixed
 *     corpus.
 *   - `markdown.json`       — rendered output lines of pi's Markdown
 *     component for a fixed corpus under the recorded deterministic theme.
 *
 * Sanitization rules (see `fixtures/pi-tui/README.md`): deterministic
 * dimensions and environment (HOME is pinned), dummy data only — no
 * credentials exist in TUI fixtures by nature. The one non-deterministic
 * input (image data) is a fixed dummy base64 payload.
 *
 * Usage (from anywhere):
 *   ../pi/node_modules/.bin/tsx fixtures/pi-tui/capture/capture-tui-snapshots.mts
 * `PI_CHECKOUT` overrides the pi source checkout (default: sibling `../pi`).
 * The pi checkout MUST sit at the frozen parity baseline commit; the script
 * refuses to run otherwise.
 */

import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// Deterministic environment before any module that reads the home directory
// or terminal capabilities runs.
process.env.HOME = "/home/tester";
process.env.USERPROFILE = "/home/tester";
delete process.env.TERM;
delete process.env.TERM_PROGRAM;
delete process.env.TERM_PROGRAM_VERSION;
delete process.env.KITTY_WINDOW_ID;
delete process.env.WEZTERM_PANE;
delete process.env.TMUX;

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");
const FROZEN_COMMIT = "83114817c68f5413e4d7ba6d7003ddc511cd31d2";

const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], {
	encoding: "utf8",
}).trim();
if (head !== FROZEN_COMMIT) {
	throw new Error(
		`pi checkout must be at the frozen baseline ${FROZEN_COMMIT}, found ${head}`,
	);
}

const tuiSrc = (rel: string): string =>
	pathToFileURL(path.join(piCheckout, "packages/tui/src", rel)).href;

const {
	parseKey,
	isKeyRelease,
	isKeyRepeat,
	setKittyProtocolActive,
} = await import(tuiSrc("keys.ts"));
const { TUI_KEYBINDINGS } = await import(tuiSrc("keybindings.ts"));
const {
	encodeKitty,
	encodeITerm2,
	hyperlink,
	imageFallback,
	setCapabilities,
} = await import(tuiSrc("terminal-image.ts"));
const {
	visibleWidth,
	truncateToWidth,
	wrapTextWithAnsi,
	sliceByColumn,
	stripTerminalSequences,
} = await import(tuiSrc("utils.ts"));
const { fuzzyMatch, fuzzyFilter } = await import(tuiSrc("fuzzy.ts"));
const { Markdown } = await import(tuiSrc("components/markdown.ts"));


// ── Input-decoding corpus ──────────────────────────────────────────────────

interface ParsedExpectation {
	id: string | null;
	eventType: "press" | "repeat" | "release" | null;
}

interface BothModes {
	legacy: ParsedExpectation;
	kitty: ParsedExpectation;
}

function parseBothModes(sequence: string): BothModes {
	const read = (): ParsedExpectation => {
		const id = parseKey(sequence);
		if (id === undefined) return { id: null, eventType: null };
		const eventType = isKeyRelease(sequence)
			? "release"
			: isKeyRepeat(sequence)
				? "repeat"
				: "press";
		return { id, eventType };
	};
	setKittyProtocolActive(false);
	const legacy = read();
	setKittyProtocolActive(true);
	const kitty = read();
	setKittyProtocolActive(false);
	return { legacy, kitty };
}

// Sequences from pi's own tables (keys.ts at the frozen baseline):
// LEGACY_SEQUENCE_KEY_IDS, LEGACY_SHIFT/CTRL_SEQUENCES, the F-key SS3/CSI
// forms, single-byte keys, ESC-prefixed combos, modifyOtherKeys `CSI 27;m;k~`,
// Kitty CSI-u (plain, modified, event-typed, alternate/base-layout keys
// Kitty navigation/functional forms, and keypad equivalents.
const legacyTable: Array<[string, string]> = [
	["\x1b[A", "up"], ["\x1bOA", "up"], ["\x1b[B", "down"], ["\x1bOB", "down"],
	["\x1b[C", "right"], ["\x1bOC", "right"], ["\x1b[D", "left"], ["\x1bOD", "left"],
	["\x1b[H", "home"], ["\x1bOH", "home"], ["\x1b[F", "end"], ["\x1bOF", "end"],
	["\x1b[E", "clear"], ["\x1bOE", "clear"], ["\x1b[2~", "insert"],
	["\x1b[3~", "delete"], ["\x1b[5~", "pageUp"], ["\x1b[[5~", "pageUp"],
	["\x1b[6~", "pageDown"], ["\x1b[[6~", "pageDown"], ["\x1b[Z", "shift+tab"],
	["\x1b[1;1H", "home"], ["\x1b[1;1F", "end"],
	["\x1b[a", "shift+up"], ["\x1b[b", "shift+down"], ["\x1b[c", "shift+right"],
	["\x1b[d", "shift+left"], ["\x1b[e", "shift+clear"],
	["\x1b[2$", "shift+insert"], ["\x1b[3$", "shift+delete"],
	["\x1b[5$", "shift+pageUp"], ["\x1b[6$", "shift+pageDown"],
	["\x1b[7$", "shift+home"], ["\x1b[8$", "shift+end"],
	["\x1bOa", "ctrl+up"], ["\x1bOb", "ctrl+down"], ["\x1bOc", "ctrl+right"],
	["\x1bOd", "ctrl+left"], ["\x1bOe", "ctrl+clear"],
	["\x1b[2^", "ctrl+insert"], ["\x1b[3^", "ctrl+delete"],
	["\x1b[5^", "ctrl+pageUp"], ["\x1b[6^", "ctrl+pageDown"],
	["\x1b[7^", "ctrl+home"], ["\x1b[8^", "ctrl+end"],
	["\x1bOP", "f1"], ["\x1bOQ", "f2"], ["\x1bOR", "f3"], ["\x1bOS", "f4"],
	["\x1b[11~", "f1"], ["\x1b[12~", "f2"], ["\x1b[13~", "f3"], ["\x1b[14~", "f4"],
	["\x1b[[A", "f1"], ["\x1b[[B", "f2"], ["\x1b[[C", "f3"], ["\x1b[[D", "f4"],
	["\x1b[[E", "f5"], ["\x1b[15~", "f5"], ["\x1b[17~", "f6"], ["\x1b[18~", "f7"],
	["\x1b[19~", "f8"], ["\x1b[20~", "f9"], ["\x1b[21~", "f10"],
	["\x1b[23~", "f11"], ["\x1b[24~", "f12"],
	["\x1bb", "alt+left"], ["\x1bf", "alt+right"], ["\x1bp", "alt+up"],
	["\x1bn", "alt+down"], ["\x1bOM", "enter"],
	["\x1bB", "alt+left"], ["\x1bF", "alt+right"],
	["\t", "tab"], ["\r", "enter"], [" ", "space"],
	["\x7f", "backspace"], ["\x08", "backspace"], ["\x00", "ctrl+space"],
	["\x1c", "ctrl+\\"], ["\x1d", "ctrl+]"], ["\x1f", "ctrl+-"],
	["\x1b\x1b", "ctrl+alt+["], ["\x1b\x1c", "ctrl+alt+\\"],
	["\x1b\x1d", "ctrl+alt+]"], ["\x1b\x1f", "ctrl+alt+-"],
	["\x1b\x7f", "alt+backspace"], ["\x1b\b", "alt+backspace"],
	["a", "a"], ["5", "5"], ["!", "!"],
	["\x01", "ctrl+a"], ["\x1a", "ctrl+z"],
];

// Kitty CSI-u, navigation, functional, and keypad sequences.
const kittyTable: Array<[string, string]> = [
	["\x1b[97u", "a"], ["\x1b[97;1u", "a"], ["\x1b[97;2u", "shift+a"],
	["\x1b[97;3u", "alt+a"], ["\x1b[97;4u", "shift+alt+a"], ["\x1b[97;5u", "ctrl+a"],
	["\x1b[97;6u", "shift+ctrl+a"], ["\x1b[97;7u", "ctrl+alt+a"],
	["\x1b[97;8u", "shift+ctrl+alt+a"],
	["\x1b[97;5:2u", "ctrl+a"], ["\x1b[97;5:3u", "ctrl+a"],
	["\x1b[65;2u", "shift+a"], ["\x1b[97:65;5u", "ctrl+a"],
	["\x1b[97::99;5u", "ctrl+a"], ["\x1b[1089::99;5u", "ctrl+c"],
	["\x1b[27;2u", "shift+escape"], ["\x1b[9;2u", "shift+tab"], ["\x1b[13;2u", "shift+enter"],
	["\x1b[32;2u", "shift+space"], ["\x1b[127;2u", "shift+backspace"],
	["\x1b[1;5A", "ctrl+up"], ["\x1b[1;6:2D", "shift+ctrl+left"],
	["\x1b[1;3H", "alt+home"], ["\x1b[1;5:3F", "ctrl+end"],
	["\x1b[1;2B", "shift+down"],
	["\x1b[3;5~", "ctrl+delete"], ["\x1b[3;3:3~", "alt+delete"],
	["\x1b[2;2~", "shift+insert"], ["\x1b[5;5~", "ctrl+pageUp"],
	["\x1b[7;2~", "shift+home"], ["\x1b[8;2~", "shift+end"],
	["\x1b[57399u", "0"], ["\x1b[57400u", "1"], ["\x1b[57409u", "."],
	["\x1b[57413u", "+"], ["\x1b[57414u", "enter"], ["\x1b[57417u", "left"],
	["\x1b[57420u", "down"], ["\x1b[57423u", "home"], ["\x1b[57426u", "delete"],
];

// modifyOtherKeys (`CSI 27;modifier;codepoint~`).
const modifyOtherKeysTable: Array<[string, string]> = [
	["\x1b[27;1;97~", "a"], ["\x1b[27;2;65~", "shift+a"], ["\x1b[27;3;97~", "alt+a"],
	["\x1b[27;5;97~", "ctrl+a"], ["\x1b[27;6;65~", "shift+ctrl+a"],
	["\x1b[27;7;97~", "ctrl+alt+a"], ["\x1b[27;8;65~", "shift+ctrl+alt+a"],
	["\x1b[27;1;13~", "enter"],
	["\x1b[27;2;9~", "shift+tab"],
];

// ESC-prefixed alt/ctrl+alt combos: pi resolves them only when the Kitty
// protocol is NOT active (mode-dependent; the C++ single-table decoder keeps
// the legacy reading — see the fixture README).
const altCombos: string[] = [
	"a", "z", "0", "9", "!", "(", "[", "\\", ";", "'", ",", ".", "/", "`", "-", "=",
	"\x01", "\x1a", "\r", " ",
];

const corpus: Array<{ sequence: string; expected: ParsedExpectation }> = [];
const modeDependent: Array<{
	sequence: string;
	legacy: ParsedExpectation;
	kitty: ParsedExpectation;
}> = [];
// Recorded identifier-level divergences between pi's raw-string parse and
// the C++ typed decoder (see the fixture README): pi's identifier output is
// pinned here; the C++ column lives in the differential test and README.
const divergences: Array<{
	sequence: string;
	pi: ParsedExpectation;
}> = [];

function classify(sequence: string, expectedId: string): void {
	const both = parseBothModes(sequence);
	if (both.legacy.id === null || both.kitty.id === null) {
		modeDependent.push({ sequence, ...both });
		return;
	}
	if (both.legacy.id !== both.kitty.id || both.legacy.eventType !== both.kitty.eventType) {
		modeDependent.push({ sequence, ...both });
		return;
	}
	if (both.legacy.id !== expectedId) {
		throw new Error(`corpus sequence ${sequence}: pi parses as ${both.legacy.id}, expected ${expectedId}`);
	}
	corpus.push({ sequence, expected: both.legacy });
}

// Record a sequence where pi's identifier output is pinned for the README
// but the C++ decoder intentionally differs (uppercase letters stay
// lowercase in the identifier grammar with case preserved in inserted text;
// ESC-prefixed sequences the C++ decoder still resolves as alt+modified).
function recordDivergence(sequence: string): void {
	const both = parseBothModes(sequence);
	const pi = both.legacy;
	if (pi.id === both.kitty.id) {
		divergences.push({ sequence, pi });
	} else {
		// The kitty-mode reading is the only divergent one; the legacy
		// reading matches the C++ decoder, so it stays a corpus entry.
		corpus.push({ sequence, expected: pi });
	}
}

for (const [sequence, id] of [...legacyTable, ...kittyTable, ...modifyOtherKeysTable]) {
	classify(sequence, id);
}
// Uppercase letters: pi parses them as raw text identifiers ("A"); the C++
// decoder canonicalizes them to the shift+letter identifier with the typed
// case preserved in inserted text (recorded divergence, README).
for (const letter of "ABCDEFGHIJKLMNOPQRSTUVWXYZ") {
	recordDivergence(letter);
}
// Unshifted uppercase codepoints (terminals only send them shifted): pi
// rejects them (undefined); the C++ decoder still decodes them to the
// uppercase character key. Recorded divergence, README.
for (const sequence of ["\x1b[65;1u", "\x1b[65;5u", "\x1b[27;5;65~"]) {
	recordDivergence(sequence);
}
for (const suffix of altCombos) {
	const sequence = suffix === "\r" || suffix === " " ? `\x1b${suffix}` : `\x1b${suffix}`;
	const expected = suffix === "\r" ? "alt+enter" : suffix === " " ? "alt+space" : `alt+${suffix}`;
	classify(sequence, expected);
}
// `\n` alone: enter under the legacy table, shift+enter under the Kitty
// protocol (pi keys.ts; the C++ single-table decoder reads it as enter).
classify("\n", "enter");

// Sequences both sides must drop (control/output sequences, not keys).
const discarded = [
	"\x1b[2J", "\x1b[K", "\x1b[6n", "\x1b[?25l", "\x1b[?2004h",
	"\x1b]0;title\x07", "\x1b]9;4;1\x07", "\x1b]1337;File=inline=1:AA\x07",
	"\x1bP1|foo\x1b\\", "\x1b_Ga=T,f=100;AAAA\x1b\\", "\x1b[M",
	"\x1b[<35;20;5m",
];

// Bracketed-paste framing: pi stdin-buffer.ts BRACKETED_PASTE_START/END
// (`\x1b[200~` / `\x1b[201~`), with terminal.ts re-wrapping paste content.
const pasteFramings: Array<{ content: string; lines: number }> = [
	{ content: "plain paste", lines: 1 },
	{ content: "line one\nline two", lines: 2 },
	{ content: "\x1b[31mred\x1b[0m \x1b[27;5:3u \x1b[200~nested", lines: 1 },
];

// Chunk-split boundaries: sequences fed in small pieces must reassemble to
// the same full-buffer decode (pi stdin-buffer.test.ts split expectations).
const chunkSplits: Array<{ sequence: string; chunkSize: number; expected: ParsedExpectation }> = [
	{ sequence: "\x1b[A", chunkSize: 1, expected: { id: "up", eventType: "press" } },
	{ sequence: "\x1b[27;6;65~", chunkSize: 2, expected: { id: "shift+ctrl+a", eventType: "press" } },
	{ sequence: "\x1b[97;6:3u", chunkSize: 3, expected: { id: "shift+ctrl+a", eventType: "release" } },
	{ sequence: "\x1b[1;6:2D", chunkSize: 1, expected: { id: "shift+ctrl+left", eventType: "repeat" } },
	{ sequence: "\x1b[200~split paste\x1b[201~", chunkSize: 4, expected: { id: null, eventType: null } },
];

const inputDecode = {
	corpus: corpus.map(({ sequence, expected }) => ({
		sequence,
		expected,
	})),
	modeDependent: modeDependent.map(({ sequence, legacy, kitty }) => ({
		sequence,
		legacy,
		kitty,
	})),
	divergences: divergences.map(({ sequence, pi }) => ({
		sequence,
		pi,
	})),
	discarded: discarded.map((sequence) => sequence),
	paste: pasteFramings.map(({ content, lines }) => ({
		framed: `\x1b[200~${content}\x1b[201~`,
		content,
		lines,
	})),
	chunkSplits: chunkSplits.map(({ sequence, chunkSize, expected }) => ({
		sequence,
		chunkSize,
		expected,
	})),
};

// ── Keybinding table ───────────────────────────────────────────────────────

const keybindings = Object.entries(TUI_KEYBINDINGS).map(([id, definition]) => ({
	id,
	defaultKeys: Array.isArray(definition.defaultKeys)
		? definition.defaultKeys
		: [definition.defaultKeys],
	description: definition.description ?? "",
}));

// ── Image encoder ──────────────────────────────────────────────────────────

// Fixed dummy payloads; the long one exercises the 4096-byte chunk boundary.
const dummyBase64Short = "AAAA";
const dummyBase64Chunked = "A".repeat(8192);
const dummyName = "notes/screenshot 1.png";

setCapabilities({ images: "kitty", hyperlinks: true });
const fallbackLinked = imageFallback(
	"image/png",
	{ widthPx: 800, heightPx: 600 },
	"/home/tester/notes/screenshot 1.png",
);
setCapabilities({ images: "kitty", hyperlinks: false });
const fallbackPlain = imageFallback(
	"image/png",
	{ widthPx: 800, heightPx: 600 },
	"/home/tester/notes/screenshot 1.png",
);
const fallbackRelative = imageFallback("image/png", undefined, "notes/screenshot 1.png");
const fallbackNoFile = imageFallback("image/png", { widthPx: 1, heightPx: 1 });
setCapabilities({ images: "kitty", hyperlinks: true });
const imageEncoder = {
	kitty: [
		{
			name: "single-chunk",
			base64: dummyBase64Short,
			options: { columns: 2, rows: 1, imageId: 3, moveCursor: false },
			output: encodeKitty(dummyBase64Short, { columns: 2, rows: 1, imageId: 3, moveCursor: false })
		},
		{
			name: "chunked-4096",
			base64: dummyBase64Chunked,
			options: { columns: 10, rows: 5, imageId: 42, moveCursor: false },
			output: encodeKitty(dummyBase64Chunked, { columns: 10, rows: 5, imageId: 42, moveCursor: false })
		},
	],
	iterm2: [
		{
			name: "named",
			base64: dummyBase64Short,
			options: { width: 2, height: 1, name: dummyName },
			output: encodeITerm2(dummyBase64Short, { width: 2, height: 1, name: dummyName })
		},
		{
			name: "no-name",
			base64: dummyBase64Short,
			options: { width: 10, height: 5 },
			output: encodeITerm2(dummyBase64Short, { width: 10, height: 5 }),
		},
	],
	hyperlink: [
		{ text: "text", url: "https://example.com/a b?x=1", output: hyperlink("text", "https://example.com/a b?x=1") },
		{ text: "notes", url: "file:///home/tester/notes/a.txt", output: hyperlink("notes", "file:///home/tester/notes/a.txt") },
	],
	fallback: [
		{
			name: "linked-absolute",
			hyperlinks: true,
			filename: "/home/tester/notes/screenshot 1.png",
			dimensions: { widthPx: 800, heightPx: 600 },
			output: fallbackLinked
		},
		{
			name: "plain-absolute",
			hyperlinks: false,
			filename: "/home/tester/notes/screenshot 1.png",
			dimensions: { widthPx: 800, heightPx: 600 },
			output: fallbackPlain
		},
		{
			name: "relative",
			hyperlinks: true,
			filename: "notes/screenshot 1.png",
			output: fallbackRelative
		},
		{
			name: "no-file",
			hyperlinks: true,
			dimensions: { widthPx: 1, heightPx: 1 },
			output: fallbackNoFile
		},
	],
};

// ── Utility corpus ─────────────────────────────────────────────────────────

const utils = {
	visibleWidth: [
		{ name: "plain", input: "hello", output: visibleWidth("hello") },
		{ name: "ansi", input: "\x1b[31mabc\x1b[0m", output: visibleWidth("\x1b[31mabc\x1b[0m") },
		{ name: "cjk", input: "你好", output: visibleWidth("你好") },
		{ name: "emoji", input: "a😀b", output: visibleWidth("a😀b") },
		{ name: "flag", input: "🇩🇪", output: visibleWidth("🇩🇪") },
		{ name: "combining", input: "e\u0301", output: visibleWidth("e\u0301") },
		{ name: "osc8", input: "\x1b]8;;https://x\x1b\\link\x1b]8;;\x1b\\", output: visibleWidth("\x1b]8;;https://x\x1b\\link\x1b]8;;\x1b\\") },
		{ name: "tab", input: "a\tb", output: visibleWidth("a\tb") },
	],
	truncate: [
		{ name: "plain", input: "abcdef", width: 4, ellipsis: "...", pad: false, output: truncateToWidth("abcdef", 4) },
		{ name: "ansi", input: "\x1b[31mabcdef\x1b[0m", width: 4, ellipsis: "...", pad: false, output: truncateToWidth("\x1b[31mabcdef\x1b[0m", 4) },
		{ name: "fits", input: "abc", width: 8, ellipsis: "...", pad: false, output: truncateToWidth("abc", 8) },
		{ name: "pad", input: "abc", width: 8, ellipsis: "", pad: true, output: truncateToWidth("abc", 8, "", true) },
		{ name: "no-ellipsis", input: "abcdef", width: 4, ellipsis: "", pad: false, output: truncateToWidth("abcdef", 4, "") },
		{ name: "cjk", input: "你好世界", width: 5, ellipsis: "", pad: false, output: truncateToWidth("你好世界", 5, "") },
		{ name: "zero", input: "abc", width: 0, ellipsis: "...", pad: false, output: truncateToWidth("abc", 0) },
	],
	wrap: [
		{ name: "plain", input: "hello world foo bar", width: 8, output: wrapTextWithAnsi("hello world foo bar", 8) },
		{ name: "long-word", input: "supercalifragilistic", width: 6, output: wrapTextWithAnsi("supercalifragilistic", 6) },
		{ name: "ansi", input: "\x1b[31mred\x1b[0m and blue", width: 10, output: wrapTextWithAnsi("\x1b[31mred\x1b[0m and blue", 10) },
		{ name: "newlines", input: "one\ntwo", width: 20, output: wrapTextWithAnsi("one\ntwo", 20) },
		{ name: "wide-grapheme", input: "ab 😀 cd", width: 3, output: wrapTextWithAnsi("ab 😀 cd", 3) },
	],
	slice: [
		{ name: "plain", input: "abcdef", start: 1, length: 3, strict: false, output: sliceByColumn("abcdef", 1, 3) },
		{ name: "ansi", input: "\x1b[31mabcdef\x1b[0m", start: 1, length: 3, strict: false, output: sliceByColumn("\x1b[31mabcdef\x1b[0m", 1, 3) },
		{ name: "wide-strict", input: "a你b", start: 0, length: 2, strict: true, output: sliceByColumn("a你b", 0, 2, true) },
		{ name: "wide-lenient", input: "a你b", start: 0, length: 2, strict: false, output: sliceByColumn("a你b", 0, 2) },
		{ name: "empty", input: "abc", start: 4, length: 2, strict: false, output: sliceByColumn("abc", 4, 2) },
	],
	strip: [
		{ name: "sgr", input: "\x1b[31mred\x1b[0m text", output: stripTerminalSequences("\x1b[31mred\x1b[0m text") },
		{ name: "osc8", input: "\x1b]8;;https://x\x1b\\link\x1b]8;;\x1b\\", output: stripTerminalSequences("\x1b]8;;https://x\x1b\\link\x1b]8;;\x1b\\") },
		{ name: "osc-title", input: "\x1b]0;title\x07body", output: stripTerminalSequences("\x1b]0;title\x07body") },
		{ name: "apc", input: "\x1b_Ga=T;AAAA\x1b\\img", output: stripTerminalSequences("\x1b_Ga=T;AAAA\x1b\\img") },
		{ name: "newlines", input: "a\nb", output: stripTerminalSequences("a\nb") },
	],
};

// ── Fuzzy corpus ───────────────────────────────────────────────────────────

const fuzzy = {
	match: [
		{ query: "", text: "anything", output: fuzzyMatch("", "anything") },
		{ query: "abc", text: "xxabcxx", output: fuzzyMatch("abc", "xxabcxx") },
		{ query: "abc", text: "aabbcc", output: fuzzyMatch("abc", "aabbcc") },
		{ query: "abc", text: "ac", output: fuzzyMatch("abc", "ac") },
		{ query: "ABC", text: "xxabcxx", output: fuzzyMatch("ABC", "xxabcxx") },
		{ query: "al", text: "alpha", output: fuzzyMatch("al", "alpha") },
		{ query: "al", text: "alpine", output: fuzzyMatch("al", "alpine") },
		{ query: "gt", text: "gpt-5.6", output: fuzzyMatch("gt", "gpt-5.6") },
		{ query: "g56", text: "gpt-5.6", output: fuzzyMatch("g56", "gpt-5.6") },
		{ query: "56g", text: "gpt-5.6", output: fuzzyMatch("56g", "gpt-5.6") },
		{ query: "kimi", text: "kimi-for-coding", output: fuzzyMatch("kimi", "kimi-for-coding") },
		{ query: "deep", text: "deepseek-v4-flash", output: fuzzyMatch("deep", "deepseek-v4-flash") },
		{ query: "not-there", text: "short", output: fuzzyMatch("not-there", "short") },
		{ query: "abc", text: "abcdef", output: fuzzyMatch("abc", "abcdef") },
	],
	filter: [
		{ name: "empty-query", items: ["alpha", "beta"], query: "", output: fuzzyFilter(["alpha", "beta"], "", (s) => s) },
		{ name: "rank", items: ["alpha", "beta", "alpine", "alps"], query: "al", output: fuzzyFilter(["alpha", "beta", "alpine", "alps"], "al", (s) => s) },
		{ name: "tokens", items: ["src/tui/Editor.cpp", "src/ai/Editor.cpp", "docs/editor.md"], query: "tui editor", output: fuzzyFilter(["src/tui/Editor.cpp", "src/ai/Editor.cpp", "docs/editor.md"], "tui editor", (s) => s) },
		{ name: "exact", items: ["model-selection", "model", "models.json"], query: "model", output: fuzzyFilter(["model-selection", "model", "models.json"], "model", (s) => s) },
		{ name: "slash", items: ["a/b/c", "a/b", "x/y"], query: "a/c", output: fuzzyFilter(["a/b/c", "a/b", "x/y"], "a/c", (s) => s) },
	],
};

// ── Markdown corpus ────────────────────────────────────────────────────────

// The deterministic theme recorded in the snapshot; the C++ differential
// test rebuilds equivalent hooks from this table.
const markdownStyles = {
	heading: { prefix: "\x1b[36m", suffix: "\x1b[39m" },
	bold: { prefix: "\x1b[1m", suffix: "\x1b[22m" },
	italic: { prefix: "\x1b[3m", suffix: "\x1b[23m" },
	strikethrough: { prefix: "\x1b[9m", suffix: "\x1b[29m" },
	underline: { prefix: "\x1b[4m", suffix: "\x1b[24m" },
	code: { prefix: "\x1b[33m", suffix: "\x1b[39m" },
	codeBlock: { prefix: "\x1b[32m", suffix: "\x1b[39m" },
	codeBlockBorder: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
	quote: { prefix: "\x1b[3m", suffix: "\x1b[23m" },
	quoteBorder: { prefix: "\x1b[34m", suffix: "\x1b[39m" },
	hr: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
	listBullet: { prefix: "\x1b[35m", suffix: "\x1b[39m" },
	link: { prefix: "\x1b[4m", suffix: "\x1b[24m" },
	linkUrl: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
};

const wrapStyle = (style: { prefix: string; suffix: string }) =>
	(text: string) => `${style.prefix}${text}${style.suffix}`;

const markdownTheme = {
	heading: wrapStyle(markdownStyles.heading),
	link: wrapStyle(markdownStyles.link),
	linkUrl: wrapStyle(markdownStyles.linkUrl),
	code: wrapStyle(markdownStyles.code),
	codeBlock: wrapStyle(markdownStyles.codeBlock),
	codeBlockBorder: wrapStyle(markdownStyles.codeBlockBorder),
	quote: wrapStyle(markdownStyles.quote),
	quoteBorder: wrapStyle(markdownStyles.quoteBorder),
	hr: wrapStyle(markdownStyles.hr),
	listBullet: wrapStyle(markdownStyles.listBullet),
	bold: wrapStyle(markdownStyles.bold),
	italic: wrapStyle(markdownStyles.italic),
	strikethrough: wrapStyle(markdownStyles.strikethrough),
	underline: wrapStyle(markdownStyles.underline),
	codeBlockIndent: "  ",
};

const markdownCases: Array<{ name: string; markdown: string; width: number }> = [
	{
		name: "plain-paragraph",
		markdown: "A plain paragraph with some words to wrap across lines.",
		width: 20,
	},
	{
		name: "inline-styles",
		markdown: "Text with *emphasis*, **strong**, ~~gone~~, and `code`.",
		width: 40,
	},
	{
		name: "strict-strikethrough",
		markdown: "~~valid~~ ~~ still open and `~~code~~` inside.",
		width: 40,
	},
	{
		name: "fenced-code",
		markdown: "```cpp\nint answer = 42;\n```",
		width: 24,
	},
	{
		// Untermined trailing partial fence (`` shorter than the opening
		// ```): pi trims the streamed partial closing fence so code blocks
		// do not shrink/flicker (markdown.ts trimPartialClosingFences).
		name: "streamed-fence-trim",
		markdown: "```cpp\nint answer = 42;\n``",
		width: 24,
	},
	{
		name: "list",
		markdown: "- first item\n- second item",
		width: 24,
	},
	{
		name: "quote",
		markdown: "> quoted words",
		width: 24,
	},
	{
		name: "rule",
		markdown: "---",
		width: 24,
	},
	{
		name: "link",
		markdown: "[site](https://example.com)",
		width: 40,
	},
	{
		name: "html-passthrough",
		markdown: "<div>plain html</div>",
		width: 40,
	},
];

const markdown = {
	styles: markdownStyles,
	cases: markdownCases.map(({ name, markdown: source, width }) => ({
		name,
		markdown: source,
		width,
		lines: new Markdown(source, 0, 0, markdownTheme).render(width),
	})),
};

// ── Write snapshots ────────────────────────────────────────────────────────

// The snapshots are plain JSON (no header comment), matching the pure-JSON
// `fixtures/pi-ai/` convention; provenance lives in the fixture README.
const header = "";

const files: Array<[string, unknown]> = [
	["input-decode.json", inputDecode],
	["keybindings.json", keybindings],
	["image-encoder.json", imageEncoder],
	["utils.json", utils],
	["fuzzy.json", fuzzy],
	["markdown.json", markdown],
];

for (const [name, value] of files) {
	const payload = `${header}\n${JSON.stringify(value, null, 2)}\n`;
	writeFileSync(path.join(fixtureDir, name), payload);
	console.log(`wrote fixtures/pi-tui/${name}`);
}

console.log(`input-decode: ${inputDecode.corpus.length} corpus, ${inputDecode.modeDependent.length} mode-dependent, ${inputDecode.divergences.length} divergences, ${inputDecode.discarded.length} discarded, ${inputDecode.chunkSplits.length} chunk splits`);
console.log(`keybindings: ${keybindings.length} entries`);
console.log(`markdown: ${markdown.cases.length} cases`);
