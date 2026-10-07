#!/usr/bin/env tsx
/**
 * Captures the pi v1.0.4 MCP + codemode evidence bundle (spec #882, ADR 0065).
 *
 * The bundle is the frozen differential baseline every MCP/codemode parity ticket diffs
 * against, so each artifact records a comparison surface rather than an implementation:
 *
 *   mcp-tool-surface.json       model-facing MCP tool naming/presentation and result schema
 *   mcp-config-surface.json     `mcp.json`/`settings.json` shapes, exposure policy, defaults
 *   codemode-tool.json          the inline `codemode` tool definition and output channel shapes
 *   codemode-source-grammar.lark  the verbatim grammar variant attached to the tool definition
 *   mcp-protocol-surface.json   protocol versions, notifications, discovery, credential store
 *
 * Values come from importing the frozen pi modules under the selected baseline, so the
 * evidence is the running surface, not a hand-copied literal. Declaration text that exists
 * only as a TypeScript type is sliced verbatim from the pinned source with a fail-loud anchor.
 *
 * Selection is by baseline name (ADR 0065): `PI_BASELINE=<name>` or the registry default.
 * The script refuses a pi checkout whose `HEAD` is not the baseline revision and refuses to
 * write into a bundle another baseline owns. It requires a baseline with its own bundle_path,
 * so it never writes the MCP/codemode artifacts into the preserved historical bundle root.
 *
 * Usage (from the repository root; `TSX_TSCONFIG_PATH` lets the pi checkout's workspace
 * `paths` resolve — the script re-execs itself under the pi tsx when it is unset):
 *   PI_BASELINE=pi-v1.0.4 ../pi/node_modules/.bin/tsx fixtures/pi-ai/capture/capture-mcp-codemode.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");

// The pi checkout's workspace `paths` only resolve under its own tsconfig. Re-exec once under
// the checkout's tsx with `TSX_TSCONFIG_PATH` set, so the documented invocation stays plain.
if (process.env.TSX_TSCONFIG_PATH === undefined) {
	const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
	const result = spawnSync(tsx, [fileURLToPath(import.meta.url), ...process.argv.slice(2)], {
		stdio: "inherit",
		env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
	});
	if (result.error) {
		throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
	}
	process.exit(result.status ?? 1);
}

const REGISTRY_PATH = path.join(fixtureDir, "baselines.json");
const REGISTRY_SCHEMA = "cpp-coding-harness/pi-ai-baselines/1";

function resolveBaseline(name: string | undefined): { registry: any; name: string; [key: string]: any } {
	const registry = JSON.parse(readFileSync(REGISTRY_PATH, "utf8"));
	if (registry.schema !== REGISTRY_SCHEMA) {
		throw new Error(`unexpected baseline registry schema: ${REGISTRY_PATH}`);
	}
	const selected = name ?? registry.default_baseline;
	const entry = registry.baselines[selected];
	if (!entry) {
		throw new Error(
			`unknown baseline ${JSON.stringify(selected)}; registered baselines: ` +
				Object.keys(registry.baselines).sort().join(", "),
		);
	}
	return { registry, name: selected, ...entry };
}

function owningBaseline(registry: any, target: string, root: string): { name: string; entry: any } | null {
	// The historical bundle occupies the fixture root, so a later bundle nests inside it;
	// ownership resolves by the LONGEST matching bundle path.
	const relativeTarget = path.relative(root, target);
	let owner: { name: string; entry: any } | null = null;
	for (const [name, entry] of Object.entries<any>(registry.baselines)) {
		const bundlePath = entry.bundle_path;
		const matches =
			bundlePath === ""
				? true
				: relativeTarget === bundlePath || relativeTarget.startsWith(`${bundlePath}/`);
		if (!matches) continue;
		if (owner === null || bundlePath.length > owner.entry.bundle_path.length) owner = { name, entry };
	}
	return owner;
}

function bundleDirectory(root: string, baseline: any): string {
	const bundlePath = baseline.bundle_path;
	return bundlePath === "" ? root : path.join(root, bundlePath);
}

const baseline = resolveBaseline(process.env.PI_BASELINE);
if (baseline.bundle_path === "") {
	throw new Error(
		`the MCP/codemode capture needs a baseline with its own bundle_path; ` +
			`${baseline.name} owns the historical fixture root. Select a registered bundle baseline, ` +
			`for example PI_BASELINE=pi-v1.0.4`,
	);
}
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baseline.name}), found ${head}`);
}

const captureRoot = bundleDirectory(fixtureDir, baseline);
const owner = owningBaseline(baseline.registry, captureRoot, fixtureDir);
if (owner && owner.name !== baseline.name) {
	throw new Error(
		`refusing to capture into ${captureRoot}: it belongs to bundle ${JSON.stringify(owner.name)}, ` +
			`but this capture records baseline ${JSON.stringify(baseline.name)}`,
	);
}
for (const target of process.argv.slice(2)) {
	const destination = path.resolve(target);
	const targetOwner = owningBaseline(baseline.registry, destination, fixtureDir);
	if (targetOwner && targetOwner.name !== baseline.name) {
		throw new Error(
			`refusing to write ${destination}: it belongs to bundle ${JSON.stringify(targetOwner.name)}, ` +
				`but this capture records baseline ${JSON.stringify(baseline.name)}`,
		);
	}
}

const bundleRoot = path.join(captureRoot, "mcp-codemode");
const ownerOfBundle = owningBaseline(baseline.registry, bundleRoot, fixtureDir);
if (ownerOfBundle && ownerOfBundle.name !== baseline.name) {
	throw new Error(
		`refusing to write ${bundleRoot}: it belongs to bundle ${JSON.stringify(ownerOfBundle.name)}`,
	);
}

const piSourceUrl = (relative: string): string => pathToFileURL(path.join(piCheckout, relative)).href;
const piSourceText = (relative: string): string => readFileSync(path.join(piCheckout, relative), "utf8");

/** Slice a verbatim declaration from the pinned source, failing loudly when the anchor moved. */
function declaration(relative: string, startAnchor: string, endAnchor: string): string {
	const text = piSourceText(relative);
	const start = text.indexOf(startAnchor);
	if (start === -1) throw new Error(`${relative}: declaration start anchor not found: ${JSON.stringify(startAnchor)}`);
	const end = text.indexOf(endAnchor, start + startAnchor.length);
	if (end === -1) throw new Error(`${relative}: declaration end anchor not found after ${JSON.stringify(startAnchor)}`);
	return text.slice(start, end + endAnchor.length);
}

