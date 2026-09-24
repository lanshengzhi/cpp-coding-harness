#!/usr/bin/env tsx
/**
 * Re-derives the DeepSeek Chat Completions wire fixtures from the pinned pi-ai
 * adapter (issue #761). The fetch seam captures the serialized request bytes;
 * the scripted SSE stream is the committed input, and event snapshots are
 * cloned at emission time before canonical projection.
 */

import { execFileSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");
const frozenCommit = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe";
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== frozenCommit) {
	throw new Error(`pi checkout must be at ${frozenCommit}, found ${head}`);
}

const aiSrc = (relative: string): string =>
	pathToFileURL(path.join(piCheckout, "packages/ai/src", relative)).href;
const { streamSimple } = await import(aiSrc("api/openai-completions.ts"));
const { normalizeContext } = await import(aiSrc("utils/transcript.ts"));

const sse = [
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[{"index":0,"delta":{"reasoning_content":"plan"},"finish_reason":null}]}',
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[{"index":0,"delta":{"content":"answer"},"finish_reason":null}]}',
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"lookup","arguments":"{\\"q\\":"}}]},"finish_reason":null}]}',
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"changed-id","function":{"arguments":"\\"x\\"}"}}]},"finish_reason":null}]}',
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}',
	'data: {"id":"chatcmpl-1","model":"deepseek-flash","choices":[],"usage":{"prompt_tokens":120,"completion_tokens":30,"prompt_tokens_details":{"cached_tokens":20,"cache_write_tokens":10},"completion_tokens_details":{"reasoning_tokens":7}}}',
	"data: [DONE]",
].join("\n\n") + "\n\n";

const model = {
	id: "deepseek-flash",
	name: "DeepSeek Flash",
	api: "openai-completions" as const,
	provider: "deepseek",
	baseUrl: "https://api.deepseek.com/",
	reasoning: true,
	input: ["text" as const],
	cost: { input: 2, output: 4, cacheRead: 1, cacheWrite: 3 },
	contextWindow: 128000,
	maxTokens: 4096,
	compat: {
		supportsStore: false,
		supportsStrictMode: true,
		maxTokensField: "max_tokens" as const,
		requiresReasoningContentOnAssistantMessages: true,
		thinkingFormat: "deepseek" as const,
	},
};

const context = normalizeContext({
	systemPrompt: "system",
	messages: [
		{
			role: "user" as const,
			content: [{ type: "text" as const, text: "hello" }],
			timestamp: 1,
		},
	],
	tools: [
		{
			name: "lookup",
			description: "Look up a value",
			parameters: {
				type: "object",
				properties: { q: { type: "string" } },
				required: ["q"],
			},
		},
	],
});

let request: unknown;
const encoder = new TextEncoder();
const fetch = async (_input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
	request = JSON.parse(String(init?.body));
	return new Response(
		new ReadableStream<Uint8Array>({
			start(controller) {
				controller.enqueue(encoder.encode(sse));
				controller.close();
			},
		}),
		{ status: 200, headers: { "content-type": "text/event-stream" } },
	);
};

const stream = streamSimple(model, context, {
	apiKey: "dummy-deepseek-key",
	maxTokens: 4096,
	reasoning: "high",
	fetch,
});
const events: unknown[] = [];
for await (const event of stream) {
	events.push(structuredClone(event));
}

function normalize(value: unknown): unknown {
	if (Array.isArray(value)) return value.map(normalize);
	if (value !== null && typeof value === "object") {
		const object = value as Record<string, unknown>;
		if (typeof object.timestamp === "number") object.timestamp = 0;
		if (Array.isArray(object.diagnostics)) {
			for (const diagnostic of object.diagnostics) {
				if (diagnostic && typeof diagnostic === "object") {
					const error = (diagnostic as Record<string, unknown>).error;
					if (error && typeof error === "object") delete (error as Record<string, unknown>).stack;
				}
			}
		}
		for (const key of ["index", "partialJson", "customInput", "partialArgs", "streamIndex"]) delete object[key];
		for (const [key, child] of Object.entries(object)) object[key] = normalize(child);
		return object;
	}
	return value;
}

function canonicalStringify(value: unknown): string {
	if (Array.isArray(value)) return `[${value.map(canonicalStringify).join(",")}]`;
	if (value !== null && typeof value === "object") {
		const object = value as Record<string, unknown>;
		return `{${Object.keys(object)
			.sort()
			.map((key) => `${JSON.stringify(key)}:${canonicalStringify(object[key])}`)
			.join(",")}}`;
	}
	return JSON.stringify(value) ?? "null";
}

const wireDirectory = path.join(fixtureDir, "wire");
writeFileSync(path.join(wireDirectory, "openai-completions-deepseek.sse"), sse);
writeFileSync(
	path.join(wireDirectory, "openai-completions-deepseek-ts-request.json"),
	`${canonicalStringify(request)}\n`,
);
writeFileSync(
	path.join(wireDirectory, "openai-completions-deepseek-ts-events.json"),
	`${JSON.stringify(normalize(events), null, 2)}\n`,
);
