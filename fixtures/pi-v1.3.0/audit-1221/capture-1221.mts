#!/usr/bin/env tsx
/**
 * Capture selected route batch B13 observations for issue #1221: keyboard
 * identity and fragmented input at the frozen pi v1.3.0 checkout.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221/capture-1221.mts
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
const family = "audit-1221";
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
const { StdinBuffer } = await import(source("stdin-buffer.ts"));
const scenarios: any[] = [];
const cases = [
  { sequence: "a", bindings: ["a", "shift+a"] },
  { sequence: "!", bindings: ["!", "shift+1"] },
  { sequence: "\x1b[49:33;2u", bindings: ["!", "shift+1", "shift+!"] },
  { sequence: "\x1b[97;5:2u", bindings: ["ctrl+a", "a"] },
  { sequence: "\x1b[97;5:3u", bindings: ["ctrl+a", "a"] },
];
for (const active of [false, true]) {
  keys.setKittyProtocolActive(active);
  scenarios.push({name: active ? "kitty-active-key-identity" : "legacy-key-identity", inputs: {active, cases},
    expected: cases.map(({sequence, bindings}) => ({parsed: keys.parseKey(sequence) ?? null,
      repeat: keys.isKeyRepeat(sequence), release: keys.isKeyRelease(sequence),
      matches: bindings.map(binding => keys.matchesKey(sequence, binding))})), normalizations: []});
}
keys.setKittyProtocolActive(false);
const frames = [
  {name: "fragmented-osc-reply", chunks: ["\x1b]10;rgb:ff/", "00/00\x07", "x"]},
  {name: "fragmented-apc-reply", chunks: ["\x1b_Gi=1;", "OK\x1b\\", "x"]},
  {name: "ambiguous-alt-input", chunks: ["\x1b]hello"]},
];
const framed = [];
for (const frame of frames) {
  const buffer = new StdinBuffer({timeout: 15, escapeTimeout: 10});
  const data: string[] = [];
  buffer.on("data", (item: string) => data.push(item));
  buffer.process(frame.chunks[0]);
  await new Promise(resolve => setTimeout(resolve, 45));
  const afterWait = [...data];
  for (const chunk of frame.chunks.slice(1)) buffer.process(chunk);
  const afterCompletion = [...data];
  buffer.destroy();
  framed.push({name: frame.name, inputs: {chunks: frame.chunks, timeoutMs: 15, waitMs: 45},
    expected: {afterWait, afterCompletion}, normalizations: []});
}
const artifacts = [
  {path: "keys-kitty-text.json", sourceEndpoints: ["packages/tui/src/keys.ts:parseKey,matchesKey,isKeyRepeat,isKeyRelease,setKittyProtocolActive"], scenarios},
  {path: "input.json", sourceEndpoints: ["packages/tui/src/stdin-buffer.ts:StdinBuffer.process,StdinBuffer.destroy"], scenarios: framed},
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
const scenarioSources: Array<{path: string; sha256: string}> = [];

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
