#!/usr/bin/env tsx
/**
 * Re-derives the DeepSeek Chat Completions wire fixtures from the pinned pi-ai
 * adapter (issue #761). The fetch seam captures the serialized request bytes;
 * the scripted SSE stream is the committed input, and event snapshots are
 * cloned at emission time before canonical projection.
 */

import { execFileSync } from "node:child_process";
import { readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const fixtureDir = path.resolve(scriptDir, "..");
const repoRoot = path.resolve(fixtureDir, "../..");
const piCheckout = process.env.PI_CHECKOUT ?? path.resolve(repoRoot, "../pi");

// The named-baseline registry is the single authority for pi revisions (ADR 0065),
// resolved relative to this file rather than from the environment.
const REGISTRY_PATH = path.join(fixtureDir, "baselines.json");
const REGISTRY_SCHEMA = "cpp-coding-harness/pi-ai-baselines/1";

function resolveBaseline(name) {
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

function owningBaseline(registry, target, root) {
	// The historical bundle occupies the fixture root, so a later bundle nests inside it;
	// ownership resolves by the LONGEST matching bundle path.
	const relativeTarget = path.relative(root, target);
	let owner = null;
	for (const [name, entry] of Object.entries(registry.baselines)) {
		const bundlePath = entry.bundle_path;
		const matches =
			bundlePath === ""
				? true
				: relativeTarget === bundlePath || relativeTarget.startsWith(`${bundlePath}/`);
		if (!matches) {
			continue;
		}
		if (owner === null || bundlePath.length > owner.entry.bundle_path.length) {
			owner = { name, entry };
		}
	}
	return owner;
}

function bundleDirectory(root, baseline) {
	const bundlePath = baseline.bundle_path;
	return bundlePath === "" ? root : path.join(root, bundlePath);
}

const baseline = resolveBaseline(process.env.PI_BASELINE);
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
	throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baseline.name}), found ${head}`);
}

// The write target is derived from the selected baseline's bundle path, so the guard below
// checks the destination this run will actually use rather than whatever the caller passed.
const captureRoot = bundleDirectory(fixtureDir, baseline);
const owner = owningBaseline(baseline.registry, captureRoot, fixtureDir);
if (owner && owner.name !== baseline.name) {
	throw new Error(
		`refusing to capture into ${captureRoot}: it belongs to bundle ${JSON.stringify(owner.name)}, ` +
			`but this capture records baseline ${JSON.stringify(baseline.name)}`,
	);
}
// Any explicitly requested destination must also belong to this run's baseline.
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

const wireDirectory = path.join(captureRoot, "wire");
writeFileSync(path.join(wireDirectory, "openai-completions-deepseek.sse"), sse);
writeFileSync(
	path.join(wireDirectory, "openai-completions-deepseek-ts-request.json"),
	`${canonicalStringify(request)}\n`,
);
writeFileSync(
	path.join(wireDirectory, "openai-completions-deepseek-ts-events.json"),
	`${JSON.stringify(normalize(events), null, 2)}\n`,
);
