#!/usr/bin/env tsx
/**
 * Capture the pi-v1.3.0 StdinBuffer framing surface for issue #1221 route
 * batch B13, slice B: paste start/end and collapsed paste, the buffer
 * lifecycle (getBuffer/clear/destroy), emitDataSequence and the Kitty
 * printable duplicate suppression, timeout versus escape-timeout selection,
 * the event/options surface, flush, and the registered fragmented-color reply
 * scenario.
 *
 * Every value below comes from executing the frozen checkout's own module
 * through its runtime surface; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221-stdin-framing/capture.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1221-stdin-framing";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT
	? path.resolve(process.env.PI_CHECKOUT)
	: path.resolve(repositoryRoot, "../pi");

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: {
			...process.env,
			NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
			TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
		},
	});
	if (result.error) throw new Error(`cannot re-exec ${tsx}: ${result.error}`);
	process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
	throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
	throw new Error(
		`unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
			Object.keys(registry.baselines).sort().join(", "),
	);
}
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
	throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report?.getReport() as any)?.header?.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
	throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
	if (name !== baselineName && entry.bundles && family in entry.bundles) {
		throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
	}
}

let tsxVersion = "unknown";
try {
	tsxVersion = execFileSync(path.join(piCheckout, "node_modules", ".bin", "tsx"), ["--version"], {
		encoding: "utf8",
	}).trim();
} catch {
	// Provenance, not a capture precondition.
}
const environment = {
	platform: process.platform,
	arch: "x64",
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const source = (file: string) => pathToFileURL(path.join(piCheckout, "packages/tui/src", file)).href;
const keys = await import(source("keys.ts"));
const { StdinBuffer } = await import(source("stdin-buffer.ts"));

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

interface Step {
	process?: string;
	flush?: boolean;
	clear?: boolean;
	destroy?: boolean;
	waitMs?: number;
	emitDataSequence?: string;
}

interface Observation {
	data: string[];
	paste: string[];
	events: Array<{ type: string; value: string }>;
	buffer: string;
	pasteMode: boolean;
	pasteBuffer: string;
	timeout: boolean;
	pending: number | null;
	flushReturn?: string[];
}

function observe(
	buffer: any,
	data: string[],
	paste: string[],
	events: Array<{ type: string; value: string }>,
	flushReturn?: string[],
): Observation {
	const observation: Observation = {
		data: [...data],
		paste: [...paste],
		events: [...events],
		buffer: buffer.getBuffer(),
		pasteMode: buffer.pasteMode,
		pasteBuffer: buffer.pasteBuffer,
		timeout: buffer.timeout !== null,
		pending: buffer.pendingKittyPrintableCodepoint ?? null,
	};
	if (flushReturn !== undefined) observation.flushReturn = flushReturn;
	return observation;
}

async function runSteps(options: any, steps: Step[]) {
	const buffer: any = new StdinBuffer(options);
	const data: string[] = [];
	const paste: string[] = [];
	const events: Array<{ type: string; value: string }> = [];
	buffer.on("data", (value: string) => {
		data.push(value);
		events.push({ type: "data", value });
	});
	buffer.on("paste", (value: string) => {
		paste.push(value);
		events.push({ type: "paste", value });
	});
	const observations: Observation[] = [];
	for (const step of steps) {
		let flushReturn: string[] | undefined;
		if (step.process !== undefined) buffer.process(step.process);
		if (step.flush) flushReturn = buffer.flush();
		if (step.clear) buffer.clear();
		if (step.destroy) buffer.destroy();
		if (step.emitDataSequence !== undefined) buffer.emitDataSequence(step.emitDataSequence);
		if (step.waitMs !== undefined) await delay(step.waitMs);
		observations.push(observe(buffer, data, paste, events, flushReturn));
	}
	buffer.destroy();
	return observations;
}

// 1. Options and event surface: defaults, explicit values, event names and
//    the data/paste event order for a collapsed paste.
const defaultBuffer: any = new StdinBuffer();
const explicitBuffer: any = new StdinBuffer({ timeout: 37, escapeTimeout: 13 });
const optionsEvents: Array<{ type: string; value: string }> = [];
const optionsData: string[] = [];
const optionsPaste: string[] = [];
explicitBuffer.on("data", (value: string) => {
	optionsData.push(value);
	optionsEvents.push({ type: "data", value });
});
explicitBuffer.on("paste", (value: string) => {
	optionsPaste.push(value);
	optionsEvents.push({ type: "paste", value });
});
const optionsStream = "a\x1b[200~hi\x1b[201~b";
explicitBuffer.process(optionsStream);
const optionsScenario = {
	name: "stdin-framing-options-and-events",
	inputs: {
		defaultOptions: {},
		explicitOptions: { timeout: 37, escapeTimeout: 13 },
		stream: optionsStream,
	},
	expected: {
		defaultTimeoutMs: defaultBuffer.timeoutMs,
		defaultEscapeTimeoutMs: defaultBuffer.escapeTimeoutMs,
		explicitTimeoutMs: explicitBuffer.timeoutMs,
		explicitEscapeTimeoutMs: explicitBuffer.escapeTimeoutMs,
		eventNames: explicitBuffer.eventNames(),
		data: optionsData,
		paste: optionsPaste,
		events: optionsEvents,
	},
	normalizations: [],
};
defaultBuffer.destroy();
explicitBuffer.destroy();

// 2. Paste start/end, collapsed paste, chunked paste, and the paste buffer
//    surviving a flush until the end marker arrives.
const pasteCases: Array<{ name: string; steps: Step[] }> = [
	{ name: "complete-paste", steps: [{ process: "\x1b[200~hello\x1b[201~" }] },
	{ name: "collapsed-paste", steps: [{ process: "\x1b[200~hello\x1b[201~x" }] },
	{
		name: "chunked-paste",
		steps: [
			{ process: "\x1b[200~hel" },
			{ process: "lo" },
			{ process: "\x1b[201~" },
			{ process: "x" },
		],
	},
	{ name: "paste-after-data", steps: [{ process: "a\x1b[200~hello\x1b[201~" }] },
	{
		name: "partial-paste-survives-flush",
		steps: [{ process: "\x1b[200~hel" }, { flush: true }, { process: "lo\x1b[201~" }],
	},
	{ name: "empty-paste", steps: [{ process: "\x1b[200~\x1b[201~" }] },
	{ name: "paste-then-tail-csi", steps: [{ process: "\x1b[200~x\x1b[201~\x1b[A" }] },
	{ name: "paste-start-inside-batch", steps: [{ process: "\x1b[A\x1b[200~x\x1b[201~" }] },
	{
		name: "clear-cancels-paste",
		steps: [{ process: "\x1b[200~hel" }, { clear: true }, { process: "lo\x1b[201~" }],
	},
	{
		name: "destroy-cancels-paste",
		steps: [{ process: "\x1b[200~hel" }, { destroy: true }, { process: "lo\x1b[201~" }],
	},
];
const pasteScenario = {
	name: "stdin-framing-paste-lifecycle",
	inputs: { cases: pasteCases },
	expected: {
		cases: await Promise.all(
			pasteCases.map(async ({ name, steps }) => ({
				name,
				observations: await runSteps({ timeout: 50, escapeTimeout: 10 }, steps),
			})),
		),
	},
	normalizations: [],
};

// 3. Buffer lifecycle: getBuffer snapshots, clear/destroy, and flush on an
//    empty buffer. A recognized reply header arms no timer, so destroy and
//    clear observe an already-empty deadline.
const bufferCases: Array<{ name: string; steps: Step[] }> = [
	{ name: "partial-csi-buffer", steps: [{ process: "\x1b[<35" }] },
	{ name: "clear-buffer", steps: [{ process: "\x1b[<35" }, { clear: true }] },
	{
		name: "destroy-buffer",
		steps: [{ process: "\x1b]4;1;rgb:eaea/6c" }, { destroy: true }],
	},
	{ name: "flush-empty", steps: [{ flush: true }] },
	{ name: "clear-after-complete", steps: [{ process: "a" }, { clear: true }] },
];
const bufferScenario = {
	name: "stdin-framing-buffer-lifecycle",
	inputs: { cases: bufferCases },
	expected: {
		cases: await Promise.all(
			bufferCases.map(async ({ name, steps }) => ({
				name,
				observations: await runSteps({ timeout: 50, escapeTimeout: 10 }, steps),
			})),
		),
	},
	normalizations: [],
};

// 4. emitDataSequence and pendingKittyPrintableCodepoint: the bare CSI-u
//    printable report suppresses the immediately following raw character;
//    explicit modifiers and control codepoints do not arm the suppression.
const emitCases: Array<{ name: string; steps: Step[] }> = [
	{
		name: "bare-kitty-then-raw",
		steps: [{ process: "\x1b[97u" }, { process: "a" }, { process: "a" }],
	},
	{
		name: "explicit-kitty-then-raw",
		steps: [{ process: "\x1b[97;1u" }, { process: "a" }],
	},
	{
		name: "control-codepoint",
		steps: [{ process: "\x1b[13u" }, { process: "\r" }],
	},
	{
		name: "direct-emit-sequence",
		steps: [
			{ emitDataSequence: "\x1b[97u" },
			{ emitDataSequence: "a" },
			{ emitDataSequence: "a" },
		],
	},
	{
		name: "pending-cleared-by-paste",
		steps: [
			{ process: "\x1b[97u" },
			{ process: "\x1b[200~x\x1b[201~" },
			{ process: "a" },
		],
	},
];
const emitScenario = {
	name: "stdin-framing-emit-and-kitty-dedup",
	inputs: { cases: emitCases },
	expected: {
		cases: await Promise.all(
			emitCases.map(async ({ name, steps }) => ({
				name,
				observations: await runSteps({ timeout: 50, escapeTimeout: 10 }, steps),
			})),
		),
	},
	normalizations: [],
};

// 5. Real timer selection: a lone ESC resolves at escapeTimeout, every other
//    held fragment at timeout, and a recognized reply header arms no timer at
//    all. The 150 ms checkpoints are well inside a 300 ms sequence window and
//    well outside a 50 ms escape window.
const timeoutOptions = { timeout: 300, escapeTimeout: 50 };
const timeoutCases: Array<{ name: string; steps: Step[] }> = [
	{
		name: "lone-escape",
		steps: [{ process: "\x1b" }, { waitMs: 150 }],
	},
	{
		name: "incomplete-csi",
		steps: [{ process: "\x1b[" }, { waitMs: 150 }, { waitMs: 200 }],
	},
	{
		name: "ambiguous-osc",
		steps: [{ process: "\x1b]hello" }, { waitMs: 150 }, { waitMs: 200 }],
	},
	{
		name: "recognized-osc-header",
		steps: [
			{ process: "\x1b]4;1;rgb:eaea/6c" },
			{ waitMs: 150 },
			{ process: "6c/6d6d\x07" },
		],
	},
	{
		name: "recognized-apc-header",
		steps: [{ process: "\x1b_Gi=1;" }, { waitMs: 150 }, { process: "OK\x1b\\" }],
	},
	{
		name: "recognized-dcs-header",
		steps: [{ process: "\x1bP>|" }, { waitMs: 150 }, { process: "x\x1b\\" }],
	},
];
const timeoutScenario = {
	name: "stdin-framing-timeout-selection",
	inputs: { options: timeoutOptions, cases: timeoutCases },
	expected: {
		cases: await Promise.all(
			timeoutCases.map(async ({ name, steps }) => ({
				name,
				observations: await runSteps(timeoutOptions, steps),
			})),
		),
	},
	normalizations: [],
};

// 6. flush: pi's flush returns the held fragment without emitting; the timer
//    callback is what emits it, reparsing an ambiguous control-string prefix.
//    Both halves are recorded so the C++ seam (which combines them) can be
//    compared against the frozen pair.
async function flushCase(name: string, steps: Step[]) {
	const buffer: any = new StdinBuffer({ timeout: 50, escapeTimeout: 10 });
	const data: string[] = [];
	const paste: string[] = [];
	const events: Array<{ type: string; value: string }> = [];
	buffer.on("data", (value: string) => {
		data.push(value);
		events.push({ type: "data", value });
	});
	buffer.on("paste", (value: string) => {
		paste.push(value);
		events.push({ type: "paste", value });
	});
	const observations: Observation[] = [];
	for (const step of steps) {
		let flushReturn: string[] | undefined;
		if (step.process !== undefined) buffer.process(step.process);
		if (step.flush) flushReturn = buffer.flush();
		if (step.clear) buffer.clear();
		if (step.destroy) buffer.destroy();
		if (step.emitDataSequence !== undefined) buffer.emitDataSequence(step.emitDataSequence);
		if (step.waitMs !== undefined) await delay(step.waitMs);
		observations.push(observe(buffer, data, paste, events, flushReturn));
	}
	const flushData = [...data];
	buffer.destroy();

	// The same input through the real timer callback, which is the half of
	// pi's flush contract that actually emits.
	const timerBuffer: any = new StdinBuffer({ timeout: 50, escapeTimeout: 10 });
	const timerData: string[] = [];
	timerBuffer.on("data", (value: string) => timerData.push(value));
	for (const step of steps) {
		if (step.process !== undefined) timerBuffer.process(step.process);
	}
	await delay(120);
	const timerObservation = observe(timerBuffer, timerData, [], []);
	timerBuffer.destroy();

	return {
		name,
		flushReturn: observations.at(-1)?.flushReturn ?? [],
		flushData,
		timerCallbackData: timerObservation.data,
		observations,
	};
}
const flushCases: Array<{ name: string; steps: Step[] }> = [
	{ name: "flush-returns-held-fragment", steps: [{ process: "\x1b[<35" }, { flush: true }] },
	{ name: "flush-control-string", steps: [{ process: "\x1b]a" }, { flush: true }] },
	{ name: "flush-empty", steps: [{ flush: true }] },
	{
		name: "flush-clears-pending",
		steps: [{ process: "\x1b[97u" }, { process: "\x1b[" }, { flush: true }],
	},
];
const flushScenario = {
	name: "stdin-framing-flush",
	inputs: { cases: flushCases },
	expected: {
		cases: await Promise.all(flushCases.map(({ name, steps }) => flushCase(name, steps))),
	},
	normalizations: [],
};

// 7. The registered fragmented-color reply scenario, recaptured in this
//    family: the complete OSC 4 header stays buffered past the sequence
//    timeout, the tail completes one out-of-band reply, and a real BEL still
//    arrives as ctrl+g.
const colorReply = "\x1b]4;1;rgb:eaea/6c6c/6d6d\x07";
const colorHeader = "\x1b]4;1;rgb:eaea/6c";
const colorBuffer: any = new StdinBuffer({ timeout: 20, escapeTimeout: 10 });
const colorData: string[] = [];
colorBuffer.on("data", (value: string) => colorData.push(value));
colorBuffer.process(colorHeader);
await delay(80);
const afterHeaderTimeout = [...colorData];
const bufferedAfterHeaderTimeout = colorBuffer.getBuffer();
colorBuffer.process(colorReply.slice(colorHeader.length));
const afterTail = [...colorData];
colorBuffer.process("\x07");
const afterRealCtrlG = [...colorData];
const colorObservation = {
	afterHeaderTimeout,
	bufferedAfterHeaderTimeout,
	afterTail,
	afterTailCtlGMatch: afterTail.some((value) => keys.matchesKey(value, "ctrl+g")),
	afterRealCtrlG,
	realCtrlGDelivered: afterRealCtrlG.length === afterTail.length + 1,
};
colorBuffer.destroy();
const colorScenario = {
	name: "fragmented-color-reply-header-survives-deadline",
	inputs: {
		reply: colorReply,
		header: colorHeader,
		split: [colorHeader.length, colorReply.length - colorHeader.length],
		timeoutMs: 20,
		tailWaitMs: 80,
	},
	expected: colorObservation,
	normalizations: [],
};

const artifacts = [
	{
		path: "stdin-framing.json",
		sourceEndpoints: [
			"packages/tui/src/stdin-buffer.ts:StdinBufferOptions,StdinBufferEventMap,StdinBuffer.constructor,StdinBuffer.process,StdinBuffer.emitDataSequence,StdinBuffer.flush,StdinBuffer.clear,StdinBuffer.getBuffer,StdinBuffer.destroy",
			"packages/tui/src/keys.ts:matchesKey",
		],
		scenarios: [
			optionsScenario,
			pasteScenario,
			bufferScenario,
			emitScenario,
			timeoutScenario,
			flushScenario,
			colorScenario,
		],
	},
];

const manifestArtifacts = [];
const artifactBytes = new Map<string, Buffer>();
for (const artifact of artifacts) {
	const document = {
		schema: "pike/pi-v1.3.0-audit-evidence/1",
		baseline: baselineName,
		revision: baseline.revision,
		capturedAt,
		environment,
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	};
	const bytes = Buffer.from(`${JSON.stringify(document, null, 2)}\n`);
	artifactBytes.set(artifact.path, bytes);
	manifestArtifacts.push({
		path: artifact.path,
		family,
		bytes: bytes.length,
		sha256: sha256(bytes),
		sourceEndpoints: artifact.sourceEndpoints,
		scenarios: artifact.scenarios,
	});
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1221",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }],
	},
	artifacts: manifestArtifacts,
};

mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ` +
		`${manifestArtifacts.reduce((total, item) => total + item.scenarios.length, 0)} scenarios) ` +
		`for ${baselineName} (${baseline.revision})`,
);
