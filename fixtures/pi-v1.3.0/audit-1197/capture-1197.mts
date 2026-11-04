#!/usr/bin/env tsx
/**
 * Capture the `audit-1197` frozen read-tool observations for the shared
 * pi-v1.3.0 evidence registry.
 *
 * The frozen durable `read` tool (`packages/durable/src/tools/read.ts`) reads
 * text with one-based line `offset`/`limit`, fails when the offset passes the
 * last line, and bounds its default output at 50 KiB / 2000 lines with
 * structured truncation details and diagnostics. This family captures that
 * behavior independently from the `audit-1158` EnvIO byte-reader
 * observations, which cover only 0-based positioned reads.
 *
 * The capture drives the frozen tool registration directly over a
 * `NodeExecutionEnv` with a minimal execution API (the text path consults
 * only `api.env`), records the exact output text, diagnostics and
 * `details.truncation` per call, and refuses a checkout whose HEAD is not the
 * registered baseline revision or whose tracked sources are modified.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1197/capture-1197.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1197";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

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
	if (result.error) {
		throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
	}
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
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
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
	// The tsx version is provenance, not a capture precondition.
}
const environment = {
	platform: process.platform,
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const { BACKGROUND_CONTEXT } = await import("@earendil-works/chord/context");
const { NodeExecutionEnv } = await import("@earendil-works/pi-durable/env/node");
const { createReadTool } = await import("@earendil-works/pi-durable/tools");

const context = BACKGROUND_CONTEXT;
const testRoot = mkdtempSync(path.join(tmpdir(), "cch-audit-1197-"));

/** Write `content` at `relative` below `root`, creating parent directories. */
function write(root: string, relative: string, content: string | Uint8Array): void {
	const target = path.join(root, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	writeFileSync(target, content);
}

function api(env: any): any {
	return {
		env,
		agent: async () => ({ model: undefined }),
		models: { getModel: () => undefined },
		diagnostic: () => undefined,
		output: () => undefined,
		retainedOutput: () => ({ text: "", truncated: false }),
	};
}

/** One frozen call: the recorded output text, diagnostics, details or error. */
async function call(env: any, args: any): Promise<any> {
	const tool = createReadTool({});
	let result: any;
	try {
		result = await tool.execute(args, api(env), context);
	} catch (error: any) {
		return { error: String(error?.message ?? error) };
	}
	const text = result.output
		.filter((block: any) => block.type === "text")
		.map((block: any) => block.text)
		.join("");
	return {
		text,
		blocks: result.output.length,
		diagnostics: result.diagnostics ?? [],
		...(result.details === undefined ? {} : { details: result.details }),
	};
}

function scenario(name: string, inputs: any, expected: any, normalizations: string[] = []) {
	return { name, inputs, expected, normalizations };
}

const scenarios = [];

// Whole-file reads without parameters.
{
	const root = path.join(testRoot, "plain");
	const env = new NodeExecutionEnv({ cwd: root });
	write(root, "small.txt", "alpha\nbeta\ngamma\n");
	write(root, "no-trailing-newline.txt", "alpha\nbeta\ngamma");
	write(root, "empty.txt", "");
	write(root, "crlf.txt", "one\r\ntwo\r\n");
	scenarios.push(
		scenario(
			"a small file reads whole without truncation",
			{ file: "small.txt", content: "alpha\nbeta\ngamma\n", args: { path: "small.txt" } },
			await call(env, { path: "small.txt" }),
		),
		scenario(
			"a trailing newline does not add an empty line",
			{ file: "small.txt", content: "alpha\nbeta\ngamma\n", args: { path: "small.txt", offset: 3 } },
			await call(env, { path: "small.txt", offset: 3 }),
		),
		scenario(
			"an offset on the line after a trailing newline is past the end",
			{ file: "small.txt", content: "alpha\nbeta\ngamma\n", args: { path: "small.txt", offset: 4 } },
			await call(env, { path: "small.txt", offset: 4 }),
		),
		scenario(
			"carriage returns are line content, not line endings",
			{ file: "crlf.txt", content: "one\r\ntwo\r\n", args: { path: "crlf.txt" } },
			await call(env, { path: "crlf.txt" }),
		),
		scenario(
			"an empty file at offset one reads empty",
			{ file: "empty.txt", content: "", args: { path: "empty.txt", offset: 1 } },
			await call(env, { path: "empty.txt", offset: 1 }),
		),
		scenario(
			"an empty file at offset two is past the end",
			{ file: "empty.txt", content: "", args: { path: "empty.txt", offset: 2 } },
			await call(env, { path: "empty.txt", offset: 2 }),
		),
		scenario(
			"a file without a trailing newline counts its last line",
			{ file: "no-trailing-newline.txt", content: "alpha\nbeta\ngamma", args: { path: "no-trailing-newline.txt", offset: 3, limit: 5 } },
			await call(env, { path: "no-trailing-newline.txt", offset: 3, limit: 5 }),
		),
	);
}

// One-based offset/limit selection and the slice-index edge semantics.
{
	const root = path.join(testRoot, "range");
	const env = new NodeExecutionEnv({ cwd: root });
	const content = "one\ntwo\nthree\nfour\nfive\n";
	write(root, "five.txt", content);
	scenarios.push(
		scenario(
			"offset and limit select one-based lines",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 2, limit: 2 } },
			await call(env, { path: "five.txt", offset: 2, limit: 2 }),
		),
		scenario(
			"an offset without a limit reads to the end of the file",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 4 } },
			await call(env, { path: "five.txt", offset: 4 }),
		),
		scenario(
			"a limit of zero selects no lines",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 1, limit: 0 } },
			await call(env, { path: "five.txt", offset: 1, limit: 0 }),
		),
		scenario(
			"a negative limit counts from the end of the file",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 1, limit: -2 } },
			await call(env, { path: "five.txt", offset: 1, limit: -2 }),
		),
		scenario(
			"fractional offsets and limits truncate toward zero",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 2.9, limit: 1.9 } },
			await call(env, { path: "five.txt", offset: 2.9, limit: 1.9 }),
		),
		scenario(
			"a limit past the end reports the remaining-lines continuation",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 4, limit: 100 } },
			await call(env, { path: "five.txt", offset: 4, limit: 100 }),
		),
		scenario(
			"a limit ending exactly at the last line reports nothing further",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 4, limit: 2 } },
			await call(env, { path: "five.txt", offset: 4, limit: 2 }),
		),
		scenario(
			"an offset at the newline-past line reads empty rather than failing",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 6 } },
			await call(env, { path: "five.txt", offset: 6 }),
		),
		scenario(
			"an offset beyond the newline-past line fails with the frozen message",
			{ file: "five.txt", content, args: { path: "five.txt", offset: 7 } },
			await call(env, { path: "five.txt", offset: 7 }),
		),
	);
}

