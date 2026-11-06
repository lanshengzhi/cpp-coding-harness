#!/usr/bin/env tsx
/**
 * Capture selected keybinding observations for issue #1221 at the frozen
 * pi v1.3.0 checkout.
 * Every value below comes from executing the frozen checkout's own modules
 * through their public seams; nothing is derived from C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1221-manager-alias/capture-1221-manager-alias.mts
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
const family = "audit-1221-manager-alias";
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
const { KeybindingsManager } = await import(source("keybindings.ts"));
const definitions = {
  alpha: { defaultKeys: ["up", "ctrl+b", "up"], description: "Alpha" },
  beta: { defaultKeys: "down", description: "Beta" },
  gamma: { defaultKeys: [], description: "Gamma" },
};
const initial = {alpha: ["down", "down"], beta: "down"};
const initialInput = structuredClone(initial);
const manager = new KeybindingsManager(definitions, initial);
const snapshot = () => structuredClone({
  user: manager.getUserBindings(), resolved: manager.getResolvedBindings(),
  definition: manager.getDefinition("alpha"), conflicts: manager.getConflicts(),
});
const before = snapshot();
const returned = manager.getUserBindings();
returned.alpha.push("left");
returned.beta = "up";
const afterMutation = snapshot();
manager.setUserBindings(manager.getUserBindings());
const afterRebuild = snapshot();
const conflictCopy = manager.getConflicts();
conflictCopy[0].keybindings.push("unknown");
const afterConflictMutation = snapshot();
const literalDefinitions = {
  first: { defaultKeys: [] }, second: { defaultKeys: [] }, third: { defaultKeys: [] },
};
const literalBindings = {
  first: ["ctrl+shift+up", "ctrl+shift+up"], second: "shift+ctrl+up", third: "ctrl+shift+up",
};
const literalManager = new KeybindingsManager(literalDefinitions, literalBindings);
const scenarios = [
  {name: "nested-user-array-and-conflict-copy", inputs: {definitions, initial: initialInput},
    expected: {before, afterMutation, afterRebuild, afterConflictMutation}, normalizations: []},
  {name: "literal-conflict-spellings", inputs: {definitions: literalDefinitions, bindings: literalBindings},
    expected: {conflicts: literalManager.getConflicts(), resolved: literalManager.getResolvedBindings()}, normalizations: []},
];
const artifacts = [{path: "manager-alias.json", sourceEndpoints: ["packages/tui/src/keybindings.ts:KeybindingsManager"], scenarios}];
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
