#!/usr/bin/env tsx
/**
 * Capture the `audit-1175` abandon-on-restart observations for the shared
 * pi-v1.3.0 evidence registry (#1175).
 *
 * The registered `audit-1055` family already captures one reopen of a flagged
 * task. This family adds the observations that slice needs to consume
 * `TaskOptions.abandonOnRestart` rather than expose it: the paused open state,
 * the `restart` reason an abort handler observes, propagation to ordinary owned
 * work, background and unflagged boundaries, a missing definition that waits
 * instead of orphaning, definition restoration, and an explicit request that
 * replaces the reason down the tree.
 *
 * The capture refuses a checkout whose HEAD is not the registered baseline
 * revision, a modified source tree, a non-Linux/glibc host, and a family already
 * registered to another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1175/capture-1175.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace
 * `paths` resolve.
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1175";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi");

if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
		stdio: "inherit",
		env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
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

const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
	tsxVersion = requirePi("tsx/package.json").version;
} catch {
	// The tsx version is provenance, not a capture precondition; keep the sentinel explicit.
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

const scenariosModule = await import(
	pathToFileURL(path.join(familyDir, "scenarios.mts")).href
);
const scenarios = [
	await scenariosModule.openOnlyVersusFirstReservation(),
	await scenariosModule.restartCascadeBoundaries(),
	await scenariosModule.missingDefinitionVersusExplicitAbort(),
	await scenariosModule.explicitAbortReplacesRestartDownTheTree(),
	await scenariosModule.flaggedTaskCreatedAfterOpenRuns(),
];

const sourceEndpoints = [
	"packages/durable/src/harness/scheduler.ts:TaskScheduler.open,#abandoned,#reserve,#reconcile,abort,#walkDown,withAbortMark",
	"packages/durable/src/types.ts:TaskOptions.abandonOnRestart,TaskRecord.abandonOnRestart,TaskRecord.abortReason",
	"packages/durable/src/session/transaction.ts:Transaction.createTask",
	"packages/durable/test/harness-nested-tools-restart.test.ts:marks nothing when the reopened Harness never starts scheduling",
	"packages/durable/test/harness-nested-tools-restart.test.ts:leaves background work under an abandoned nested call running, and settles without it",
	"packages/durable/test/harness-nested-tools-restart.test.ts:keeps an abandoned task whose definition is missing waiting until it is installed, then runs its abort",
	"packages/durable/test/harness-cancellation-barrier.test.ts:upgrades restart marks down the tree when an abort is requested above them",
];

const artifactPath = "abandon-on-restart.json";
const artifactDocument = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	capturedAt,
	environment,
	sourceEndpoints,
	scenarios,
};
const artifactBytes = Buffer.from(`${JSON.stringify(artifactDocument, null, 2)}\n`);

const manifestArtifacts = [
	{
		path: artifactPath,
		family,
		bytes: artifactBytes.length,
		sha256: sha256(artifactBytes),
		sourceEndpoints,
		scenarios,
	},
];

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorSources = ["capture-1175.mts", "scenarios.mts", "pi.mts"].map((name) => {
	const relative = path
		.relative(repositoryRoot, path.join(familyDir, name))
		.split(path.sep)
		.join("/");
	return { path: relative, sha256: sha256(readFileSync(path.join(familyDir, name))) };
});
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1175",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(scriptPath)),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: generatorSources,
	},
	artifacts: manifestArtifacts,
};

// Write only after every guard and scenario succeeded.
mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, artifactPath), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(
	`captured ${family} (${manifestArtifacts.length} artifact, ${scenarios.length} scenarios) for ` +
		`${baselineName} (${baseline.revision})`,
);
