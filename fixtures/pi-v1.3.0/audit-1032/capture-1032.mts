#!/usr/bin/env tsx
/**
 * Capture the `audit-1032` generated `system` theme for the shared pi-v1.3.0
 * evidence registry (#1032, B74 R1032).
 *
 * Observes the frozen `generateSystemThemeColors` public generator (and its
 * `terminalAppearance`/`relativeLuminance`/`wcagContrast` exports) against
 * fixed reported-color samples: a dark and a light terminal with a full
 * 16-slot palette (tier 1, palette-anchored hues), the pending grayscale
 * state at zero saturation, background-only reports (tier 2, family hues),
 * and the no-report indexed tier (tier 3, ANSI slots plus the faint `dim`
 * set) in both the pending and the settled states and both appearance hints.
 * Every scenario records the complete 56-token color map (`#rrggbb`, an ANSI
 * palette index, or `""` for the terminal default), the `dim` token list and
 * the generated appearance; scenarios with a reported background also record
 * the body-text WCAG contrast of `text`, `userMessageText` and `toolTitle`
 * on the surfaces the frozen RULES draw them on (minimum 4.5:1 by
 * construction of the frozen `withTextContrast` pass).
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1032/capture-1032.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace
 * `paths` resolve. It refuses a checkout whose HEAD is not the registered
 * baseline revision, a modified source tree, a non-Linux/glibc host, and a
 * family already registered to another baseline.
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
const family = "audit-1032";
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

const source = (relative: string) => pathToFileURL(path.join(piCheckout, relative)).href;
const systemTheme = await import(
	source("packages/coding-agent/src/modes/interactive/theme/system-theme.ts")
);
const { generateSystemThemeColors, relativeLuminance, wcagContrast } = systemTheme;

const sourceEndpoints = [
	"packages/coding-agent/src/modes/interactive/theme/system-theme.ts:generateSystemThemeColors,terminalAppearance,relativeLuminance,wcagContrast",
	"packages/coding-agent/src/modes/interactive/theme/theme.ts:createSystemTheme,loadTheme,getTerminalTheme",
];

type Rgb = { r: number; g: number; b: number };
const rgbOf = (hex: string): Rgb => ({
	r: Number.parseInt(hex.slice(1, 3), 16),
	g: Number.parseInt(hex.slice(3, 5), 16),
	b: Number.parseInt(hex.slice(5, 7), 16),
});

// Fixed reported palettes: classic xterm defaults (dark sample) and Solarized
// Light (light sample). Recorded verbatim in every scenario's inputs.
const darkPalette = [
	"#000000",
	"#cd0000",
	"#00cd00",
	"#cdcd00",
	"#0000ee",
	"#cd00cd",
	"#00cdcd",
	"#e5e5e5",
	"#7f7f7f",
	"#ff0000",
	"#00ff00",
	"#ffff00",
	"#5c5cff",
	"#ff00ff",
	"#00ffff",
	"#ffffff",
];
const lightPalette = [
	"#eee8d5",
	"#dc322f",
	"#859900",
	"#b58900",
	"#268bd2",
	"#d33682",
	"#2aa198",
	"#073642",
	"#fdf6e3",
	"#cb4b16",
	"#93a1a1",
	"#839496",
	"#657b83",
	"#6c71c4",
	"#586e75",
	"#002b36",
];

// The surfaces the frozen RULES draw each body-text token on
// (system-theme.ts RULES); used only to record the contrast observation.
const bodyTextSurfaces: Record<string, string[]> = {
	text: ["background", "selectedBg"],
	userMessageText: ["userMessageBg"],
	toolTitle: ["toolPendingBg", "toolSuccessBg", "toolErrorBg"],
};

interface Sample {
	foreground?: string;
	background?: string;
	palette?: string[];
	saturation: number;
	appearanceHint?: "dark" | "light";
}

const observe = (sample: Sample) => {
	const input: Record<string, unknown> = { saturation: sample.saturation };
	if (sample.foreground !== undefined) input.foreground = rgbOf(sample.foreground);
	if (sample.background !== undefined) input.background = rgbOf(sample.background);
	if (sample.palette !== undefined) input.palette = sample.palette.map(rgbOf);
	if (sample.appearanceHint !== undefined) input.appearanceHint = sample.appearanceHint;
	const generated = generateSystemThemeColors(input);
	const effective = (value: string | number): Rgb | undefined => {
		if (typeof value === "number") return undefined;
		if (value === "") return sample.foreground !== undefined ? rgbOf(sample.foreground) : undefined;
		return rgbOf(value);
	};
	const surfaceColor = (surface: string): Rgb | undefined => {
		if (surface === "background") {
			return sample.background !== undefined ? rgbOf(sample.background) : undefined;
		}
		const value = generated.colors[surface as keyof typeof generated.colors];
		if (typeof value === "number") return undefined;
		if (value === "") return sample.background !== undefined ? rgbOf(sample.background) : undefined;
		return rgbOf(value);
	};
	const textContrast: Record<string, unknown> = {};
	if (sample.background !== undefined) {
		for (const [token, surfaces] of Object.entries(bodyTextSurfaces)) {
			const color = effective(generated.colors[token as keyof typeof generated.colors]);
			if (!color) continue;
			const ratios: Record<string, number> = {};
			for (const surface of surfaces) {
				const on = surfaceColor(surface);
				if (on) ratios[surface] = wcagContrast(color, on);
			}
			textContrast[token] = { surfaces: ratios, minimum: Math.min(...Object.values(ratios)) };
		}
	}
	return {
		appearance: generated.appearance ?? null,
		colors: generated.colors,
		dim: generated.dim,
		...(Object.keys(textContrast).length > 0 ? { textContrast } : {}),
	};
};

const scenarioInputs = (sample: Sample) => {
	const inputs: Record<string, unknown> = { saturation: sample.saturation };
	if (sample.foreground !== undefined) inputs.foreground = sample.foreground;
	if (sample.background !== undefined) inputs.background = sample.background;
	if (sample.palette !== undefined) inputs.palette = sample.palette;
	if (sample.appearanceHint !== undefined) inputs.appearanceHint = sample.appearanceHint;
	return inputs;
};

const darkSample: Sample = {
	foreground: "#e5e5e7",
	background: "#000000",
	palette: darkPalette,
	saturation: 1,
};
const lightSample: Sample = {
	foreground: "#000000",
	background: "#ffffff",
	palette: lightPalette,
	saturation: 1,
};

const scenarios = [
	{
		name: "dark terminal full palette anchors hues to the reported colors",
		inputs: scenarioInputs(darkSample),
		expected: observe(darkSample),
		normalizations: [],
	},
	{
		name: "light terminal full palette anchors hues to the reported colors",
		inputs: scenarioInputs(lightSample),
		expected: observe(lightSample),
		normalizations: [],
	},
	{
		name: "pending state renders grayscale at zero saturation",
		inputs: scenarioInputs({ ...darkSample, saturation: 0 }),
		expected: observe({ ...darkSample, saturation: 0 }),
		normalizations: [],
	},
	{
		name: "dark terminal without palette uses family hues",
		inputs: scenarioInputs({ foreground: "#e5e5e7", background: "#000000", saturation: 1 }),
		expected: observe({ foreground: "#e5e5e7", background: "#000000", saturation: 1 }),
		normalizations: [],
	},
	{
		name: "light terminal without palette uses family hues",
		inputs: scenarioInputs({ foreground: "#000000", background: "#ffffff", saturation: 1 }),
		expected: observe({ foreground: "#000000", background: "#ffffff", saturation: 1 }),
		normalizations: [],
	},
	{
		name: "no report pending renders indexed grayscale with faint neutrals",
		inputs: scenarioInputs({ saturation: 0, appearanceHint: "dark" }),
		expected: observe({ saturation: 0, appearanceHint: "dark" }),
		normalizations: [],
	},
	{
		name: "no report settled renders indexed palette slots",
		inputs: scenarioInputs({ saturation: 1, appearanceHint: "dark" }),
		expected: observe({ saturation: 1, appearanceHint: "dark" }),
		normalizations: [],
	},
	{
		name: "no report pending light hint renders indexed grayscale",
		inputs: scenarioInputs({ saturation: 0, appearanceHint: "light" }),
		expected: observe({ saturation: 0, appearanceHint: "light" }),
		normalizations: [],
	},
	{
		name: "no report settled light hint renders indexed palette slots",
		inputs: scenarioInputs({ saturation: 1, appearanceHint: "light" }),
		expected: observe({ saturation: 1, appearanceHint: "light" }),
		normalizations: [],
	},
];

// The luminance helpers are part of the observed surface; reference them so a
// broken export fails the capture instead of silently recording nothing.
if (relativeLuminance({ r: 0, g: 0, b: 0 }) !== 0) {
	throw new Error("relativeLuminance(0,0,0) must be 0");
}

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const artifactName = "system-theme-capture.json";
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
	ticket: "1032",
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
