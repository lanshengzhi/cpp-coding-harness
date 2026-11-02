#!/usr/bin/env tsx
/**
 * Capture the `audit-1115` settings-diagnostic observations for the shared pi-v1.3.0
 * evidence registry (#1115).
 *
 * The registered `audit-1049/configuration-schema.json` family already captures the
 * settings schema's accepted/rejected shapes, the settings manager's consumption and
 * reload, keybinding migration/retention/reload and the 56-token theme set. It does not
 * capture what pi prints when a `settings.json` file cannot be loaded, so the C++
 * diagnostic text had no frozen comparison target. This family adds exactly that:
 *
 * - the `collectSettingsDiagnostics` message template for a malformed settings file,
 *   separated into its stable prefix and the parser-supplied tail;
 * - the project-scope path and the trust gate around that read;
 * - `deduplicateDiagnostics` collapsing the same file error reported twice;
 * - the `SettingsSchema` error paths/messages for the two rejected shapes;
 * - the excluded upstream behavior: a schema-rejected but JSON-valid file loads with no
 *   diagnostic and the runtime keeps the raw value.
 *
 * The capture refuses a checkout whose HEAD is not the registered baseline revision, a
 * modified source tree, a non-Linux/glibc host, and a family already registered to
 * another baseline.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1115/capture-1115.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace `paths` resolve.
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1115";
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

/** Drop inherited pi state so the capture cannot read this session's agent directory. */
for (const key of Object.keys(process.env)) {
	if (key.startsWith("PI_")) delete process.env[key];
}

const importPi = (relativePath: string) => import(pathToFileURL(path.join(piCheckout, relativePath)).href);
const { SettingsManager } = await importPi("packages/coding-agent/src/core/settings-manager.ts");
const { collectSettingsDiagnostics, deduplicateDiagnostics } = await importPi(
	"packages/coding-agent/src/core/settings-diagnostics.ts",
);
const { SettingsSchema } = await importPi("packages/coding-agent/src/core/settings-schema.ts");
const { Compile } = requirePi("typebox/compile");
const settingsSchema = Compile(SettingsSchema);

const root = mkdtempSync(path.join(tmpdir(), "audit-1115-"));
const workspace = path.join(root, "workspace");
const agentDir = path.join(root, "agent");
const homeDir = path.join(root, "home");
process.env.HOME = homeDir;
process.env.XDG_CONFIG_HOME = path.join(homeDir, ".config");
process.env.XDG_DATA_HOME = path.join(homeDir, ".local", "share");
process.env.XDG_STATE_HOME = path.join(homeDir, ".local", "state");
process.env.XDG_CACHE_HOME = path.join(homeDir, ".cache");
for (const directory of [workspace, agentDir, homeDir]) {
	mkdirSync(directory, { recursive: true });
}

const writeJson = (target: string, value: any) => {
	writeFileSync(target, `${JSON.stringify(value, null, 2)}\n`);
};

/** Split `Invalid settings file <path>: <message>` into its stable template and parser tail. */
function splitDiagnostic(message: string) {
	const marker = "Invalid settings file ";
	if (!message.startsWith(marker)) {
		return { template: null, path: null, errorMessage: message };
	}
	const separator = message.indexOf(": ");
	if (separator < 0) {
		return { template: null, path: null, errorMessage: message };
	}
	const reportedPath = message.slice(marker.length, separator);
	const errorMessage = message.slice(separator + 2);
	// The tail is the frozen runtime's parser text, not part of the contract; it is
	// recorded as provenance so the C++ comparison pins the template and path only.
	return {
		template: `${marker}<path>: <message>`,
		path: reportedPath,
		errorMessage,
	};
}

const scenarios: any[] = [];

