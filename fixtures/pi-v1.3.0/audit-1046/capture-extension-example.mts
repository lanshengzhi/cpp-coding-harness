#!/usr/bin/env tsx
/**
 * Capture the `audit-1046` extension example for the shared pi-v1.3.0 evidence registry (#1046).
 *
 * This is the worked example every audit lane (#1047-#1057) extends: one reproducible public-call
 * scenario set against frozen pi, captured with the named baseline, a full-revision guard, a
 * clean-source guard and a bundle-local destination. The observations are not a toolkit audit
 * claim; they demonstrate the envelope, provenance and scenario conventions documented in
 * `fixtures/pi-v1.3.0/README.md`. Toolkit rows remain owned by their audit tickets.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1046/capture-extension-example.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace `paths` resolve.
 * It refuses a checkout whose HEAD is not the registered baseline revision, a modified source
 * tree, a non-Linux/glibc host, and a family already registered to another baseline.
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
const family = "audit-1046";
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

// The package entry point is the public surface: `fuzzyMatch`/`fuzzyFilter` are re-exported there.
const tui = await import(pathToFileURL(path.join(piCheckout, "packages/tui/src/index.ts")).href);
const sourceEndpoints = [
	"packages/tui/src/fuzzy.ts:fuzzyMatch,fuzzyFilter",
	"packages/tui/test/fuzzy.test.ts:fuzzyMatch,fuzzyFilter",
];
const upstreamTest = (testCase: string) => ({ path: "packages/tui/test/fuzzy.test.ts", case: testCase });
const scenarios = [
	{
		name: "empty query matches everything with score zero",
		inputs: { query: "", text: "anything" },
		expected: tui.fuzzyMatch("", "anything"),
		normalizations: [],
		upstreamTest: upstreamTest("empty query matches everything with score 0"),
	},
	{
		name: "word boundary scores better than interior match",
		inputs: { query: "fb", texts: { boundary: "foo-bar", interior: "afbx" } },
		expected: {
			boundary: tui.fuzzyMatch("fb", "foo-bar"),
			interior: tui.fuzzyMatch("fb", "afbx"),
		},
		normalizations: [],
		upstreamTest: upstreamTest("word boundary matches score better"),
	},
	{
		name: "filter sorts results by match quality",
		inputs: { query: "app", items: ["a_p_p", "app", "application"] },
		expected: {
			order: tui.fuzzyFilter(["a_p_p", "app", "application"], "app", (item: string) => item),
		},
		normalizations: [],
		upstreamTest: upstreamTest("sorts results by match quality"),
	},
];

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const artifactName = "fuzzy-match.json";
const artifact = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	capturedAt,
	environment,
	sourceEndpoints,
	scenarios,
};
const artifactBytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1046",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=${piCheckout} PI_BASELINE=${baselineName} ${path.join(piCheckout, "node_modules/.bin/tsx")} ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
	},
	artifacts: [
		{
			path: artifactName,
			family,
			bytes: artifactBytes.length,
			sha256: sha256(artifactBytes),
			sourceEndpoints,
			scenarios,
		},
	],
};

// Write only after every guard and observation succeeded.
mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, artifactName), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} artifact ${artifactName} for ${baselineName} (${baseline.revision})`);