// Default head truncation at the line and byte bounds.
{
	const root = path.join(testRoot, "truncation");
	const env = new NodeExecutionEnv({ cwd: root });
	const many = Array.from({ length: 2500 }, (_, i) => `line ${i + 1}`).join("\n") + "\n";
	write(root, "many-lines.txt", many);
	const wide = Array.from({ length: 100 }, () => "x".repeat(600)).join("\n") + "\n";
	write(root, "wide-lines.txt", wide);
	scenarios.push(
		scenario(
			"the default read stops at 2000 lines and reports continuation",
			{ file: "many-lines.txt", content: many, lines: 2500, args: { path: "many-lines.txt" } },
			await call(env, { path: "many-lines.txt" }),
			["the 2500-line file is described by its line count; its text is the captured output itself"],
		),
		scenario(
			"the default read stops at 50KB when bytes bind first",
			{ file: "wide-lines.txt", content: wide, lines: 100, bytes: wide.length, args: { path: "wide-lines.txt" } },
			await call(env, { path: "wide-lines.txt" }),
			["the 60KB file is described by its line and byte counts; its text is the captured output itself"],
		),
	);
}

// First-line-over-limit and byte-order-mark handling.
{
	const root = path.join(testRoot, "head");
	const env = new NodeExecutionEnv({ cwd: root });
	const single = "y".repeat(60 * 1024);
	write(root, "single-line.txt", single);
	// A three-byte character straddles the 50KB cut, so the head ends on the
	// last character boundary at or before 51200 bytes.
	const multibyte = "€".repeat(20000);
	write(root, "multibyte-line.txt", multibyte);
	const bom = new Uint8Array([0xef, 0xbb, 0xbf, ...Buffer.from("bommed\nplain\n", "utf8")]);
	write(root, "bom.txt", bom);
	scenarios.push(
		scenario(
			"a single line over the byte limit shows its head with a warning",
			{ file: "single-line.txt", content: single, bytes: single.length, args: { path: "single-line.txt" } },
			await call(env, { path: "single-line.txt" }),
		),
		scenario(
			"the head cut lands on a character boundary",
			{ file: "multibyte-line.txt", content: multibyte, bytes: multibyte.length, args: { path: "multibyte-line.txt" } },
			await call(env, { path: "multibyte-line.txt" }),
		),
		scenario(
			"a byte-order mark is stripped and not counted",
			{ file: "bom.txt", content: "\ufeffbommed\nplain\n", args: { path: "bom.txt" } },
			await call(env, { path: "bom.txt" }),
		),
	);
}

