#!/usr/bin/env tsx
/**
 * Capture selected keybinding observations for issue #1221 at the frozen
 * pi v1.3.0 checkout.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221-keybindings/capture-1221-keybindings.mts
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
const family = "audit-1221-keybindings";
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
const { KeybindingsManager, TUI_KEYBINDINGS } = await import(source("keybindings.ts"));
const definitions = {
  alpha: { defaultKeys: ["up", "ctrl+b", "up"], description: "Alpha" },
  beta: { defaultKeys: "down", description: "Beta" },
  gamma: { defaultKeys: [], description: "Gamma" },
};
const cases = [
  { name: "defaults-deduplicate", user: {} },
  { name: "overrides-disable-and-conflict", user: { alpha: ["down", "down"], beta: "down", gamma: [] } },
  { name: "unknown-claim-and-default-overlap", user: { alpha: "down", unknown: "down" } },
];
const scenarios = cases.map(({name, user}) => {
  const manager = new KeybindingsManager(definitions, user);
  const sequences = ["\x1b[A", "\x1b[B", "\x02", "x"];
  return { name, inputs: {definitions, user, sequences}, expected: {
    resolved: manager.getResolvedBindings(), conflicts: manager.getConflicts(),
    matches: Object.fromEntries(Object.keys(definitions).map(id => [id, sequences.map(data => manager.matches(data, id))])),
  }, normalizations: [] };
});
const changing = new KeybindingsManager(definitions, {alpha: "down", beta: "down"});
const before = {resolved: changing.getResolvedBindings(), conflicts: changing.getConflicts()};
changing.setUserBindings({alpha: "ctrl+b"});
scenarios.push({name: "replace-user-bindings", inputs: {definitions, initial: {alpha: "down", beta: "down"}, replacement: {alpha: "ctrl+b"}}, expected: {before, after: {resolved: changing.getResolvedBindings(), conflicts: changing.getConflicts()}}, normalizations: []} as any);
scenarios.push({name: "builtin-definitions", inputs: {table: "TUI_KEYBINDINGS"}, expected: TUI_KEYBINDINGS, normalizations: []} as any);
const artifacts = [{path: "keybindings.json", sourceEndpoints: ["packages/tui/src/keybindings.ts:KeybindingsManager,TUI_KEYBINDINGS"], scenarios}];
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
