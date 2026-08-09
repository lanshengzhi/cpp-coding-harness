#!/usr/bin/env tsx
/**
 * Session- and value-suite scenario driver (issue #421, the session-family
 * differential goldens of the pi-coding-agent gate, ADR 0036).
 *
 * Runs the five scripted session scenarios against the FROZEN pi sources and
 * writes the committed snapshots at message/content-block level. This file is
 * spawned by `session-snapshots.mts` with the frozen checkout's capture
 * tsconfig, so the `@earendil-works/*` imports below resolve to the frozen
 * `packages/` sources (the same module-resolution trick the pi-ai/pi-tui
 * sidecars use for the ai package; the coding-agent internals are imported by
 * absolute path from `PI_CHECKOUT`).
 *
 * The five families (R2 §6): session lifecycle, resume, compaction, model
 * switching, and the session-family flows (most-recent selection + the
 * session-manager JSONL values). Each snapshot carries the pinned baseline
 * citation in `meta` and is regenerated deterministically: timestamps, usage,
 * entry ids, parentId chains, and machine paths are projected out, and the
 * script's own determinism is verified by `session-snapshots.mts` (capture +
 * byte-compare pass).
 *
 * Env:
 *   - `CCH_SESSION_FIXTURE_DIR`: target directory for the snapshot files.
 *   - `PI_CHECKOUT`: the frozen pi checkout (must sit at the baseline; the
 *     parent script guards this before spawning).
 */

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, basename } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { registerFauxProvider, fauxAssistantMessage } from "@earendil-works/pi-ai/compat";

const fixtureDir = process.env.CCH_SESSION_FIXTURE_DIR!;
const piCheckout = process.env.PI_CHECKOUT!;
const BASELINE = "83114817c68f5413e4d7ba6d7003ddc511cd31d2";
const ARTIFACT = "@earendil-works/pi-coding-agent@0.83.0";

const codingAgentSrc = (rel: string) =>
	pathToFileURL(join(piCheckout, "packages/coding-agent/src", rel)).href;
const codingAgentTest = (rel: string) =>
	pathToFileURL(join(piCheckout, "packages/coding-agent/test", rel)).href;

const { createAgentSession } = await import(codingAgentSrc("core/sdk.ts"));
const { AuthStorage } = await import(codingAgentSrc("core/auth-storage.ts"));
const { SessionManager, findMostRecentSession } = await import(
	codingAgentSrc("core/session-manager.ts"),
);
const { SettingsManager } = await import(codingAgentSrc("core/settings-manager.ts"));
const { convertToLlm } = await import(codingAgentSrc("core/messages.ts"));
const { createInMemoryModelRegistry, getModelRuntime } = await import(
	codingAgentTest("model-runtime-test-utils.ts"),
);
const { createTestResourceLoader } = await import(codingAgentTest("utilities.ts"));

// ── Canonical projection (shared with the C++ comparator) ───────────────────
// Message/content-block level: identity fields (api/provider/model/stopReason)
// kept, wall-clock timestamps / usage / diagnostics / response ids dropped.
// Session entries project to type + positional id + type-specific value
// fields; entry ids (identity) and parentId chains (tree structure) project
// out, firstKeptEntryId maps to the referenced entry's ordinal.

function canonicalContentBlock(block: any): any {
	if (block.type === "text") return { type: "text", text: block.text };
	if (block.type === "thinking") return { type: "thinking", thinking: block.thinking };
	if (block.type === "toolCall") {
		return { type: "toolCall", id: block.id, name: block.name, arguments: block.arguments };
	}
	if (block.type === "image") return { type: "image", data: block.data, mimeType: block.mimeType };
	return block;
}

function canonicalMessage(message: any): any {
	const out: any = { role: message.role };
	if (Array.isArray(message.content)) {
		out.content = message.content.map(canonicalContentBlock);
	} else if (typeof message.content === "string") {
		out.content = [{ type: "text", text: message.content }];
	}
	if (message.role === "assistant") {
		if (message.api !== undefined) out.api = message.api;
		if (message.provider !== undefined) out.provider = message.provider;
		if (message.model !== undefined) out.model = message.model;
		if (message.stopReason !== undefined) out.stopReason = message.stopReason;
		if (message.errorMessage !== undefined) out.errorMessage = message.errorMessage;
	}
	if (message.role === "toolResult") {
		if (message.toolCallId !== undefined) out.toolCallId = message.toolCallId;
		if (message.toolName !== undefined) out.toolName = message.toolName;
		if (message.isError !== undefined) out.isError = message.isError;
	}
	if (message.role === "compactionSummary" && message.summary !== undefined) {
		out.summary = message.summary;
	}
	if (message.role === "branchSummary" && message.summary !== undefined) {
		out.summary = message.summary;
	}
	if (message.role === "custom" && message.customType !== undefined) {
		out.customType = message.customType;
	}
	return out;
}