// A selection that starts past the head window: offset/limit interact with
// the default bounds on the selected range only.
{
	const root = path.join(testRoot, "selected");
	const env = new NodeExecutionEnv({ cwd: root });
	const many = Array.from({ length: 2500 }, (_, i) => `line ${i + 1}`).join("\n") + "\n";
	write(root, "many.txt", many);
	const wide = Array.from({ length: 100 }, () => "x".repeat(600)).join("\n") + "\n";
	write(root, "wide.txt", wide);
	scenarios.push(
		scenario(
			"offset and limit bound the selection before the default limits",
			{ file: "many.txt", content: many, lines: 2500, args: { path: "many.txt", offset: 100, limit: 50 } },
			await call(env, { path: "many.txt", offset: 100, limit: 50 }),
			["the 2500-line file is described by its line count; its text is the captured output itself"],
		),
		scenario(
			"the default limits apply to a selected range that exceeds them",
			{ file: "many.txt", content: many, lines: 2500, args: { path: "many.txt", offset: 2000 } },
			await call(env, { path: "many.txt", offset: 2000 }),
			["the 2500-line file is described by its line count; its text is the captured output itself"],
		),
		scenario(
			"the default byte limit applies to a selected range that exceeds it",
			{ file: "wide.txt", content: wide, lines: 100, bytes: wide.length, args: { path: "wide.txt", offset: 10 } },
			await call(env, { path: "wide.txt", offset: 10 }),
			["the 60KB file is described by its line and byte counts; its text is the captured output itself"],
		),
	);
}

const readToolEndpoints = [
	"packages/durable/src/tools/read.ts:readSchema,readText,readHead",
	"packages/durable/src/truncate.ts:DEFAULT_MAX_LINES,DEFAULT_MAX_BYTES,truncateHeadOf,formatSize",
	"packages/durable/src/env/line-scan.ts:LineScanner",
	"packages/durable/src/env/node.ts:NodeBinaryReader.scanLines",
];

const artifacts = [
	{
		path: "read-tool.json",
		scenarios,
		sourceEndpoints: readToolEndpoints,
	},
];

const manifestArtifacts = [];
const artifactBytes = new Map<string, Buffer>();
try {
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
} finally {
	rmSync(testRoot, { recursive: true, force: true });
}

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1197",
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

// Write only after every guard and scenario succeeded.
mkdirSync(familyDir, { recursive: true });
for (const [relative, bytes] of artifactBytes) writeFileSync(path.join(familyDir, relative), bytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);

console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts, ${scenarios.length} scenarios) for ${baselineName} (${baseline.revision})`,
);
