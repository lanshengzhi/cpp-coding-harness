#!/usr/bin/env tsx
/**
 * Regenerates the full-payload TS event snapshots for the pi-ai wire fixtures
 * (issue #370, extended with terminal-outcome scenarios by issue #375).
 *
 * Replays the frozen `.sse` / `.ws` inputs under `fixtures/pi-ai/wire/`
 * through the frozen pi adapters (`streamSimple` per scoped API) with scripted
 * transport seams, deep-clones every `AssistantMessageEvent` as it is emitted,
 * and writes the canonical projections to `*-ts-events.json` next to the
 * inputs. Each scenario also re-derives the request bytes and asserts them
 * against the frozen `*-ts-request.json` / `-ws.json` fixtures so a capture
 * setup that no longer matches the frozen request fails loudly.
 *
 * Canonical projection (applied identically by the C++ comparator in
 * `tests/support/PiEventSnapshot.hpp`):
 *   - `timestamp` (messages and diagnostic entries) is wall-clock: zeroed.
 *   - `diagnostics[].error.stack` is machine-specific: removed.
 *   - Content-block parser scratch fields (`index`, `partialJson`,
 *     `customInput`) live inside pi's mutated partial objects while streaming:
 *     removed (the C++ adapters keep scratch state outside the message).
 *   - `textSignature` / `thinkingSignature` strings that hold a JSON object or
 *     array are re-serialized with recursively sorted keys (pi keeps insertion
 *     order; the C++ surface serializes sorted maps).
 *   - Numbers compare with a 1e-9 relative + 1e-12 absolute tolerance on the
 *     C++ side to absorb IEEE-754 evaluation-order differences between V8 and
 *     C++ cost arithmetic; the snapshot stores the exact V8 values.
 *
 * Usage (from this directory, or anywhere):
 *   ../pi/node_modules/.bin/tsx fixtures/pi-ai/capture/capture-ts-events.mts
 * `PI_CHECKOUT` overrides the pi source checkout (default: sibling `../pi`).
 * The pi checkout MUST sit at the frozen parity baseline commit; the script
 * refuses to run otherwise.
 */

import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { zstdDecompressSync } from "node:zlib";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");
const FROZEN_COMMIT = "83114817c68f5413e4d7ba6d7003ddc511cd31d2";

const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], {
	encoding: "utf8",
}).trim();
if (head !== FROZEN_COMMIT) {
	throw new Error(
		`pi checkout must be at the frozen baseline ${FROZEN_COMMIT}, found ${head}`,
	);
}

const aiSrc = (rel: string): string =>
	pathToFileURL(path.join(piCheckout, "packages/ai/src", rel)).href;

const { streamSimple: streamSimpleAnthropic } = await import(
	aiSrc("api/anthropic-messages.ts")
);
const { streamSimple: streamSimpleResponses } = await import(
	aiSrc("api/openai-responses.ts")
);
const {
	streamSimple: streamSimpleCodex,
	closeOpenAICodexWebSocketSessions,
	resetOpenAICodexWebSocketDebugStats,
} = await import(aiSrc("api/openai-codex-responses.ts"));

// ── Canonical projection ────────────────────────────────────────────────────

function canonicalStringify(value: unknown): string {
	if (Array.isArray(value)) {
		return `[${value.map((entry) => canonicalStringify(entry)).join(",")}]`;
	}
	if (value !== null && typeof value === "object") {
		const entries = Object.keys(value as Record<string, unknown>)
			.sort()
			.map(
				(key) =>
					`${JSON.stringify(key)}:${canonicalStringify(
						(value as Record<string, unknown>)[key],
					)}`,
			);
		return `{${entries.join(",")}}`;
	}
	return JSON.stringify(value) ?? "undefined";
}

/** Re-serializes signature strings that hold JSON with sorted keys. */
function canonicalSignature(signature: string): string {
	try {
		const parsed: unknown = JSON.parse(signature);
		if (parsed !== null && typeof parsed === "object") {
			return canonicalStringify(parsed);
		}
	} catch {
		// Not JSON: keep verbatim.
	}
	return signature;
}

const SCRATCH_KEYS = new Set(["index", "partialJson", "customInput"]);

