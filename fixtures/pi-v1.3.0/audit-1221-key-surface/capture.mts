#!/usr/bin/env tsx
/**
 * Capture the complete pi-v1.3.0 key surface for issue #1221 route batch B13:
 * the `Key` constant/helper table, `decodeKittyPrintable` accept/reject classes,
 * and the raw `isKeyRelease`/`isKeyRepeat` suffix predicates.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221-key-surface/capture.mts
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
const family = "audit-1221-key-surface";
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
	arch: process.arch,
	libc: "glibc",
	glibcVersion: glibc,
	node: process.version,
	tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();
const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");

const source = (file: string) => pathToFileURL(path.join(piCheckout, "packages/tui/src", file)).href;
const keys = await import(source("keys.ts"));

// 1. Every string entry of `Key` plus every modifier helper applied to "x".
const baseKey = "x";
const constants: Record<string, string> = {};
const helpers: Record<string, string> = {};
for (const [name, value] of Object.entries<any>(keys.Key)) {
	if (typeof value === "string") constants[name] = value;
	else if (typeof value === "function") helpers[name] = value(baseKey);
}
const keyConstants = {
	name: "key-constants-and-modifier-helpers",
	inputs: { baseKey, modifierHelpers: Object.keys(helpers) },
	expected: { constants, helpers },
	normalizations: [],
};

// 2. `decodeKittyPrintable` across the accepted and rejected classes.
const printableCases = [
	{ name: "plain", sequence: "\x1b[97u" },
	{ name: "shifted-key", sequence: "\x1b[97:65;2u" },
	{ name: "keypad-normalized", sequence: "\x1b[57413u" },
	{ name: "astral", sequence: "\x1b[128512u" },
	{ name: "ctrl", sequence: "\x1b[97;5u" },
	{ name: "alt", sequence: "\x1b[97;3u" },
	{ name: "super", sequence: "\x1b[97;9u" },
	{ name: "release", sequence: "\x1b[97;1:3u" },
	{ name: "control-codepoint", sequence: "\x1b[13u" },
	{ name: "non-csi-u-text", sequence: "a" },
	{ name: "non-csi-u-modify-other-keys", sequence: "\x1b[27;2;97~" },
];
const kittyText = {
	name: "kitty-printable-text-decoding",
	inputs: { cases: printableCases },
	expected: {
		cases: printableCases.map(({ name, sequence }) => ({
			name,
			text: keys.decodeKittyPrintable(sequence) ?? null,
		})),
	},
	normalizations: [],
};

// 3. The raw release/repeat suffix predicates, including the paste exclusion.
const suffixes = ["u", "~", "A", "B", "C", "D", "E", "H", "F"];
const suffixSequence = (eventType: number, suffix: string) => {
	if (suffix === "u") return `\x1b[97;1:${eventType}u`;
	if (suffix === "~") return `\x1b[3;1:${eventType}~`;
	return `\x1b[1;1:${eventType}${suffix}`;
};
const predicateCases: Array<{ name: string; data: string }> = [];
for (const suffix of suffixes) {
	predicateCases.push({ name: `release-${suffix}`, data: suffixSequence(3, suffix) });
}
for (const suffix of suffixes) {
	predicateCases.push({ name: `repeat-${suffix}`, data: suffixSequence(2, suffix) });
}
predicateCases.push(
	{ name: "plain-letter", data: "a" },
	{ name: "kitty-press", data: "\x1b[97u" },
	{ name: "kitty-ctrl-press", data: "\x1b[97;5u" },
	{ name: "explicit-press", data: "\x1b[97;1:1u" },
	{ name: "paste-with-release-suffix", data: "\x1b[200~90:62:3F:A5\x1b[201~" },
	{ name: "paste-with-repeat-suffix", data: "\x1b[200~:2F\x1b[201~" },
);
const releaseRepeat = {
	name: "key-release-and-repeat-raw-predicates",
	inputs: { cases: predicateCases },
	expected: {
		cases: predicateCases.map(({ name, data }) => ({
			name,
			release: keys.isKeyRelease(data),
			repeat: keys.isKeyRepeat(data),
		})),
	},
	normalizations: [],
};

const artifacts = [
	{
		path: "key-constants.json",
		sourceEndpoints: ["packages/tui/src/keys.ts:Key"],
		scenarios: [keyConstants],
	},
	{
		path: "kitty-text.json",
		sourceEndpoints: ["packages/tui/src/keys.ts:decodeKittyPrintable"],
		scenarios: [kittyText],
	},
	{
		path: "release-repeat.json",
		sourceEndpoints: ["packages/tui/src/keys.ts:isKeyRelease,isKeyRepeat"],
		scenarios: [releaseRepeat],
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
const scenarioSources: Array<{ path: string; sha256: string }> = [];

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
		sources: [{ path: generatorPath, sha256: sha256(readFileSync(scriptPath)) }, ...scenarioSources],
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