try {
	// ── Malformed global settings file ────────────────────────────────────────────────────────
	{
		const settingsPath = path.join(agentDir, "settings.json");
		writeFileSync(settingsPath, "{not valid json");
		const manager = SettingsManager.create(workspace, agentDir);
		const diagnostics = collectSettingsDiagnostics(manager);
		const observed = {
			diagnosticCount: diagnostics.length,
			diagnosticType: diagnostics[0]?.type ?? null,
			diagnostic: diagnostics[0] ? splitDiagnostic(diagnostics[0].message) : null,
			settingsKeyCount: Object.keys(manager.getSettings() ?? {}).length,
		};
		if (observed.diagnosticCount !== 1 || observed.diagnosticType !== "warning") {
			throw new Error("malformed global settings must produce exactly one warning diagnostic");
		}
		if (observed.diagnostic?.path !== settingsPath) {
			throw new Error(`diagnostic must name the settings path, saw ${JSON.stringify(observed.diagnostic?.path)}`);
		}
		scenarios.push({
			name: "a malformed global settings file yields one warning naming the file path",
			inputs: {
				scope: "global",
				settingsPath: "<agent-dir>/settings.json",
				content: "{not valid json",
			},
			expected: {
				diagnosticCount: 1,
				diagnosticType: "warning",
				diagnosticTemplate: "Invalid settings file <path>: <message>",
				reportedPath: "<agent-dir>/settings.json",
				settingsKeyCount: 0,
			},
			normalizations: ["<path> stands for the reported settings file path"],
			observed,
		});
		rmSync(settingsPath);
	}

	// ── Project scope: trusted read, untrusted skip ───────────────────────────────────────────
	{
		const projectSettingsDir = path.join(workspace, ".pi");
		mkdirSync(projectSettingsDir, { recursive: true });
		const projectSettingsPath = path.join(projectSettingsDir, "settings.json");
		writeFileSync(projectSettingsPath, "{not valid json");

		const trusted = SettingsManager.create(workspace, agentDir, { projectTrusted: true });
		const trustedDiagnostics = collectSettingsDiagnostics(trusted);
		const untrusted = SettingsManager.create(workspace, agentDir, { projectTrusted: false });
		const untrustedDiagnostics = collectSettingsDiagnostics(untrusted);

		const observed = {
			trustedCount: trustedDiagnostics.length,
			trustedDiagnostic: trustedDiagnostics[0] ? splitDiagnostic(trustedDiagnostics[0].message) : null,
			untrustedCount: untrustedDiagnostics.length,
		};
		if (observed.trustedCount !== 1) {
			throw new Error("a trusted project scope must report its malformed file once");
		}
		if (observed.trustedDiagnostic?.path !== projectSettingsPath) {
			throw new Error("the project diagnostic must name the project settings path");
		}
		if (observed.untrustedCount !== 0) {
			throw new Error("an untrusted project scope must not be read, so it reports no diagnostic");
		}
		scenarios.push({
			name: "a malformed project settings file reports its own path only while the project is trusted",
			inputs: {
				projectSettingsPath: "<workspace>/.pi/settings.json",
				content: "{not valid json",
				trusted: true,
				untrusted: false,
			},
			expected: {
				trustedCount: 1,
				trustedDiagnosticTemplate: "Invalid settings file <path>: <message>",
				trustedReportedPath: "<workspace>/.pi/settings.json",
				untrustedCount: 0,
			},
			normalizations: ["<path> stands for the reported settings file path"],
			observed,
		});
		rmSync(projectSettingsPath);
	}

	// ── Deduplication of the same file error ─────────────────────────────────────────────────
	{
		const settingsPath = path.join(agentDir, "settings.json");
		writeFileSync(settingsPath, "{not valid json");
		const startup = collectSettingsDiagnostics(SettingsManager.create(workspace, agentDir));
		const runtime = collectSettingsDiagnostics(SettingsManager.create(workspace, agentDir));
		const combined = deduplicateDiagnostics([...startup, ...runtime]);
		const observed = {
			beforeCount: startup.length + runtime.length,
			afterCount: combined.length,
			sameMessage: combined.length === 1 && combined[0]!.message === startup[0]!.message,
		};
		if (observed.beforeCount !== 2 || observed.afterCount !== 1 || !observed.sameMessage) {
			throw new Error("duplicate settings file diagnostics must collapse to the first occurrence");
		}
		scenarios.push({
			name: "the same settings file diagnostic from two managers deduplicates to one message",
			inputs: { managers: 2, settingsPath: "<agent-dir>/settings.json", content: "{not valid json" },
			expected: { beforeCount: 2, afterCount: 1, sameMessage: true },
			normalizations: [],
			observed,
		});
		rmSync(settingsPath);
	}

	// ── Schema rejection paths and messages ──────────────────────────────────────────────────
	{
		const invalidThinking = { defaultThinkingLevel: "bogus" };
		const negativeReserve = { compaction: { reserveTokens: -1 } };
		// TypeBox reports one `{keyword, schemaPath, instancePath, params, message}`
		// entry per violation; `instancePath` is the dotted JSON pointer the C++
		// schema contract is compared against.
		const summarize = (errors: any[]) =>
			errors.map((error) => ({
				instancePath: String(error.instancePath ?? ""),
				keyword: String(error.keyword ?? ""),
				message: String(error.message ?? ""),
			}));
		const thinkingErrors = summarize(settingsSchema.Errors(invalidThinking) as any[]);
		const reserveErrors = summarize(settingsSchema.Errors(negativeReserve) as any[]);
		const observed = {
			invalidThinkingCheck: settingsSchema.Check(invalidThinking),
			invalidThinkingErrors: thinkingErrors,
			negativeReserveCheck: settingsSchema.Check(negativeReserve),
			negativeReserveErrors: reserveErrors,
			unknownTopLevelCheck: settingsSchema.Check({ futureSetting: true }),
		};
		if (observed.invalidThinkingCheck !== false || observed.negativeReserveCheck !== false) {
			throw new Error("the settings schema must reject both invalid shapes");
		}
		if (!thinkingErrors.some((error) => error.instancePath === "/defaultThinkingLevel")) {
			throw new Error("the thinking-level rejection must name the offending key");
		}
		if (!reserveErrors.some((error) => error.instancePath === "/compaction/reserveTokens")) {
			throw new Error("the reserve rejection must name the offending key");
		}
		if (observed.unknownTopLevelCheck !== true) {
			throw new Error("the settings schema must accept an unknown top-level key");
		}
		scenarios.push({
			name: "the settings schema rejects an invalid thinking level and a negative compaction reserve naming the key",
			inputs: { invalidThinking, negativeReserve, unknownTopLevel: { futureSetting: true }, schema: "SettingsSchema" },
			expected: {
				invalidThinkingCheck: false,
				invalidThinkingInstancePath: "/defaultThinkingLevel",
				invalidThinkingMessage: "must be equal to one of the allowed values",
				negativeReserveCheck: false,
				negativeReserveInstancePath: "/compaction/reserveTokens",
				negativeReserveMessage: "must be >= 0",
				unknownTopLevelCheck: true,
			},
			normalizations: [],
			observed,
		});
	}

	// ── Schema vocabulary for every rejected shape the C++ contract publishes ────────────────
	{
		const cases: Array<{ name: string; document: any }> = [
			{ name: "tuiMode union", document: { tuiMode: "bogus" } },
			{ name: "outputPad literal union", document: { outputPad: 5 } },
			{ name: "hideThinkingBlock boolean", document: { hideThinkingBlock: "yes" } },
			{ name: "reserveTokens integer", document: { compaction: { reserveTokens: "abc" } } },
			{ name: "enabledModels element", document: { enabledModels: [1] } },
			{ name: "compaction object", document: { compaction: "x" } },
			{ name: "wheelScrollLines union", document: { fullscreenWheelScrollLines: true } },
		];
		const observed = cases.map((entry) => {
			const errors = settingsSchema.Errors(entry.document) as any[];
			const last = errors.at(-1);
			return {
				name: entry.name,
				check: settingsSchema.Check(entry.document),
				instancePath: String(last?.instancePath ?? ""),
				message: String(last?.message ?? ""),
			};
		});
		for (const entry of observed) {
			if (entry.check !== false) {
				throw new Error(`${entry.name}: the settings schema must reject this shape`);
			}
		}
		scenarios.push({
			name: "the settings schema names the offending path and pi message for each rejected shape",
			inputs: { documents: cases.map((entry) => entry.document), schema: "SettingsSchema" },
			expected: {
				tuiMode: { instancePath: "/tuiMode", message: "must match a schema in anyOf" },
				outputPad: { instancePath: "/outputPad", message: "must match a schema in anyOf" },
				hideThinkingBlock: { instancePath: "/hideThinkingBlock", message: "must be boolean" },
				reserveTokens: { instancePath: "/compaction/reserveTokens", message: "must be integer" },
				enabledModelsElement: { instancePath: "/enabledModels/0", message: "must be string" },
				compactionObject: { instancePath: "/compaction", message: "must be object" },
				wheelScrollLines: { instancePath: "/fullscreenWheelScrollLines", message: "must match a schema in anyOf" },
			},
			normalizations: [],
			observed,
		});
	}

	// ── The excluded upstream behavior: schema-rejected values still load ────────────────────
	{
		const settingsPath = path.join(agentDir, "settings.json");
		writeJson(settingsPath, { defaultThinkingLevel: "bogus" });
		const manager = SettingsManager.create(workspace, agentDir);
		const diagnostics = collectSettingsDiagnostics(manager);
		const observed = {
			diagnosticCount: diagnostics.length,
			defaultThinkingLevel: manager.getSettings().defaultThinkingLevel ?? null,
			schemaCheck: settingsSchema.Check({ defaultThinkingLevel: "bogus" }),
		};
		if (observed.diagnosticCount !== 0) {
			throw new Error("pi's runtime accepts schema-rejected values without a load diagnostic");
		}
		if (observed.defaultThinkingLevel !== "bogus") {
			throw new Error("pi's runtime keeps the schema-rejected thinking level as stored");
		}
		scenarios.push({
			name: "a schema-rejected thinking level loads without a diagnostic and the runtime keeps the raw value",
			inputs: { storedSettings: { defaultThinkingLevel: "bogus" }, schema: "SettingsSchema" },
			expected: { diagnosticCount: 0, defaultThinkingLevel: "bogus", schemaCheck: false },
			normalizations: [],
			observed,
		});
		rmSync(settingsPath);
	}

	// ── The compaction consumer's own validation ─────────────────────────────────────────────
	{
		const observeResolution = (compaction: any) => {
			const settingsPath = path.join(agentDir, "settings.json");
			writeJson(settingsPath, { compaction });
			const manager = SettingsManager.create(workspace, agentDir);
			const diagnostics = collectSettingsDiagnostics(manager);
			let error: string | null = null;
			try {
				manager.getCompactionSettings();
			} catch (thrown) {
				error = thrown instanceof Error ? thrown.message : String(thrown);
			}
			rmSync(settingsPath);
			return { diagnosticCount: diagnostics.length, error };
		};
		const negativeReserve = observeResolution({ reserveTokens: -1 });
		const negativeKeepRecent = observeResolution({ keepRecentTokens: -1 });
		const fractionalReserve = observeResolution({ reserveTokens: 1.5 });
		const observed = { negativeReserve, negativeKeepRecent, fractionalReserve };
		if (negativeReserve.error !== "Invalid compaction.reserveTokens setting: -1. Expected a non-negative safe integer.") {
			throw new Error(`unexpected reserve diagnostic: ${JSON.stringify(negativeReserve.error)}`);
		}
		if (negativeKeepRecent.error !==
			"Invalid compaction.keepRecentTokens setting: -1. Expected a non-negative safe integer.") {
			throw new Error(`unexpected keepRecentTokens diagnostic: ${JSON.stringify(negativeKeepRecent.error)}`);
		}
		if (fractionalReserve.error !==
			"Invalid compaction.reserveTokens setting: 1.5. Expected a non-negative safe integer.") {
			throw new Error(`unexpected fractional diagnostic: ${JSON.stringify(fractionalReserve.error)}`);
		}
		for (const [name, result] of Object.entries(observed)) {
			if ((result as any).diagnosticCount !== 0) {
				throw new Error(`${name}: the compaction diagnostic is a consumer error, not a load diagnostic`);
			}
		}
		scenarios.push({
			name: "the compaction consumer rejects negative and fractional token settings naming the field",
			inputs: {
				storedSettings: [
					{ compaction: { reserveTokens: -1 } },
					{ compaction: { keepRecentTokens: -1 } },
					{ compaction: { reserveTokens: 1.5 } },
				],
				consumer: "SettingsManager.getCompactionSettings",
			},
			expected: {
				negativeReserveMessage: "Invalid compaction.reserveTokens setting: -1. Expected a non-negative safe integer.",
				negativeKeepRecentMessage:
					"Invalid compaction.keepRecentTokens setting: -1. Expected a non-negative safe integer.",
				fractionalReserveMessage:
					"Invalid compaction.reserveTokens setting: 1.5. Expected a non-negative safe integer.",
				diagnosticCount: 0,
			},
			normalizations: [],
			observed,
		});
	}
} finally {
	rmSync(root, { recursive: true, force: true });
}