function normalizeContentBlock(block: any): void {
	for (const key of Object.keys(block)) {
		if (SCRATCH_KEYS.has(key)) {
			delete block[key];
		}
	}
	if (block.type === "text" && typeof block.textSignature === "string") {
		block.textSignature = canonicalSignature(block.textSignature);
	}
	if (block.type === "thinking" && typeof block.thinkingSignature === "string") {
		block.thinkingSignature = canonicalSignature(block.thinkingSignature);
	}
}

function normalizeMessage(message: any): void {
	message.timestamp = 0;
	if (Array.isArray(message.diagnostics)) {
		for (const diagnostic of message.diagnostics) {
			diagnostic.timestamp = 0;
			if (diagnostic.error) {
				delete diagnostic.error.stack;
			}
		}
	}
	if (Array.isArray(message.content)) {
		for (const block of message.content) {
			normalizeContentBlock(block);
		}
	}
}

/** Deep-clones the event at emission time and applies the projection. */
function normalizeEvent(event: any): any {
	const clone = structuredClone(event);
	if (clone.partial) normalizeMessage(clone.partial);
	if (clone.message) normalizeMessage(clone.message);
	if (clone.error) normalizeMessage(clone.error);
	if (clone.toolCall) normalizeContentBlock(clone.toolCall);
	return clone;
}

// ── Shared model/context builders (mirror the C++ adapter test inputs) ─────

const ZERO_USAGE = {
	input: 0,
	output: 0,
	cacheRead: 0,
	cacheWrite: 0,
	totalTokens: 0,
	cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 },
};

const LOOKUP_TOOL = {
	name: "lookup",
	description: "Look up a value",
	parameters: {
		type: "object",
		properties: { q: { type: "string" } },
		required: ["q"],
	},
};

function simpleContext(): any {
	return {
		systemPrompt: "system",
		messages: [
			{
				role: "user",
				content: [
					{ type: "text", text: "hi" },
					{ type: "image", data: "YWJj", mimeType: "image/png" },
				],
				timestamp: 1,
			},
		],
		tools: [LOOKUP_TOOL],
	};
}

function deepseekModel(): any {
	return {
		id: "deepseek-v4-flash",
		name: "deepseek-v4-flash",
		api: "openai-responses",
		provider: "deepseek",
		baseUrl: "https://api.deepseek.example/v1",
		reasoning: true,
		thinkingLevelMap: { high: "max" },
		input: ["text"],
		cost: { input: 2.0, output: 4.0, cacheRead: 1.0, cacheWrite: 3.0 },
		contextWindow: 100000,
		maxTokens: 4096,
	};
}

function codexModel(): any {
	return {
		id: "gpt-5.5-codex",
		name: "gpt-5.5-codex",
		api: "openai-codex-responses",
		provider: "openai-codex",
		baseUrl: "https://chatgpt.com/backend-api",
		reasoning: true,
		thinkingLevelMap: { xhigh: "xhigh" },
		input: ["text"],
		cost: { input: 2.0, output: 4.0, cacheRead: 1.0, cacheWrite: 3.0 },
		contextWindow: 100000,
		maxTokens: 4096,
	};
}

/** Kimi model pinned to the frozen-baseline shard (the C++ catalog source). */
function kimiModel(): any {
	const shard = JSON.parse(
		readFileSync(
			path.join(piCheckout, "packages/ai/src/providers/data/kimi-coding.json"),
			"utf8",
		),
	);
	return shard["anthropic-messages"]["kimi-for-coding"];
}

function kimiContext(): any {
	return {
		systemPrompt: "system",
		messages: [
			{
				role: "user",
				content: [{ type: "image", data: "YWJj", mimeType: "image/png" }],
				timestamp: 1,
			},
			{
				role: "assistant",
				content: [
					{ type: "thinking", thinking: "thought", thinkingSignature: "" },
					{
						type: "thinking",
						thinking: "[Reasoning redacted]",
						thinkingSignature: "dummy-redacted",
						redacted: true,
					},
					{ type: "text", text: "answer" },
				],
				api: "anthropic-messages",
				provider: "kimi-coding",
				model: "kimi-for-coding",
				usage: structuredClone(ZERO_USAGE),
				stopReason: "toolUse",
				timestamp: 2,
			},
			{ role: "user", content: [{ type: "text", text: "next" }], timestamp: 3 },
			{
				role: "assistant",
				content: [
					{
						type: "toolCall",
						id: "bad id!",
						name: "lookup",
						arguments: { q: "x" },
					},
				],
				api: "openai-responses",
				provider: "deepseek",
				model: "deepseek-v4-flash",
				usage: structuredClone(ZERO_USAGE),
				stopReason: "toolUse",
				timestamp: 4,
			},
			{
				role: "toolResult",
				toolCallId: "bad id!",
				toolName: "lookup",
				content: [{ type: "text", text: "failed" }],
				isError: true,
				timestamp: 5,
			},
			{
				role: "toolResult",
				toolCallId: "second",
				toolName: "lookup",
				content: [{ type: "image", data: "ZGVm", mimeType: "image/png" }],
				isError: false,
				timestamp: 6,
			},
		],
		tools: [LOOKUP_TOOL],
	};
}