function projectEntries(entries: any[]): any[] {
	const idToOrdinal = new Map<string, string>();
	const out: any[] = [];
	for (const entry of entries) {
		idToOrdinal.set(entry.id, `entry-${out.length}`);
		// id is identity, parentId is tree-structure — both projected out.
		const projected: any = { type: entry.type, id: `entry-${out.length}` };
		if (entry.type === "message") {
			projected.message = canonicalMessage(entry.message);
		} else if (entry.type === "model_change") {
			projected.provider = entry.provider;
			projected.modelId = entry.modelId;
		} else if (entry.type === "thinking_level_change") {
			projected.thinkingLevel = entry.thinkingLevel;
		} else if (entry.type === "active_tools_change") {
			projected.activeToolNames = entry.activeToolNames;
		} else if (entry.type === "compaction") {
			projected.summary = entry.summary;
			if (entry.firstKeptEntryId !== undefined) {
				projected.firstKeptEntryId =
					idToOrdinal.get(entry.firstKeptEntryId) ?? entry.firstKeptEntryId;
			}
		} else if (entry.type === "session_info") {
			if (entry.name !== undefined) projected.name = entry.name;
		} else if (entry.type === "label") {
			if (entry.label !== undefined) projected.label = entry.label;
		}
		out.push(projected);
	}
	return out;
}

const meta = (family: string) => ({ baseline: BASELINE, artifact: ARTIFACT, family });

// ── Shared harness wiring (mirrors test/suite/harness.ts seams) ─────────────

async function setup(
	cwd: string,
	settings: any,
	id: string,
	models: any[] = [{ id: "faux-1", name: "One", reasoning: false }],
) {
	rmSync(cwd, { recursive: true, force: true });
	mkdirSync(cwd, { recursive: true });
	const faux = registerFauxProvider({ api: "fake", provider: "fake", models });
	faux.setResponses([]);
	const model = faux.getModel();
	const authStorage = AuthStorage.inMemory();
	await authStorage.modify(model.provider, async () => ({ type: "api_key", key: "faux-key" }));
	const modelRegistry = await createInMemoryModelRegistry(authStorage);
	modelRegistry.registerProvider(model.provider, {
		baseUrl: model.baseUrl,
		apiKey: "faux-key",
		api: faux.api,
		models: faux.models.map((m: any) => ({
			id: m.id, name: m.name, api: m.api, reasoning: m.reasoning,
			input: m.input, cost: m.cost, contextWindow: m.contextWindow, maxTokens: m.maxTokens, baseUrl: m.baseUrl,
		})),
	});
	const sessionManager = SessionManager.create(cwd, join(cwd, "sessions"), { id });
	const settingsManager = SettingsManager.inMemory(settings as any);
	const { session } = await createAgentSession({
		cwd,
		sessionManager,
		settingsManager,
		model,
		modelRuntime: getModelRuntime(modelRegistry),
		resourceLoader: createTestResourceLoader(),
	});
	return {
		session, sessionManager, settingsManager, faux,
		dispose: () => { session.dispose(); faux.unregister(); },
	};
}

async function reopen(cwd: string, file: string, settings: any = {}) {
	const sessionManager = SessionManager.open(file);
	const settingsManager = SettingsManager.inMemory(settings as any);
	const faux = registerFauxProvider({
		api: "fake", provider: "fake",
		models: [{ id: "faux-1", name: "One", reasoning: false }],
	});
	faux.setResponses([]);
	const model = faux.getModel();
	const authStorage = AuthStorage.inMemory();
	await authStorage.modify(model.provider, async () => ({ type: "api_key", key: "faux-key" }));
	const modelRegistry = await createInMemoryModelRegistry(authStorage);
	modelRegistry.registerProvider(model.provider, {
		baseUrl: model.baseUrl,
		apiKey: "faux-key",
		api: faux.api,
		models: faux.models.map((m: any) => ({
			id: m.id, name: m.name, api: m.api, reasoning: m.reasoning,
			input: m.input, cost: m.cost, contextWindow: m.contextWindow, maxTokens: m.maxTokens, baseUrl: m.baseUrl,
		})),
	});
	const { session } = await createAgentSession({
		cwd,
		sessionManager,
		settingsManager,
		modelRuntime: getModelRuntime(modelRegistry),
		resourceLoader: createTestResourceLoader(),
	});
	return {
		session, sessionManager, settingsManager, faux,
		dispose: () => { session.dispose(); faux.unregister(); },
	};
}

// ── Scenarios ────────────────────────────────────────────────────────────────

function write(name: string, snapshot: unknown) {
	writeFileSync(join(fixtureDir, name), JSON.stringify(snapshot, null, 2) + "\n");
}

// 1. Session lifecycle: new persisted session, two scripted turns.
{
	const cwd = join(tmpdir(), "cch-capture-lifecycle");
	const h = await setup(cwd, {}, "lifecycle-session");
	h.faux.setResponses([fauxAssistantMessage("Hello there!")]);
	await h.session.prompt("hi");
	h.faux.setResponses([fauxAssistantMessage("Second reply.")]);
	await h.session.prompt("again");
	write("session-lifecycle.json", {
		meta: meta("session-lifecycle"),
		messages: h.session.messages.map(canonicalMessage),
		context: convertToLlm(h.session.messages).map(canonicalMessage),
		entries: projectEntries(h.sessionManager.getEntries()),
		values: {
			model: h.session.model?.id,
			provider: h.session.model?.provider,
			thinkingLevel: h.session.thinkingLevel,
		},
	});
	h.dispose();
}