// ── Write the artifact and manifest ──────────────────────────────────────────────────────────

const replacements: Array<[string, string]> = [
	[workspace.split(path.sep).join("/"), "<workspace>"],
	[agentDir.split(path.sep).join("/"), "<agent-dir>"],
	[homeDir.split(path.sep).join("/"), "<home>"],
	[root.split(path.sep).join("/"), "<temp-root>"],
	[piCheckout.split(path.sep).join("/"), "<pi-checkout>"],
];
function normalize(value: any): any {
	if (typeof value === "string") {
		let result = value;
		for (const [from, to] of replacements) {
			result = result.split(from).join(to);
		}
		return result;
	}
	if (Array.isArray(value)) return value.map(normalize);
	if (value && typeof value === "object") {
		const result: Record<string, any> = {};
		for (const [key, entry] of Object.entries(value)) result[key] = normalize(entry);
		return result;
	}
	return value;
}

const sourceEndpoints = [
	"packages/coding-agent/src/core/settings-manager.ts:SettingsManager,SettingsError,FileSettingsStorage",
	"packages/coding-agent/src/core/settings-diagnostics.ts:collectSettingsDiagnostics,deduplicateDiagnostics",
	"packages/coding-agent/src/core/settings-schema.ts:SettingsSchema",
	"packages/coding-agent/src/main.ts:startupSettingsDiagnostics,startupDiagnostics",
];

const normalizedScenarios = normalize(scenarios);
const artifact = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	capturedAt,
	environment,
	sourceEndpoints,
	scenarios: normalizedScenarios,
};
const artifactBytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1115",
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} <pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
	},
	artifacts: [
		{
			path: "settings-diagnostics.json",
			family,
			bytes: artifactBytes.length,
			sha256: sha256(artifactBytes),
			sourceEndpoints,
			scenarios: normalizedScenarios,
		},
	],
};

mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, "settings-diagnostics.json"), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} (1 artifact, ${normalizedScenarios.length} scenarios) for ${baselineName}`);