// ── Transport seams ─────────────────────────────────────────────────────────

class RecordedRequest {
	constructor(
		public url: string,
		public body: string,
	) {}
}

function sseFetch(sse: string, recorded: RecordedRequest[]): typeof fetch {
	return (async (url: any, init: any) => {
		let body = typeof init?.body === "string" ? init.body : "";
		const headers = new Headers(init?.headers);
		if (!body && init?.body instanceof Uint8Array) {
			if (headers.get("content-encoding") === "zstd") {
				body = new TextDecoder().decode(zstdDecompressSync(init.body));
			} else {
				body = new TextDecoder().decode(init.body);
			}
		}
		recorded.push(new RecordedRequest(String(url), body));
		return new Response(sse, {
			status: 200,
			headers: { "content-type": "text/event-stream" },
		});
	}) as typeof fetch;
}

/** Fake WebSocket that answers `send` with the frozen server frame sequence. */
class ScriptedWebSocket {
	static instances: ScriptedWebSocket[] = [];
	sentFrames: string[] = [];
	private listeners = new Map<string, ((event: unknown) => void)[]>();

	constructor(
		public url: string,
		public options: unknown,
		private frames: string[],
	) {
		ScriptedWebSocket.instances.push(this);
		setTimeout(() => this.fire("open", { type: "open" }), 0);
	}

	addEventListener(type: string, listener: (event: unknown) => void): void {
		const listeners = this.listeners.get(type) ?? [];
		listeners.push(listener);
		this.listeners.set(type, listeners);
	}

	removeEventListener(type: string, listener: (event: unknown) => void): void {
		const listeners = this.listeners.get(type) ?? [];
		const index = listeners.indexOf(listener);
		if (index >= 0) listeners.splice(index, 1);
	}

	send(data: string): void {
		this.sentFrames.push(data);
		this.frames.forEach((frame, index) => {
			setTimeout(() => this.fire("message", { data: frame }), index + 1);
		});
	}

	close(): void {}

	private fire(type: string, event: unknown): void {
		for (const listener of this.listeners.get(type) ?? []) {
			listener(event);
		}
	}
}

/** Fake WebSocket whose connect fails before the first event. */
class FailingWebSocket {
	constructor() {
		setTimeout(
			() =>
				(this as any).fire("error", { message: "connect refused" }),
			0,
		);
		this.listeners = new Map();
	}

	private listeners: Map<string, ((event: unknown) => void)[]>;

	addEventListener(type: string, listener: (event: unknown) => void): void {
		const listeners = this.listeners.get(type) ?? [];
		listeners.push(listener);
		this.listeners.set(type, listeners);
	}

	removeEventListener(type: string, listener: (event: unknown) => void): void {
		const listeners = this.listeners.get(type) ?? [];
		const index = listeners.indexOf(listener);
		if (index >= 0) listeners.splice(index, 1);
	}

	send(): void {
		throw new Error("send after failed connect");
	}

	close(): void {}

	private fire(type: string, event: unknown): void {
		for (const listener of this.listeners.get(type) ?? []) {
			listener(event);
		}
	}
}

// ── Assertions and capture ──────────────────────────────────────────────────

function readFixture(relative: string): string {
	return readFileSync(path.join(fixtureDir, relative), "utf8");
}

function assertCanonicalRequest(
	actualBody: string,
	expectedJson: unknown,
	label: string,
): void {
	const actual = canonicalStringify(JSON.parse(actualBody));
	const expected = canonicalStringify(expectedJson);
	if (actual !== expected) {
		throw new Error(
			`${label} request no longer matches the frozen request fixture\n` +
				`expected: ${expected}\nactual:   ${actual}`,
		);
	}
}

