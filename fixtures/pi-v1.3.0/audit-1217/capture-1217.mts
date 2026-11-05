#!/usr/bin/env tsx
/**
 * Capture the `audit-1217` frozen observations: route batch B9 (issue
 * #1217, Markdown, tables and math rendering) recaptured at the frozen
 * pi v1.3.0 checkout. Every value below comes from executing the frozen
 * checkout's own modules through their public seams; nothing is derived from
 * C++.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1217/capture-1217.mts
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
const family = "audit-1217";
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

const captureDir = path.join(repositoryRoot, "fixtures/pi-tui/capture");
const { captureMarkdownScenarios } = await import(
	pathToFileURL(path.join(captureDir, "markdown-scenarios.mts")).href
);

const artifacts: Array<{ path: string; sourceEndpoints: string[]; scenarios: any[] }> = [];

// 1. Family: markdown.json (Markdown headings, lists, tables, CJK, code blocks, quotes, links, math, lifecycle)
{
	const observation = await captureMarkdownScenarios(piCheckout);
	artifacts.push({
		path: "markdown.json",
		sourceEndpoints: observation.sourceEndpoints,
		scenarios: observation.scenarios,
	});
}

// 2. Family: latex.json (re-capture of renderLatex and RenderLatexOptions at pi-v1.3.0)
{
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { renderLatex } = await import(source("src/latex.ts"));

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

	const observeLatex = (sourceStr: string, options?: { display?: boolean }) => {
		const output = renderLatex(sourceStr, options);
		return output === undefined ? null : output;
	};
	const latexRows = (rows: string[][]) => rows.map(([name, sourceStr]) => ({ name, source: sourceStr }));

	const latexScenarios = [
		{
			name: "inline-grammar",
			dimensions: { columns: 40, rows: 8 },
			inputs: { inline: latexRows(latexInline) },
			expected: { inline: latexRows(latexInline).map((row) => ({ ...row, output: observeLatex(row.source) })) },
		},
		{
			name: "inline-failure-value",
			dimensions: { columns: 40, rows: 8 },
			inputs: { failing: latexRows(latexFailing) },
			expected: { failing: latexRows(latexFailing).map((row) => ({ ...row, output: observeLatex(row.source) })) },
		},
		{
			name: "display-option",
			dimensions: { columns: 40, rows: 8 },
			inputs: { display: latexRows(latexDisplay) },
			expected: {
				display: latexRows(latexDisplay).map((row) => ({
					...row,
					output: observeLatex(row.source, { display: true }),
				})),
			},
		},
	];

	artifacts.push({
		path: "latex.json",
		sourceEndpoints: ["packages/tui/src/latex.ts:renderLatex,RenderLatexOptions"],
		scenarios: latexScenarios,
	});
}

// ---------------------------------------------------------------------------
// Write the artifacts and the manifest only after every scenario succeeded.
// ---------------------------------------------------------------------------
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
const scenarioSources = ["markdown-scenarios.mts"].map((name) => {
	const relative = `fixtures/pi-tui/capture/${name}`;
	return { path: relative, sha256: sha256(readFileSync(path.join(repositoryRoot, relative))) };
});
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1217",
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