// 2. Resume: persist a session, reopen, capture the restored message level.
{
	const cwd = join(tmpdir(), "cch-capture-resume");
	const h = await setup(cwd, {}, "resume-session");
	h.faux.setResponses([fauxAssistantMessage("Hello there!")]);
	await h.session.prompt("hi");
	h.faux.setResponses([fauxAssistantMessage("Second reply.")]);
	await h.session.prompt("again");
	const file = h.sessionManager.getSessionFile()!;
	h.dispose();

	const r = await reopen(cwd, file);
	write("session-resume.json", {
		meta: meta("session-resume"),
		messages: r.session.messages.map(canonicalMessage),
		context: convertToLlm(r.session.messages).map(canonicalMessage),
		entries: projectEntries(r.sessionManager.getEntries()),
		values: {
			model: r.session.model?.id,
			provider: r.session.model?.provider,
			thinkingLevel: r.session.thinkingLevel,
		},
	});
	r.dispose();
}

// 3. Compaction: three turns then a manual compact (split-turn summary).
{
	const cwd = join(tmpdir(), "cch-capture-compaction");
	const h = await setup(cwd, { compaction: { keepRecentTokens: 0 } }, "compaction-session");
	h.faux.setResponses([fauxAssistantMessage("first reply.")]);
	await h.session.prompt("first");
	h.faux.setResponses([fauxAssistantMessage("second reply.")]);
	await h.session.prompt("second");
	h.faux.setResponses([fauxAssistantMessage("third reply.")]);
	await h.session.prompt("third");
	h.faux.setResponses([
		fauxAssistantMessage("summary of the work so far."),
		fauxAssistantMessage("turn context summary."),
	]);
	const result = await h.session.compact();
	write("session-compaction.json", {
		meta: meta("session-compaction"),
		messages: h.session.messages.map(canonicalMessage),
		context: convertToLlm(h.session.messages).map(canonicalMessage),
		entries: projectEntries(h.sessionManager.getEntries()),
		values: { summary: result.summary },
	});
	h.dispose();
}

// 4. Model switching: setModel to a second reasoning faux model.
{
	const cwd = join(tmpdir(), "cch-capture-model-switch");
	const h = await setup(
		cwd,
		{},
		"switch-session",
		[
			{ id: "faux-1", name: "One", reasoning: false },
			{ id: "faux-2", name: "Two", reasoning: true },
		],
	);
	h.faux.setResponses([fauxAssistantMessage("Hello there!")]);
	await h.session.prompt("hi");
	await h.session.setModel(h.faux.getModel("faux-2")!);
	h.faux.setResponses([fauxAssistantMessage("After the switch.")]);
	await h.session.prompt("after switch");
	write("session-model-switch.json", {
		meta: meta("session-model-switch"),
		messages: h.session.messages.map(canonicalMessage),
		context: convertToLlm(h.session.messages).map(canonicalMessage),
		entries: projectEntries(h.sessionManager.getEntries()),
		values: {
			model: h.session.model?.id,
			provider: h.session.model?.provider,
			thinkingLevel: h.session.thinkingLevel,
		},
	});
	h.dispose();
}

// 5. Session-family flows: most-recent selection over a session directory.
{
	const cwd = join(tmpdir(), "cch-capture-session-family");
	rmSync(cwd, { recursive: true, force: true });
	mkdirSync(cwd, { recursive: true });
	const dir = join(cwd, "sessions");
	mkdirSync(dir, { recursive: true });
	const makeSession = async (name: string, ageSeconds: number) => {
		const manager = SessionManager.create(cwd, dir, { id: name });
		manager.appendMessage({
			role: "user", content: [{ type: "text", text: "hello" }], timestamp: 1,
		} as any);
		manager.appendMessage({
			role: "assistant", content: [{ type: "text", text: "hi there" }],
			api: "fake", provider: "fake", model: "faux-1", stopReason: "stop", timestamp: 2,
		} as any);
		const file = manager.getSessionFile()!;
		const mtime = new Date(Date.now() - ageSeconds * 1000);
		const { promises } = await import("node:fs");
		await promises.utimes(file, mtime, mtime);
	};
	await makeSession("older", 300);
	await makeSession("mid", 200);
	await makeSession("newer", 100);
	const mostRecent = findMostRecentSession(dir);
	// The selected session id: pi names session files `<timestamp>_<id>.jsonl`;
	// the timestamp prefix is machine-specific, the id is the value.
	const mostRecentName = mostRecent
		? basename(mostRecent).replace(/^\d{4}-\d{2}-\d{2}T[\d-]+Z_/, "").replace(/\.jsonl$/, "")
		: null;
	const recentManager = SessionManager.open(mostRecent!);
	write("session-family.json", {
		meta: meta("session-family"),
		mostRecent: mostRecentName,
		entries: projectEntries(recentManager.getEntries()),
	});
}

console.log(`wrote 5 session snapshots to ${fixtureDir}`);