function writeJson(relative: string, value: unknown): void {
	const target = path.join(bundleRoot, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	writeFileSync(target, `${JSON.stringify(value, null, 2)}\n`);
}

function writeText(relative: string, value: string): void {
	const target = path.join(bundleRoot, relative);
	mkdirSync(path.dirname(target), { recursive: true });
	writeFileSync(target, value);
}

const SOURCE = {
	mcpTools: "packages/coding-agent/src/extensions/mcp/tools.ts",
	mcpConfig: "packages/coding-agent/src/extensions/mcp/config.ts",
	mcpRuntime: "packages/coding-agent/src/extensions/mcp/runtime.ts",
	mcpOAuthExtension: "packages/coding-agent/src/extensions/mcp/oauth.ts",
	mcpServers: "packages/coding-agent/src/core/mcp-servers.ts",
	mcpAuthStorage: "packages/coding-agent/src/core/auth-storage.ts",
	mcpProtocolTypes: "packages/mcp/src/protocol/types.ts",
	mcpOAuthTypes: "packages/mcp/src/oauth/types.ts",
	mcpOAuthProvider: "packages/mcp/src/oauth/provider.ts",
	mcpDiscovery: "packages/mcp/src/oauth/discovery.ts",
	codemodeTool: "packages/coding-agent/src/extensions/codemode/tool.ts",
	codemodeSource: "packages/codemode/src/source.ts",
	codemodeTypes: "packages/codemode/src/types.ts",
	codemodePrelude: "packages/codemode/src/runtime/prelude-source.ts",
	settingsManager: "packages/coding-agent/src/core/settings-manager.ts",
} as const;

const mcpTools = await import(piSourceUrl(SOURCE.mcpTools));
const mcpConfig = await import(piSourceUrl(SOURCE.mcpConfig));
const mcpServers = await import(piSourceUrl(SOURCE.mcpServers));
const mcpOAuthExtension = await import(piSourceUrl(SOURCE.mcpOAuthExtension));
const mcpAuthStorage = await import(piSourceUrl(SOURCE.mcpAuthStorage));
const mcpProtocolTypes = await import(piSourceUrl(SOURCE.mcpProtocolTypes));
const mcpOAuthTypes = await import(piSourceUrl(SOURCE.mcpOAuthTypes));
const mcpDiscovery = await import(piSourceUrl(SOURCE.mcpDiscovery));
const codemodeTool = await import(piSourceUrl(SOURCE.codemodeTool));
const codemodeSource = await import(piSourceUrl(SOURCE.codemodeSource));
const codemodePrelude = await import(piSourceUrl(SOURCE.codemodePrelude));
const codemodeIndex = await import(piSourceUrl("packages/codemode/src/index.ts"));
const settingsManager = await import(piSourceUrl(SOURCE.settingsManager));

// ── MCP tool naming/presentation ────────────────────────────────────────────

const sampleTool = {
	name: "read_file",
	description: "Read a file from disk",
	inputSchema: {
		type: "object",
		properties: { path: { type: "string" } },
		required: ["path"],
	},
};

function mcpDefinition(exposure: string, tool: Record<string, unknown> = sampleTool): Record<string, unknown> {
	const name = mcpTools.createMcpToolName("filesystem", String(tool.name));
	const definition = mcpTools.createMcpToolDefinition({
		server: "filesystem",
		name,
		tool,
		exposure,
		namespace: { name: mcpServers.mcpNamespace("filesystem") },
		timeoutMs: 60000,
		getClient: async () => ({ callTool: async () => ({ content: [] }) }),
	});
	return {
		name: definition.name,
		label: definition.label,
		description: definition.description,
		parameters: definition.parameters,
		outputSchema: definition.outputSchema,
		exposure: definition.exposure,
		namespace: definition.namespace,
		annotations: definition.annotations ?? null,
	};
}

const firstCollisionName = mcpTools.createMcpToolName("srv", "a-b");
writeJson("mcp-tool-surface.json", {
	schema: "cpp-coding-harness/pi-mcp-tool-surface/1",
	source: SOURCE.mcpTools,
	toolName: {
		pattern: "mcp__<server>__<tool>",
		sanitization: "every character outside [A-Za-z0-9_] becomes _",
		maxLength: mcpTools.createMcpToolName("s".repeat(80), "t".repeat(80)).length,
		examples: [
			{ server: "filesystem", tool: "read_file", name: mcpTools.createMcpToolName("filesystem", "read_file") },
			{ server: "a-b", tool: "x.y", name: mcpTools.createMcpToolName("a-b", "x.y") },
			{ server: "s".repeat(80), tool: "t".repeat(80), name: mcpTools.createMcpToolName("s".repeat(80), "t".repeat(80)) },
		],
		collision: {
			first: firstCollisionName,
			second: mcpTools.createMcpToolName("srv", "a_b"),
			secondWhenTaken: mcpTools.createMcpToolName("srv", "a_b", (name: string) => name === firstCollisionName),
		},
	},
	namespace: {
		example: mcpServers.mcpNamespace("my-server"),
		rule: "mcp__<server> with every - replaced by _",
	},
	exposureMapping: {
		codemode: mcpTools.toToolExposure("codemode"),
		deferred: mcpTools.toToolExposure("deferred"),
		direct: mcpTools.toToolExposure("direct"),
		hidden: mcpTools.toToolExposure("hidden"),
	},
	definitions: {
		codemode: mcpDefinition("codemode"),
		withoutDescription: mcpDefinition("direct", { name: "empty", inputSchema: {} }),
		withAnnotations: mcpDefinition("deferred", {
			...sampleTool,
			annotations: { readOnlyHint: true, destructiveHint: false, title: "Sample title" },
		}),
	},
	outputMaxBytes: mcpTools.MCP_OUTPUT_MAX_BYTES,
	resultSchema: {
		withoutStructuredContent: mcpTools.createMcpResultSchema(undefined),
		withStructuredContent: mcpTools.createMcpResultSchema({
			type: "object",
			properties: { total: { type: "number" } },
			required: ["total"],
		}),
	},
	resourceTools: {
		list: mcpServers.LIST_MCP_RESOURCES_TOOL,
		listTemplates: mcpServers.LIST_MCP_RESOURCE_TEMPLATES_TOOL,
		read: mcpServers.READ_MCP_RESOURCE_TOOL,
	},
	isMcpToolNameExamples: {
		serverTool: mcpServers.isMcpToolName("mcp__filesystem__read_file"),
		resourceTool: mcpServers.isMcpToolName(mcpServers.READ_MCP_RESOURCE_TOOL),
		other: mcpServers.isMcpToolName("read"),
	},
});

// ── Declaration and config shapes ───────────────────────────────────────────

function validated(raw: unknown): unknown {
	const result = mcpServers.validateMcpServerConfig("demo", raw);
	return typeof result === "string" ? { error: result } : { config: result };
}

const tempDir = mkdtempSync(path.join(tmpdir(), "pi-mcp-capture-"));
let loadedGlobalAndProject: unknown;
let loadedGlobalOnly: unknown;
try {
	const agentDir = path.join(tempDir, "agent");
	const cwd = path.join(tempDir, "project");
	mkdirSync(agentDir, { recursive: true });
	mkdirSync(path.join(cwd, ".pi"), { recursive: true });
	writeFileSync(
		path.join(agentDir, "mcp.json"),
		`${JSON.stringify(
			{
				mcpServers: {
					filesystem: { command: "npx", args: ["-y", "@modelcontextprotocol/server-filesystem", "."] },
					docs: { url: "https://example.com/mcp", headers: { Authorization: "Bearer ${DOCS_TOKEN}" } },
					sentry: { url: "https://mcp.example.dev/mcp", exposure: "deferred" },
				},
				autoEnableCodemode: true,
			},
			null,
			2,
		)}\n`,
	);
	writeFileSync(
		path.join(cwd, ".pi", "mcp.json"),
		`${JSON.stringify(
			{
				mcpServers: {
					filesystem: { enabled: false },
					local: { command: "node", args: ["server.mjs"], exposure: "hidden" },
				},
			},
			null,
			2,
		)}\n`,
	);
	const sanitize = (loaded: any): unknown => ({
		servers: loaded.servers.map((server: any) => ({
			name: server.name,
			config: server.config,
			scope: server.scope ?? null,
			source: "<config>",
			override: server.override ? "<override>" : null,
		})),
		autoEnableCodemode: loaded.autoEnableCodemode ?? null,
		errors: loaded.errors,
		projectConfig: loaded.projectConfig ? "<projectConfig>" : null,
	});
	loadedGlobalAndProject = sanitize(mcpConfig.loadMcpConfig({ agentDir, cwd, projectTrusted: true }));
	loadedGlobalOnly = sanitize(mcpConfig.loadMcpConfig({ agentDir, cwd, projectTrusted: false }));
} finally {
	rmSync(tempDir, { recursive: true, force: true });
}

const exposureCases = ["codemode", "deferred", "direct", "hidden", "codemode-deferred", "nope"].map((exposure) => ({
	exposure,
	result: validated({ command: "node", exposure }),
}));

writeJson("mcp-config-surface.json", {
	schema: "cpp-coding-harness/pi-mcp-config-surface/1",
	sources: [SOURCE.mcpConfig, SOURCE.mcpServers, SOURCE.settingsManager],
	locations: {
		global: "<agentDir>/mcp.json",
		project: "<cwd>/.pi/mcp.json (trusted projects only)",
		settings: "<agentDir>/settings.json (codemode keys only)",
	},
	defaults: {
		exposure: mcpServers.getMcpToolExposure({ command: "node" }, "any_tool"),
		autoEnableCodemode: (loadedGlobalAndProject as any).autoEnableCodemode,
		codemodeInlineBudget: codemodeTool.DEFAULT_CODEMODE_INLINE_BUDGET,
		defaultTools: [...settingsManager.DEFAULT_TOOL_NAMES],
	},
	exposureValues: {
		cases: exposureCases,
		aliases: ["codemode-deferred -> codemode"],
		toolExposure: {
			exactWins: mcpServers.getMcpToolExposure(
				{ command: "node", exposure: "codemode", toolExposure: { "read_file": "direct" } },
				"read_file",
			),
			patternMatch: mcpServers.getMcpToolExposure(
				{ command: "node", exposure: "codemode", toolExposure: { "files_*": "hidden" } },
				"files_read",
			),
			firstPatternWins: mcpServers.getMcpToolExposure(
				{ command: "node", exposure: "codemode", toolExposure: { "*_read": "direct", "files_*": "hidden" } },
				"files_read",
			),
			fallbackToServer: mcpServers.getMcpToolExposure(
				{ command: "node", exposure: "deferred", toolExposure: { other: "direct" } },
				"unlisted",
			),
		},
	},
	serverEntryExamples: [
		{ label: "stdio", result: validated({ command: "node", args: ["server.mjs"], env: { TOKEN: "${TOKEN}" } }) },
		{ label: "http", result: validated({ url: "https://example.com/mcp", headers: { Authorization: "Bearer dummy-docs-token" } }) },
		{
			label: "http+oauth",
			result: validated({
				url: "https://example.com/mcp",
				oauth: { clientId: "dummy-client-id", callbackUrl: "http://127.0.0.1:7777/callback" },
			}),
		},
		{ label: "http+auth", result: validated({ url: "https://example.com/mcp", auth: { provider: "demo" } }) },
		{ label: "legacy-sse", result: validated({ type: "sse", url: "https://example.com/mcp" }) },
		{ label: "invalid-name", result: mcpServers.validateMcpServerConfig("bad name", { command: "node" }) },
	],
	loopbackRedirectUri: {
		accepted: mcpServers.isLoopbackRedirectUri("http://127.0.0.1:7777/callback"),
		rejected: mcpServers.isLoopbackRedirectUri("https://example.com/callback"),
	},
	loadedConfigs: {
		trustedProject: loadedGlobalAndProject,
		untrustedProject: loadedGlobalOnly,
	},
	declarations: {
		codemodeMode: declaration(SOURCE.settingsManager, "export type CodemodeMode = ", ";"),
		codemodeSettings: declaration(SOURCE.settingsManager, "export interface CodemodeSettings {", "\n}"),
		defaultToolNames: declaration(SOURCE.settingsManager, "export const DEFAULT_TOOL_NAMES", ";"),
		mcpExposure: declaration(SOURCE.mcpServers, "export type McpExposure = ", ";"),
		mcpExposures: declaration(SOURCE.mcpServers, "const MCP_EXPOSURES: readonly string[] = ", ";"),
		mcpExposureAliases: declaration(SOURCE.mcpServers, "const MCP_EXPOSURE_ALIASES: Readonly<Record<string, McpExposure>> = ", ";"),
		serverConfigBase: declaration(SOURCE.mcpServers, "interface McpServerConfigBase {", "\n}"),
		stdioServerConfig: declaration(SOURCE.mcpServers, "export interface McpStdioServerConfig extends McpServerConfigBase {", "\n}"),
		oauthConfig: declaration(SOURCE.mcpServers, "export interface McpOAuthConfig {", "\n}"),
		httpServerConfig: declaration(SOURCE.mcpServers, "export interface McpHttpServerConfig extends McpServerConfigBase {", "\n}"),
		serverConfigPatch: declaration(SOURCE.mcpConfig, "export interface McpServerConfigPatch {", "\n}"),
		overrideKeys: declaration(SOURCE.mcpConfig, "const OVERRIDE_KEYS = ", ";"),
	},
});

// ── Codemode inline script tool surface ─────────────────────────────────────

const codemodeDefinition = codemodeTool.createCodemodeToolDefinition();
writeJson("codemode-tool.json", {
	schema: "cpp-coding-harness/pi-codemode-tool-surface/1",
	sources: [SOURCE.codemodeTool, SOURCE.codemodeSource, SOURCE.codemodeTypes, SOURCE.codemodePrelude],
	name: codemodeDefinition.name,
	label: codemodeDefinition.label,
	description: codemodeDefinition.description,
	promptSnippet: codemodeDefinition.promptSnippet,
	promptGuidelines: codemodeDefinition.promptGuidelines,
	parameters: codemodeDefinition.parameters,
	exposure: codemodeDefinition.exposure,
	constrainedSampling: codemodeDefinition.constrainedSampling,
	toolNameConstant: codemodeTool.CODEMODE_TOOL_NAME,
	storeEntryType: codemodeTool.CODEMODE_STORE_ENTRY_TYPE,
	defaultInlineBudget: codemodeTool.DEFAULT_CODEMODE_INLINE_BUDGET,
	optionsPrefix: codemodeSource.CODEMODE_OPTIONS_PREFIX,
	limits: {
		maxOutputChars: codemodePrelude.MAX_OUTPUT_CHARS,
		maxOutputItems: codemodePrelude.MAX_OUTPUT_ITEMS,
		maxStoreValueChars: codemodePrelude.MAX_STORE_VALUE_CHARS,
		maxStoreTotalChars: codemodePrelude.MAX_STORE_TOTAL_CHARS,
		defaultInputSchemaMaxChars: codemodeIndex.DEFAULT_INPUT_SCHEMA_MAX_CHARS,
	},
	outputChannel: {
		items: [
			{ type: "text", text: "string" },
			{ type: "image", data: "base64 string", mimeType: "string" },
		],
		resultOk: { ok: true, value: "unknown", output: "CodemodeOutputItem[]", calls: "CodemodeCall[]", storeWrites: "CodemodeStoreWrites" },
		resultError: { ok: false, error: "CodemodeError", output: "CodemodeOutputItem[]", calls: "CodemodeCall[]" },
		callStatus: ["ok", "error", "cancelled"],
		errorKind: ["script", "timeout", "aborted", "sandbox"],
		sourceType: SOURCE.codemodeTypes,
	},
});

writeText("codemode-source-grammar.lark", codemodeSource.CODEMODE_SOURCE_GRAMMAR);

// ── Protocol, discovery, and credential surfaces ────────────────────────────

const store = new mcpOAuthExtension.McpOAuthCredentialStore(new mcpAuthStorage.InMemoryAuthStorageBackend());
store.forServer("filesystem", "https://example.com/mcp").save({
	serverUrl: "https://example.com/mcp",
	tokens: { access_token: "dummy-access-token", token_type: "Bearer", refresh_token: "dummy-refresh-token" },
	tokensExpireAt: 0,
});
const storedCredentialFile = store["backend"].withLock((current: string | undefined) => ({ result: current }));

writeJson("mcp-protocol-surface.json", {
	schema: "cpp-coding-harness/pi-mcp-protocol-surface/1",
	sources: [
		SOURCE.mcpProtocolTypes,
		SOURCE.mcpDiscovery,
		SOURCE.mcpOAuthTypes,
		SOURCE.mcpOAuthExtension,
		SOURCE.mcpRuntime,
	],
	protocol: {
		latest: mcpProtocolTypes.LATEST_PROTOCOL_VERSION,
		supported: [...mcpProtocolTypes.SUPPORTED_PROTOCOL_VERSIONS],
	},
	notifications: {
		initialized: "notifications/initialized",
		cancelled: { method: "notifications/cancelled" },
		progress: "notifications/progress",
	},
	discovery: {
		protectedResourceWellKnown: "/.well-known/oauth-protected-resource",
		authorizationServerWellKnown: mcpDiscovery
			.buildAuthorizationServerDiscoveryUrls("https://auth.example.com/tenant1")
			.map((entry: any) => entry.url.href),
		wwwAuthenticateExample: mcpDiscovery.parseWwwAuthenticate(
			'Bearer resource_metadata="https://example.com/.well-known/oauth-protected-resource/mcp", scope="files:read"',
		),
		protectedResourceMetadata: mcpOAuthTypes.parseProtectedResourceMetadata({
			resource: "https://example.com/mcp",
			authorization_servers: ["https://auth.example.com"],
			scopes_supported: ["files:read"],
		}),
		authorizationServerMetadata: mcpOAuthTypes.parseAuthorizationServerMetadata({
			issuer: "https://auth.example.com",
			authorization_endpoint: "https://auth.example.com/authorize",
			token_endpoint: "https://auth.example.com/token",
			registration_endpoint: "https://auth.example.com/register",
			response_types_supported: ["code"],
			code_challenge_methods_supported: ["S256"],
		}),
	},
	credentialStore: {
		file: "mcp-auth.json",
		keyFormat: "mcp__<server>|<serverUrl>",
		sampleFile: JSON.parse(storedCredentialFile ?? "{}"),
		sourceType: SOURCE.mcpOAuthProvider,
	},
	lifecycleStates: {
		client: ["idle", "connecting", "connected", "closed"],
		server: declaration(SOURCE.mcpRuntime, "type ServerState = ", ";"),
	},
	declarations: {
		cancelledNotification: declaration(SOURCE.mcpProtocolTypes, "export interface CancelledNotification {", "\n}"),
		protectedResourceMetadata: declaration(SOURCE.mcpOAuthTypes, "export interface OAuthProtectedResourceMetadata {", "\n}"),
		authorizationServerMetadata: declaration(SOURCE.mcpOAuthTypes, "export interface AuthorizationServerMetadata {", "\n}"),
		oauthState: declaration(SOURCE.mcpOAuthProvider, "export interface McpOAuthState {", "\n}"),
	},
});

console.log(`captured pi MCP/codemode bundle for baseline ${baseline.name} at ${head}`);
console.log(`wrote ${["mcp-tool-surface.json", "mcp-config-surface.json", "codemode-tool.json", "codemode-source-grammar.lark", "mcp-protocol-surface.json"].length} artifacts to ${bundleRoot}`);