async function collectEvents(stream: AsyncIterable<any>): Promise<any[]> {
	// pi's EventStream delivers the same mutable partial object by reference, so
	// the events must be deep-cloned at PUSH time (matching the C++ adapters,
	// which copy value snapshots as they emit). Cloning at consume time would
	// capture the shared object's later-mutated state for any event the
	// consumer does not receive synchronously.
	const events: any[] = [];
	const prototype = Object.getPrototypeOf(stream);
	const originalPush = prototype.push;
	prototype.push = function (event: any) {
		events.push(normalizeEvent(event));
		return originalPush.call(this, event);
	};
	try {
		await (stream as any).result();
	} finally {
		prototype.push = originalPush;
	}
	return events;
}

function writeSnapshot(relative: string, events: any[]): void {
	const target = path.join(fixtureDir, relative);
	writeFileSync(target, `${JSON.stringify(events, null, 2)}\n`);
	console.log(`wrote ${relative} (${events.length} events)`);
}

const K_CODEX_TOKEN =
	"aaa.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjX3Rlc3QifX0=.bbb";

async function captureDeepseek(): Promise<void> {
	const recorded: RecordedRequest[] = [];
	const events = await collectEvents(
		streamSimpleResponses(deepseekModel(), simpleContext(), {
			apiKey: "dummy-deepseek-key",
			temperature: 0.2,
			maxTokens: 123,
			reasoning: "high",
			sessionId: "session-1",
			cacheRetention: "long",
			timeoutMs: 4321,
			fetch: sseFetch(readFixture("wire/openai-responses-deepseek.sse"), recorded),
		} as any),
	);
	if (recorded.length !== 1) {
		throw new Error(`deepseek: expected 1 request, got ${recorded.length}`);
	}
	assertCanonicalRequest(
		recorded[0].body,
		JSON.parse(readFixture("wire/openai-responses-deepseek-ts-request.json")),
		"deepseek",
	);
	writeSnapshot("wire/openai-responses-deepseek-ts-events.json", events);
}

/**
 * H2-T2 (issue #375): the end-of-stream still-pending guard, exercised by the
 * frozen `openai-responses-terminal-event.test.ts`. The same deepseek request
 * bytes as the happy path (the request never depends on the response), but the
 * SSE ends without a terminal response event, so pi's shared processor guard
 * surfaces "OpenAI Responses stream ended before a terminal response event"
 * (`openai-responses-shared.ts`) — the observable wording, not the defensive
 * wrapper check.
 */
async function captureResponsesNoTerminal(): Promise<void> {
	const recorded: RecordedRequest[] = [];
	const events = await collectEvents(
		streamSimpleResponses(deepseekModel(), simpleContext(), {
			apiKey: "dummy-deepseek-key",
			temperature: 0.2,
			maxTokens: 123,
			reasoning: "high",
			sessionId: "session-1",
			cacheRetention: "long",
			timeoutMs: 4321,
			fetch: sseFetch(
				readFixture("wire/openai-responses-deepseek-no-terminal.sse"),
				recorded,
			),
		} as any),
	);
	if (recorded.length !== 1) {
		throw new Error(
			`deepseek no-terminal: expected 1 request, got ${recorded.length}`,
		);
	}
	assertCanonicalRequest(
		recorded[0].body,
		JSON.parse(readFixture("wire/openai-responses-deepseek-ts-request.json")),
		"deepseek no-terminal",
	);
	writeSnapshot("wire/openai-responses-deepseek-no-terminal-ts-events.json", events);
}

async function captureKimi(): Promise<void> {
	const recorded: RecordedRequest[] = [];
	const events = await collectEvents(
		streamSimpleAnthropic(kimiModel(), kimiContext(), {
			headers: { Authorization: "Bearer dummy-kimi-oauth" },
			temperature: 0.5,
			maxTokens: 256,
			reasoning: "high",
			cacheRetention: "short",
			timeoutMs: 4321,
			fetch: sseFetch(readFixture("wire/anthropic-messages-kimi.sse"), recorded),
		} as any),
	);
	if (recorded.length !== 1) {
		throw new Error(`kimi: expected 1 request, got ${recorded.length}`);
	}
	assertCanonicalRequest(
		recorded[0].body,
		JSON.parse(readFixture("wire/anthropic-messages-kimi-ts-request.json")),
		"kimi",
	);
	writeSnapshot("wire/anthropic-messages-kimi-ts-events.json", events);
}

