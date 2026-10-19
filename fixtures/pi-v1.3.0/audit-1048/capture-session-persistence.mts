#!/usr/bin/env tsx
/**
 * Capture the `audit-1048` application Session family for the pi-v1.3.0 evidence registry.
 *
 * Scope: `#1048` only. These are frozen upstream observations of the public `@earendil-works/pi-coding-agent`
 * and `@earendil-works/pi-ai` source entries. They do not claim that Pike's C++ implementation is aligned
 * or verified; capability status remains in `docs/research/pi-v1.3.0/1048-session-persistence.md`.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1048/capture-session-persistence.mts
 *
 * The script re-execs itself under the checkout's tsx so the frozen workspace `paths` resolve.
 * It refuses a checkout whose HEAD is not the registered baseline revision, a modified tracked
 * source tree, a non-Linux/glibc host, and a family already registered to another baseline.
 *
 * The frozen source workspace is missing the ignored generated shard
 * `packages/ai/src/providers/data/azure.json` (the checked-in `azure.models.ts` imports it). An
 * empty shard was supplied in the private checkout so the public source entry graph can load;
 * no tracked source file is changed and the model catalog is not part of these observations.
 * The preflight below refuses a checkout without that ignored shard and names the reason.
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1048";
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
	throw new Error("pi capture requires an unmodified tracked source checkout");
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
const azureDataPath = path.join(piCheckout, "packages/ai/src/providers/data/azure.json");
if (!existsSync(azureDataPath)) {
	throw new Error(
		`the frozen checkout is missing ignored generated shard ${azureDataPath}; ` +
			"the checked-in azure.models.ts imports it. Supply an empty JSON object there before capture " +
			'(for example: printf \'{}\\n\' > .../azure.json). No tracked source file is changed.',
	);
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

const sourceEndpoints = {
	tools: [
		"packages/coding-agent/src/index.ts:createAgentSession,SessionManager,SettingsManager,DefaultResourceLoader,ModelRuntime",
		"packages/coding-agent/src/core/agent-session.ts:AgentSession.getActiveToolNames,AgentSession.setActiveToolsByName,AgentSession.prompt,_restoreToolsFromTranscript",
		"packages/coding-agent/src/core/sdk.ts:createAgentSession",
		"packages/coding-agent/src/core/settings-manager.ts:getToolListError,applyToolModifiers",
		"packages/coding-agent/test/default-tools-setting.test.ts:restores the loadout the transcript records,applies +name and -name tool options to the recorded loadout",
		"packages/ai/src/compat.ts:registerFauxProvider",
		"packages/ai/src/providers/faux.ts:fauxAssistantMessage,fauxThinking",
	],
	compaction: [
		"packages/coding-agent/src/core/agent-session.ts:AgentSession.compact,_runDefaultCompaction",
		"packages/coding-agent/src/core/session-manager.ts:SessionManager.appendCompaction,buildSessionProjection",
		"packages/coding-agent/src/core/compaction/index.ts:prepareCompaction,compact",
		"packages/coding-agent/src/core/compaction/utils.ts:serializeConversation",
		"packages/coding-agent/test/compaction-serialization.test.ts:should omit thinking",
		"packages/coding-agent/test/session-manager/build-context.test.ts:includes summary before kept messages",
		"packages/ai/src/compat.ts:registerFauxProvider",
		"packages/ai/src/providers/faux.ts:fauxAssistantMessage,fauxThinking",
	],
	tree: [
		"packages/coding-agent/src/core/session-manager.ts:SessionManager.appendUsage,appendContextEdit,appendCustomEntry,appendSessionInfo,appendLabelChange,branch,branchWithSummary,createBranchedSession,getTree,getBranch,buildSessionProjection",
		"packages/coding-agent/test/session-context-edit.test.ts:omits a target only from model projection,replaces only content and lets the latest edit win",
		"packages/coding-agent/test/session-manager/tree-traversal.test.ts:inserts branch summary with the source and destination and advances leaf",
		"packages/coding-agent/test/agent-session-branching.test.ts:should fork from middle of conversation",
	],
};

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");

function parseJsonl(text: string): any[] {
	return text
		.split("\n")
		.filter((line) => line.length > 0)
		.map((line) => JSON.parse(line));
}

function readJsonl(pathname: string): string {
	return readFileSync(pathname, "utf8");
}

function normalizeRoot(text: string, root: string): string {
	return text.replaceAll(root, "<capture-root>");
}

function contentText(content: any): string {
	if (typeof content === "string") return content;
	if (!Array.isArray(content)) return "";
	return content
		.map((block) => {
			if (block?.type === "text") return block.text;
			if (block?.type === "thinking") return block.thinking;
			if (block?.type === "toolCall") return `${block.name}(${JSON.stringify(block.arguments)})`;
			return "";
		})
		.join("");
}

function contentTypes(content: any): string[] {
	if (typeof content === "string") return ["text"];
	if (!Array.isArray(content)) return [];
	return content.map((block) => block?.type ?? "unknown");
}

function systemMessageSummary(message: any): any {
	return {
		role: message.role,
		sectionNames: Object.keys(message.sections ?? {}),
		toolsAdded: (message.toolsAdded ?? []).map((tool: any) => tool.name),
		hasTimestamp: typeof message.timestamp === "number",
	};
}

function messageSummary(message: any): any {
	if (message.role === "compactionSummary") {
		return { role: message.role, summary: message.summary, tokensBefore: message.tokensBefore };
	}
	if (message.role === "branchSummary") {
		return { role: message.role, summary: message.summary };
	}
	if (message.role === "system") return systemMessageSummary(message);
	return {
		role: message.role,
		contentTypes: contentTypes(message.content),
		text: contentText(message.content),
		hasThinking: Array.isArray(message.content) && message.content.some((block: any) => block?.type === "thinking"),
	};
}

async function importPublicEntries() {
	const ai: any = await import(pathToFileURL(path.join(piCheckout, "packages/ai/src/index.ts")).href);
	const compat: any = await import(pathToFileURL(path.join(piCheckout, "packages/ai/src/compat.ts")).href);
	const codingAgent: any = await import(pathToFileURL(path.join(piCheckout, "packages/coding-agent/src/index.ts")).href);
	return { ai, compat, codingAgent };
}

function makeRoot(label: string): string {
	const root = execFileSync("mktemp", ["-d", path.join(tmpdir(), `cch-1048-${label}-XXXXXX`)], {
		encoding: "utf8",
	}).trim();
	mkdirSync(path.join(root, "workspace"), { recursive: true });
	mkdirSync(path.join(root, "agent"), { recursive: true });
	mkdirSync(path.join(root, "sessions"), { recursive: true });
	return root;
}

async function registerFauxProvider(
	ai: any,
	compat: any,
	modelRuntime: any,
	name: string,
): Promise<{ faux: any; model: any }> {
	const faux = compat.registerFauxProvider({
		provider: name,
		api: name,
		models: [{ id: "faux-1", name: "Faux 1", reasoning: true, contextWindow: 200000, maxTokens: 8192 }],
	});
	const model = faux.getModel();
	modelRuntime.registerProvider(model.provider, {
		baseUrl: model.baseUrl,
		apiKey: "dummy-audit-key",
		api: faux.api,
		models: faux.models.map((catalogModel: any) => ({
			id: catalogModel.id,
			name: catalogModel.name,
			api: catalogModel.api,
			reasoning: catalogModel.reasoning,
			input: catalogModel.input,
			cost: catalogModel.cost,
			contextWindow: catalogModel.contextWindow,
			maxTokens: catalogModel.maxTokens,
			baseUrl: catalogModel.baseUrl,
		})),
	});
	return { faux, model };
}

async function makeModelRuntime(ai: any, codingAgent: any): Promise<any> {
	const credentials = new ai.InMemoryCredentialStore();
	const modelRuntime = await codingAgent.ModelRuntime.create({
		credentials,
		modelsPath: null,
		allowModelNetwork: false,
	});
	if (typeof modelRuntime.getAuth !== "function") {
		throw new Error("frozen ModelRuntime does not expose the session auth surface");
	}
	return modelRuntime;
}

async function captureToolRestore(ai: any, compat: any, codingAgent: any): Promise<any> {
	const root = makeRoot("tools");
	const cwd = path.join(root, "workspace");
	const agentDir = path.join(root, "agent");
	const sessionDir = path.join(root, "sessions");
	const modelRuntime = await makeModelRuntime(ai, codingAgent);
	const { faux, model } = await registerFauxProvider(ai, compat, modelRuntime, "faux-tools");
	try {
		const settingsManager = codingAgent.SettingsManager.inMemory({
			defaultTools: ["read", "bash"],
			compaction: { enabled: true, reserveTokens: 1000, keepRecentTokens: 1 },
		});
		const resourceLoader = new codingAgent.DefaultResourceLoader({ cwd, agentDir, settingsManager });
		await resourceLoader.reload();
		const sessionManager = codingAgent.SessionManager.create(cwd, sessionDir);
		const created = await codingAgent.createAgentSession({
			cwd,
			agentDir,
			modelRuntime,
			model,
			settingsManager,
			resourceLoader,
			sessionManager,
		});
		const session = created.session;
		const observation: any = {
			initialActive: session.getActiveToolNames(),
			initialModel: { provider: session.model.provider, id: session.model.id },
			initialThinkingLevel: session.thinkingLevel,
		};

		faux.setResponses([compat.fauxAssistantMessage("first response")]);
		await session.prompt("initial tools");
		observation.afterFirstPromptActive = session.getActiveToolNames();

		session.setActiveToolsByName(["read", "bash", "write"]);
		observation.afterActivationActive = session.getActiveToolNames();
		faux.appendResponses([compat.fauxAssistantMessage("write activated")]);
		await session.prompt("activate write");

		const sessionFile = sessionManager.getSessionFile();
		const beforeResumeJsonl = readJsonl(sessionFile);
		const beforeResumeEntries = parseJsonl(beforeResumeJsonl);
		observation.beforeResume = {
			active: session.getActiveToolNames(),
			leafId: sessionManager.getLeafId(),
			entryTypes: beforeResumeEntries.map((entry) => entry.type),
			toolDeclarations: beforeResumeEntries
				.filter((entry) => entry.type === "message" && entry.message?.role === "system")
				.map((entry) => ({
					toolsAdded: (entry.message.toolsAdded ?? []).map((tool: any) => tool.name),
					toolsRemoved: (entry.message.toolsRemoved ?? []).map((tool: any) => tool.name),
					sectionNames: Object.keys(entry.message.sections ?? {}),
				})),
			messageRoles: session.messages.map((message: any) => message.role),
			model: { provider: session.model.provider, id: session.model.id },
			thinkingLevel: session.thinkingLevel,
		};
		session.dispose();

		const firstResumeManager = codingAgent.SessionManager.open(sessionFile);
		const firstResume = await codingAgent.createAgentSession({
			cwd,
			agentDir,
			modelRuntime,
			model,
			settingsManager,
			resourceLoader,
			sessionManager: firstResumeManager,
			tools: ["-bash", "+edit"],
		});
		observation.resumeWithRelativeModifiers = {
			requestedModifiers: ["-bash", "+edit"],
			active: firstResume.session.getActiveToolNames(),
			messageRoles: firstResume.session.messages.map((message: any) => message.role),
			leafId: firstResumeManager.getLeafId(),
			model: { provider: firstResume.session.model.provider, id: firstResume.session.model.id },
			thinkingLevel: firstResume.session.thinkingLevel,
		};
		firstResume.session.dispose();

		const secondResumeManager = codingAgent.SessionManager.open(sessionFile);
		const secondResume = await codingAgent.createAgentSession({
			cwd,
			agentDir,
			modelRuntime,
			model,
			settingsManager,
			resourceLoader,
			sessionManager: secondResumeManager,
		});
		observation.resumeWithoutModifiers = {
			active: secondResume.session.getActiveToolNames(),
			messageRoles: secondResume.session.messages.map((message: any) => message.role),
			leafId: secondResumeManager.getLeafId(),
		};
		const afterResumeJsonl = readJsonl(sessionFile);
		observation.resumeRewroteTranscript = afterResumeJsonl !== beforeResumeJsonl;
		observation.sessionFileLineCount = beforeResumeEntries.length;
		secondResume.session.dispose();

		if (observation.afterActivationActive.join(",") !== "read,bash,write") {
			throw new Error(`unexpected activated loadout: ${observation.afterActivationActive.join(",")}`);
		}
		if (observation.resumeWithRelativeModifiers.active.join(",") !== "read,write,edit") {
			throw new Error(
				`unexpected modified resumed loadout: ${observation.resumeWithRelativeModifiers.active.join(",")}`,
			);
		}
		if (observation.resumeWithoutModifiers.active.join(",") !== "read,bash,write") {
			throw new Error(
				`unexpected restored loadout: ${observation.resumeWithoutModifiers.active.join(",")}`,
			);
		}
		if (observation.resumeRewroteTranscript) {
			throw new Error("resume rewrote the session transcript");
		}

		observation.sessionJsonl = normalizeRoot(beforeResumeJsonl, root);
		return observation;
	} finally {
		faux.unregister();
	}
}

async function captureCompactionBoundary(ai: any, compat: any, codingAgent: any): Promise<any> {
	const root = makeRoot("compaction");
	const cwd = path.join(root, "workspace");
	const agentDir = path.join(root, "agent");
	const sessionDir = path.join(root, "sessions");
	const modelRuntime = await makeModelRuntime(ai, codingAgent);
	const { faux, model } = await registerFauxProvider(ai, compat, modelRuntime, "faux-compaction");
	try {
		const settingsManager = codingAgent.SettingsManager.inMemory({
			defaultTools: ["read", "bash"],
			compaction: { enabled: true, reserveTokens: 1000, keepRecentTokens: 1 },
			retry: { enabled: false },
		});
		const resourceLoader = new codingAgent.DefaultResourceLoader({ cwd, agentDir, settingsManager });
		await resourceLoader.reload();
		const sessionManager = codingAgent.SessionManager.create(cwd, sessionDir);
		const created = await codingAgent.createAgentSession({
			cwd,
			agentDir,
			modelRuntime,
			model,
			settingsManager,
			resourceLoader,
			sessionManager,
		});
		const session = created.session;
		const observation: any = {
			initialActive: session.getActiveToolNames(),
		};

		faux.setResponses([
			compat.fauxAssistantMessage([
				compat.fauxThinking("SECRET_THINKING_FIRST"),
				compat.fauxText("first answer"),
			]),
			compat.fauxAssistantMessage([
				compat.fauxThinking("SECRET_THINKING_SECOND"),
				compat.fauxText("second answer"),
			]),
		]);
		await session.prompt("first request");
		await session.prompt("second request");

		const sessionFile = sessionManager.getSessionFile();
		const beforeJsonl = readJsonl(sessionFile);
		const beforeEntries = parseJsonl(beforeJsonl);
		observation.beforeCompaction = {
			entryTypes: beforeEntries.map((entry) => entry.type),
			messageRoles: beforeEntries
				.filter((entry) => entry.type === "message")
				.map((entry) => entry.message.role),
			thinkingBlocks: beforeEntries
				.filter((entry) => entry.type === "message")
				.flatMap((entry) => entry.message?.content ?? [])
				.filter((block: any) => block?.type === "thinking")
				.map((block: any) => block.thinking),
			projectionRoles: sessionManager.buildSessionProjection().messages.map((message: any) => message.role),
			sessionJsonl: normalizeRoot(beforeJsonl, root),
		};

		const summarizationRequests: any[] = [];
		faux.setResponses([
			(context: any) => {
				summarizationRequests.push({
					roles: context.messages.map((message: any) => message.role),
					userText: contentText(context.messages.find((message: any) => message.role === "user")?.content),
				});
				return compat.fauxAssistantMessage("## Goal\nfrozen history summary");
			},
			(context: any) => {
				summarizationRequests.push({
					roles: context.messages.map((message: any) => message.role),
					userText: contentText(context.messages.find((message: any) => message.role === "user")?.content),
				});
				return compat.fauxAssistantMessage("## Original Request\nfrozen turn prefix summary");
			},
		]);
		const result = await session.compact("focus on persistence");

		const afterJsonl = readJsonl(sessionFile);
		const afterEntries = parseJsonl(afterJsonl);
		const originalLineCount = beforeJsonl.split("\n").filter((line) => line.length > 0).length;
		const afterLines = afterJsonl.split("\n").filter((line) => line.length > 0);
		const originalLinesByteIdentical = afterJsonl.startsWith(beforeJsonl);
		const appendedLines = afterLines.slice(originalLineCount);
		const compactionEntry = afterEntries.find((entry) => entry.type === "compaction");
		const keptEntry = afterEntries.find((entry) => entry.id === compactionEntry?.firstKeptEntryId);
		const projection = sessionManager.buildSessionProjection();

		observation.compaction = {
			result: {
				summary: result.summary,
				firstKeptEntryType: keptEntry?.type,
				firstKeptMessageRole: keptEntry?.message?.role,
				tokensBefore: result.tokensBefore,
				estimatedTokensAfter: result.estimatedTokensAfter,
				usagePresent: result.usage !== undefined,
				details: result.details,
			},
			entry: compactionEntry
				? {
						type: compactionEntry.type,
						summary: compactionEntry.summary,
						firstKeptEntryIdPresent: typeof compactionEntry.firstKeptEntryId === "string",
						firstKeptEntryIdEqualsEntry: compactionEntry.firstKeptEntryId === compactionEntry.id,
						tokensBefore: compactionEntry.tokensBefore,
						details: compactionEntry.details,
						fromHook: compactionEntry.fromHook,
						usagePresent: compactionEntry.usage !== undefined,
						systemMessage: compactionEntry.systemMessage
							? systemMessageSummary(compactionEntry.systemMessage)
							: null,
						systemMessageTimestampEqualsEntry:
							compactionEntry.systemMessage !== undefined &&
							compactionEntry.systemMessage.timestamp === new Date(compactionEntry.timestamp).getTime(),
					}
				: null,
			originalLinesByteIdentical,
			originalLineCount,
			afterLineCount: afterLines.length,
			appendedEntryTypes: appendedLines.map((line) => JSON.parse(line).type),
			appendedJsonl: appendedLines.join("\n"),
			originalThinkingBlocksStillStored: afterEntries
				.filter((entry) => entry.type === "message")
				.flatMap((entry) => entry.message?.content ?? [])
				.filter((block: any) => block?.type === "thinking")
				.map((block: any) => block.thinking),
			summarizationRequests,
			summarizationRequestsContainThinking: summarizationRequests.some((request) =>
				request.userText.includes("SECRET_THINKING"),
			),
			summaryContainsThinking: (result.summary ?? "").includes("SECRET_THINKING"),
			postCompactionProjection: projection.messages.map(messageSummary),
			sessionJsonl: normalizeRoot(afterJsonl, root),
		};

		if (!observation.compaction.originalLinesByteIdentical) {
			throw new Error("compaction rewrote the earlier transcript lines");
		}
		if (observation.compaction.summaryContainsThinking) {
			throw new Error("compaction summary retained thinking");
		}
		if (observation.compaction.summarizationRequestsContainThinking) {
			throw new Error("summarization request retained thinking");
		}
		if (
			observation.compaction.originalThinkingBlocksStillStored.indexOf("SECRET_THINKING_FIRST") === -1 ||
			observation.compaction.originalThinkingBlocksStillStored.indexOf("SECRET_THINKING_SECOND") === -1
		) {
			throw new Error("stored transcript lost a thinking block");
		}
		if (!observation.compaction.entry?.systemMessage) {
			throw new Error("compaction entry did not capture the current system boundary");
		}
		session.dispose();
		return observation;
	} finally {
		faux.unregister();
	}
}

async function captureSessionTree(codingAgent: any): Promise<any> {
	const root = makeRoot("tree");
	const cwd = path.join(root, "workspace");
	const sessionDir = path.join(root, "sessions");
	const manager = codingAgent.SessionManager.create(cwd, sessionDir);
	const observation: any = {};

	manager.appendModelChange("faux", "faux-1");
	manager.appendThinkingLevelChange("medium");
	const user = manager.appendMessage({ role: "user", content: [{ type: "text", text: "original user" }], timestamp: 1 });
	const assistant = manager.appendMessage({
		role: "assistant",
		content: [{ type: "text", text: "original answer" }],
		api: "faux:x",
		provider: "faux",
		model: "faux-1",
		usage: {
			input: 1,
			output: 1,
			cacheRead: 0,
			cacheWrite: 0,
			totalTokens: 2,
			cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
		},
		stopReason: "stop",
		timestamp: 2,
	});
	const usage = manager.appendUsage("cache_warm", "faux", "faux-1", {
		input: 1,
		output: 0,
		cacheRead: 0,
		cacheWrite: 0,
		totalTokens: 1,
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
	}, "note");
	manager.appendCustomEntry("audit", { key: "value" });
	manager.appendLabelChange(user, "first");
	manager.appendSessionInfo("tree session");

	const beforeEdit = manager.buildSessionProjection();
	const contextEdit = manager.appendContextEdit(user, {
		content: [{ type: "text", text: "edited user" }],
	});
	const afterEdit = manager.buildSessionProjection();

	manager.branch(user);
	const branchUser = manager.appendMessage({
		role: "user",
		content: [{ type: "text", text: "branch question" }],
		timestamp: 3,
	});
	const branchAssistant = manager.appendMessage({
		role: "assistant",
		content: [{ type: "text", text: "branch answer" }],
		api: "faux:x",
		provider: "faux",
		model: "faux-1",
		usage: {
			input: 1,
			output: 1,
			cacheRead: 0,
			cacheWrite: 0,
			totalTokens: 2,
			cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
		},
		stopReason: "stop",
		timestamp: 4,
	});
	const branchSummary = manager.branchWithSummary(assistant, "branch summary text", { why: "test" }, false, {
		input: 2,
		output: 1,
		cacheRead: 0,
		cacheWrite: 0,
		totalTokens: 3,
		cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
	});

	const tree = manager.getTree();
	const branch = manager.getBranch();
	const projection = manager.buildSessionProjection();
	const originalSessionFile = manager.getSessionFile();
	const originalJsonl = readJsonl(originalSessionFile);
	const originalEntries = parseJsonl(originalJsonl);
	const usageEntry = originalEntries.find((entry) => entry.type === "usage");
	const contextEditEntry = originalEntries.find((entry) => entry.type === "context_edit");
	const branchSummaryEntry = originalEntries.find((entry) => entry.type === "branch_summary");
	const branchSummaryWasLeaf = manager.getLeafId() === branchSummary;
	const liveEntryTypesBeforeFork = manager.getEntries().map((entry: any) => entry.type);
	const forkedFile = manager.createBranchedSession(assistant);
	const forkedManager = codingAgent.SessionManager.open(forkedFile);
	const forkedEntries = forkedManager.getEntries();
	const reopenedOriginal = codingAgent.SessionManager.open(originalSessionFile);

	const flatten = (nodes: any[]): any[] =>
		nodes.flatMap((node) => [
			{
				type: node.entry.type,
				label: node.label,
				children: flatten(node.children),
			},
		]);

	const inMemory = codingAgent.SessionManager.inMemory(cwd);
	inMemory.appendMessage({ role: "user", content: [{ type: "text", text: "memory user" }], timestamp: 5 });

	observation.entries = {
		wireEntryTypes: originalEntries.map((entry) => entry.type),
		liveEntryTypes: liveEntryTypesBeforeFork,
		usage: usageEntry
			? {
					type: usageEntry.type,
					kind: usageEntry.kind,
					provider: usageEntry.provider,
					model: usageEntry.model,
					note: usageEntry.note,
					totalTokens: usageEntry.usage?.totalTokens,
				}
			: null,
		contextEdit: contextEditEntry
			? {
					type: contextEditEntry.type,
					targetType: originalEntries.find((entry) => entry.id === contextEditEntry.targetId)?.type,
					replacement: contextEditEntry.replacement,
				}
			: null,
		branchSummary: branchSummaryEntry
			? {
					type: branchSummaryEntry.type,
					summary: branchSummaryEntry.summary,
					fromIdType: originalEntries.find((entry) => entry.id === branchSummaryEntry.fromId)?.type,
					fromHook: branchSummaryEntry.fromHook,
					usagePresent: branchSummaryEntry.usage !== undefined,
				}
			: null,
	};
	observation.contextEditProjection = {
		beforeRoles: beforeEdit.messages.map((message: any) => message.role),
		beforeUserText: contentText(beforeEdit.messages[0]?.content),
		afterRoles: afterEdit.messages.map((message: any) => message.role),
		afterUserText: contentText(afterEdit.messages[0]?.content),
		storedUserText: contentText(manager.getEntry(user)?.message?.content),
	};
	observation.tree = {
		topology: flatten(tree),
		activeBranchTypes: branch.map((entry: any) => entry.type),
		activeProjection: projection.messages.map(messageSummary),
		branchSummaryLeaf: branchSummaryWasLeaf,
		branchSummaryFromIdEqualsBranchLeaf: branchSummaryEntry?.fromId === branchAssistant,
	};
	observation.fork = {
		forkedEntryTypes: forkedEntries.map((entry: any) => entry.type),
		forkedLeafType: forkedEntries.find((entry: any) => entry.id === forkedManager.getLeafId())?.type,
		forkedProjectionRoles: forkedManager.buildSessionProjection().messages.map((message: any) => message.role),
		forkedHeaderParentSession: forkedManager.getHeader()?.parentSession
			? normalizeRoot(forkedManager.getHeader()?.parentSession, root)
			: null,
		originalFileStillHasAllEntries:
			reopenedOriginal.getEntries().map((entry: any) => entry.type).join(",") ===
			originalEntries.filter((entry) => entry.type !== "session").map((entry) => entry.type).join(","),
		originalFile: normalizeRoot(originalJsonl, root),
		forkedFile: normalizeRoot(readJsonl(forkedFile), root),
	};
	observation.inMemory = {
		isPersisted: inMemory.isPersisted(),
		sessionFile: inMemory.getSessionFile() ?? null,
		entryTypes: inMemory.getEntries().map((entry: any) => entry.type),
		projectionRoles: inMemory.buildSessionProjection().messages.map((message: any) => message.role),
	};

	if (!observation.contextEditProjection.afterUserText.includes("edited user")) {
		throw new Error("context edit did not change the model projection");
	}
	if (observation.contextEditProjection.storedUserText !== "original user") {
		throw new Error("context edit rewrote the stored message");
	}
	if (!observation.tree.branchSummaryLeaf) {
		throw new Error("branch summary did not become the active leaf");
	}
	if (!observation.tree.branchSummaryFromIdEqualsBranchLeaf) {
		throw new Error("branch summary did not record the abandoned branch leaf");
	}
	if (!observation.inMemory.entryTypes.includes("message") || observation.inMemory.sessionFile !== null) {
		throw new Error("in-memory session did not keep the same live entry model without a file");
	}
	return observation;
}

const { ai, compat, codingAgent } = await importPublicEntries();
const tools = await captureToolRestore(ai, compat, codingAgent);
const compaction = await captureCompactionBoundary(ai, compat, codingAgent);
const tree = await captureSessionTree(codingAgent);

const toolsScenarios = [
	{
		name: "activation persists the added tool declaration and advances the live loadout",
		inputs: {
			defaultTools: ["read", "bash"],
			activation: ["read", "bash", "write"],
			prompts: ["initial tools", "activate write"],
		},
		expected: {
			initialActive: tools.initialActive,
			afterFirstPromptActive: tools.afterFirstPromptActive,
			afterActivationActive: tools.afterActivationActive,
			toolDeclarations: tools.beforeResume.toolDeclarations,
			persistedEntryTypes: tools.beforeResume.entryTypes,
		},
		normalizations: [
			"entry ids, session id, ISO timestamps and the capture workspace path are host-generated; they are omitted from the expected observation",
		],
	},
	{
		name: "resume applies relative tool modifiers to the recorded loadout",
		inputs: { tools: ["-bash", "+edit"] },
		expected: {
			active: tools.resumeWithRelativeModifiers.active,
			messageRoles: tools.resumeWithRelativeModifiers.messageRoles,
			model: tools.resumeWithRelativeModifiers.model,
			thinkingLevel: tools.resumeWithRelativeModifiers.thinkingLevel,
		},
		normalizations: ["entry ids and the capture workspace path are host-generated"],
	},
	{
		name: "resume without modifiers restores the recorded order and does not rewrite the transcript",
		inputs: { mode: "tool options omitted" },
		expected: {
			active: tools.resumeWithoutModifiers.active,
			messageRoles: tools.resumeWithoutModifiers.messageRoles,
			resumeRewroteTranscript: tools.resumeRewroteTranscript,
			sessionFileLineCount: tools.sessionFileLineCount,
		},
		normalizations: ["entry ids and ISO timestamps are host-generated"],
	},
];

const compactionScenarios = [
	{
		name: "summary serialization omits thinking while the stored transcript retains it",
		inputs: {
			prompts: ["first request", "second request"],
			compactionSettings: { reserveTokens: 1000, keepRecentTokens: 1 },
			customInstructions: "focus on persistence",
		},
		expected: {
			thinkingInStoredTranscript: compaction.compaction.originalThinkingBlocksStillStored,
			summarizationRequestsContainThinking: compaction.compaction.summarizationRequestsContainThinking,
			summaryContainsThinking: compaction.compaction.summaryContainsThinking,
			summary: compaction.compaction.entry?.summary,
		},
		normalizations: [
			"summary usage and token counts depend on the frozen checkout's rendered system prompt and are observed, not normalized away",
		],
	},
	{
		name: "compaction appends a checkpoint without rewriting the earlier transcript lines",
		inputs: { entriesBeforeCompaction: compaction.beforeCompaction.entryTypes },
		expected: {
			originalLinesByteIdentical: compaction.compaction.originalLinesByteIdentical,
			originalLineCount: compaction.compaction.originalLineCount,
			afterLineCount: compaction.compaction.afterLineCount,
			appendedEntryTypes: compaction.compaction.appendedEntryTypes,
			compactionEntry: compaction.compaction.entry,
			result: compaction.compaction.result,
		},
		normalizations: [
			"entry ids, first-kept id, ISO timestamps and the capture workspace path are host-generated",
			"tokensBefore/estimatedTokensAfter/usage reflect the host-rendered system prompt and assistant usage",
		],
	},
	{
		name: "rebuilt context places the captured system boundary before the summary and retained tail",
		inputs: { mode: "post-compaction projection" },
		expected: {
			postCompactionProjection: compaction.compaction.postCompactionProjection,
			summarizationRequestRoles: compaction.compaction.summarizationRequests.map((request: any) => request.roles),
		},
		normalizations: ["system prompt section contents depend on the local skill/document discovery"],
	},
];

const treeScenarios = [
	{
		name: "usage and context_edit entries persist without rewriting the stored message",
		inputs: {
			target: "user message",
			replacement: { content: [{ type: "text", text: "edited user" }] },
		},
		expected: {
			wireEntryTypes: tree.entries.wireEntryTypes,
			liveEntryTypes: tree.entries.liveEntryTypes,
			usage: tree.entries.usage,
			contextEdit: tree.entries.contextEdit,
			projection: tree.contextEditProjection,
		},
		normalizations: ["entry ids and ISO timestamps are host-generated"],
	},
	{
		name: "branchWithSummary records the abandoned leaf and joins the active model context",
		inputs: { branchAt: "first assistant", summary: "branch summary text" },
		expected: {
			topology: tree.tree.topology,
			activeBranchTypes: tree.tree.activeBranchTypes,
			activeProjection: tree.tree.activeProjection,
			branchSummary: tree.entries.branchSummary,
		},
		normalizations: ["entry ids, labels and ISO timestamps are host-generated"],
	},
	{
		name: "createBranchedSession writes the active path with a parentSession header",
		inputs: { leaf: "first assistant" },
		expected: {
			forkedEntryTypes: tree.fork.forkedEntryTypes,
			forkedLeafType: tree.fork.forkedLeafType,
			forkedProjectionRoles: tree.fork.forkedProjectionRoles,
			forkedHeaderParentSessionPresent: typeof tree.fork.forkedHeaderParentSession === "string",
			originalFileStillHasAllEntries: tree.fork.originalFileStillHasAllEntries,
		},
		normalizations: ["entry ids, file paths and ISO timestamps are host-generated"],
	},
	{
		name: "in-memory sessions keep the live entry model without a file",
		inputs: { mode: "in-memory" },
		expected: tree.inMemory,
		normalizations: ["entry ids and ISO timestamps are host-generated"],
	},
];

const artifacts = [
	{
		name: "session-tool-restore.json",
		scenarios: toolsScenarios,
		sourceEndpoints: sourceEndpoints.tools,
		observations: tools,
	},
	{
		name: "session-compaction-boundary.json",
		scenarios: compactionScenarios,
		sourceEndpoints: sourceEndpoints.compaction,
		observations: compaction,
	},
	{
		name: "session-tree-and-fork.json",
		scenarios: treeScenarios,
		sourceEndpoints: sourceEndpoints.tree,
		observations: tree,
	},
];

const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const manifestArtifacts = [];
for (const artifactRecord of artifacts) {
	const artifact = {
		schema: "pike/pi-v1.3.0-audit-evidence/1",
		baseline: baselineName,
		revision: baseline.revision,
		capturedAt,
		environment,
		sourceEndpoints: artifactRecord.sourceEndpoints,
		scenarios: artifactRecord.scenarios,
		observations: artifactRecord.observations,
	};
	const artifactBytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
	writeFileSync(path.join(familyDir, artifactRecord.name), artifactBytes);
	manifestArtifacts.push({
		path: artifactRecord.name,
		family,
		bytes: artifactBytes.length,
		sha256: sha256(artifactBytes),
		sourceEndpoints: artifactRecord.sourceEndpoints,
		scenarios: artifactRecord.scenarios,
	});
}

const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline: baselineName,
	revision: baseline.revision,
	ticket: "1048",
	family,
	capturedAt,
	environment,
	checkoutPreparation: {
		generatedDataFile: "packages/ai/src/providers/data/azure.json",
		reason:
			"ignored generated model-catalog shard missing from the frozen workspace; the checked-in azure.models.ts imports it",
		action: "supplied an empty JSON object in the private checkout; no tracked source file changed",
	},
	generator: {
		path: generatorPath,
		sha256: sha256(generatorBytes),
		command: `PI_CHECKOUT=${piCheckout} PI_BASELINE=${baselineName} ${path.join(piCheckout, "node_modules/.bin/tsx")} ${generatorPath}`,
		sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
	},
	artifacts: manifestArtifacts,
};

writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(
	`captured ${family} (${manifestArtifacts.length} artifacts) for ${baselineName} (${baseline.revision})`,
);