/**
 * H2-T2 (issue #375): the pre-mapping raw-stop-reason capture, exercised by
 * the frozen `anthropic-sse-parsing.test.ts` (refusal / sensitive cases).
 * Same kimi request bytes as the happy path; the terminal `message_delta`
 * carries a stop reason the mapper rejects, and the raw value is preserved on
 * the terminal error message (pi `anthropic-messages.ts` captures
 * `output.rawStopReason` before `mapStopReason`).
 */
async function captureKimiRefusal(): Promise<void> {
	const recorded: RecordedRequest[] = [];
	const events = await collectEvents(
		streamSimpleAnthropic(kimiModel(), kimiContext(), {
			headers: { Authorization: "Bearer dummy-kimi-oauth" },
			temperature: 0.5,
			maxTokens: 256,
			reasoning: "high",
			cacheRetention: "short",
			timeoutMs: 4321,
			fetch: sseFetch(
				readFixture("wire/anthropic-messages-kimi-refusal.sse"),
				recorded,
			),
		} as any),
	);
	if (recorded.length !== 1) {
		throw new Error(
			`kimi refusal: expected 1 request, got ${recorded.length}`,
		);
	}
	assertCanonicalRequest(
		recorded[0].body,
		JSON.parse(readFixture("wire/anthropic-messages-kimi-ts-request.json")),
		"kimi refusal",
	);
	writeSnapshot("wire/anthropic-messages-kimi-refusal-ts-events.json", events);
}

async function captureCodexWebSocket(): Promise<void> {
	resetOpenAICodexWebSocketDebugStats();
	closeOpenAICodexWebSocketSessions();
	const wsFixture = JSON.parse(readFixture("wire/openai-codex-responses-ws.json"));
	const frames = (wsFixture.events as unknown[]).map((event) =>
		JSON.stringify(event),
	);
	(globalThis as any).WebSocket = class extends ScriptedWebSocket {
		constructor(url: string, options: unknown) {
			super(url, options, frames);
		}
	};
	try {
		const events = await collectEvents(
			streamSimpleCodex(codexModel(), simpleContext(), {
				apiKey: K_CODEX_TOKEN,
				temperature: 0.2,
				maxTokens: 123,
				reasoning: "xhigh",
				sessionId: "session-1",
				cacheRetention: "long",
				timeoutMs: 4321,
			} as any),
		);
		const sockets = ScriptedWebSocket.instances;
		if (sockets.length !== 1 || sockets[0].sentFrames.length !== 1) {
			throw new Error(
				`codex ws: expected 1 socket with 1 sent frame, got ${sockets.length} sockets`,
			);
		}
		assertCanonicalRequest(sockets[0].sentFrames[0], wsFixture.request, "codex ws");
		writeSnapshot("wire/openai-codex-responses-ws-ts-events.json", events);
	} finally {
		ScriptedWebSocket.instances = [];
		closeOpenAICodexWebSocketSessions();
		resetOpenAICodexWebSocketDebugStats();
		delete (globalThis as any).WebSocket;
	}
}

async function captureCodexSseFallback(): Promise<void> {
	resetOpenAICodexWebSocketDebugStats();
	closeOpenAICodexWebSocketSessions();
	(globalThis as any).WebSocket = FailingWebSocket;
	const recorded: RecordedRequest[] = [];
	try {
		const events = await collectEvents(
			streamSimpleCodex(codexModel(), simpleContext(), {
				apiKey: K_CODEX_TOKEN,
				temperature: 0.2,
				maxTokens: 123,
				reasoning: "xhigh",
				sessionId: "session-1",
				cacheRetention: "long",
				timeoutMs: 4321,
				fetch: sseFetch(readFixture("wire/openai-codex-responses.sse"), recorded),
			} as any),
		);
		if (recorded.length !== 1) {
			throw new Error(`codex sse: expected 1 request, got ${recorded.length}`);
		}
		assertCanonicalRequest(
			recorded[0].body,
			JSON.parse(readFixture("wire/openai-codex-responses-ts-request.json")),
			"codex sse",
		);
		writeSnapshot("wire/openai-codex-responses-ts-events.json", events);
	} finally {
		closeOpenAICodexWebSocketSessions();
		resetOpenAICodexWebSocketDebugStats();
		delete (globalThis as any).WebSocket;
	}
}

await captureDeepseek();
await captureResponsesNoTerminal();
await captureKimi();
await captureKimiRefusal();
await captureCodexWebSocket();
await captureCodexSseFallback();
