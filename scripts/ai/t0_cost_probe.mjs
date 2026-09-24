#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join, relative, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const EXPECTED_PI_REVISION = "f07218c4d4bbc12bef056a7058c3dd49dfe41abe";

function parseArgs(argv) {
	const args = {
		fixtureRoot: resolve("fixtures/pi-ai"),
		output: undefined,
		piRoot: undefined,
	};
	for (let index = 0; index < argv.length; index += 1) {
		const argument = argv[index];
		if (argument === "--fixture-root") {
			args.fixtureRoot = resolve(argv[++index]);
		} else if (argument === "--output") {
			args.output = resolve(argv[++index]);
		} else if (argument === "--pi-root") {
			args.piRoot = resolve(argv[++index]);
		} else {
			throw new Error(`unknown argument: ${argument}`);
		}
	}
	if (!args.piRoot) {
		throw new Error("--pi-root is required");
	}
	return args;
}

function gitRevision(piRoot) {
	return execFileSync("git", ["-C", piRoot, "rev-parse", "HEAD"], {
		encoding: "utf8",
	}).trim();
}

function readJson(path) {
	return JSON.parse(readFileSync(path, "utf8"));
}

function sha256(value) {
	return createHash("sha256").update(value).digest("hex");
}

function sortKeys(value) {
	if (Array.isArray(value)) return value.map(sortKeys);
	if (value && typeof value === "object") {
		return Object.fromEntries(
			Object.entries(value)
				.sort(([left], [right]) => left.localeCompare(right))
				.map(([key, child]) => [key, sortKeys(child)]),
		);
	}
	return value;
}

function sourceMeasurement(path, root) {
	const source = readFileSync(path, "utf8");
	return {
		path: relative(root, path),
		bytes: Buffer.byteLength(source),
		lines: source.split("\n").length - 1,
	};
}

function tool() {
	return {
		name: "lookup",
		description: "Look up a value",
		parameters: {
			type: "object",
			properties: { q: { type: "string" } },
			required: ["q"],
		},
	};
}

function context() {
	return {
		systemPrompt: "system",
		messages: [
			{
				role: "user",
				content: [{ type: "text", text: "hello" }],
				timestamp: 1,
			},
		],
		tools: [tool()],
	};
}

function chatCompletionSse() {
	return [
		'data: {"id":"probe","object":"chat.completion.chunk","created":0,"model":"probe","choices":[{"index":0,"delta":{"role":"assistant","content":"ok"},"finish_reason":null}]}',
		'data: {"id":"probe","object":"chat.completion.chunk","created":0,"model":"probe","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}',
		"data: [DONE]",
		"",
	].join("\n\n");
}

function anthropicSse() {
	return [
		'event: message_start\ndata: {"type":"message_start","message":{"id":"msg_probe","type":"message","role":"assistant","content":[],"model":"probe","stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":1}}}',
		'event: message_delta\ndata: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":1}}',
		'event: message_stop\ndata: {"type":"message_stop"}',
		"",
	].join("\n\n");
}

function responsesSse() {
	return [
		'event: response.created\ndata: {"type":"response.created","response":{"id":"resp_probe","object":"response","status":"in_progress","model":"probe","output":[]}}',
		'event: response.output_text.delta\ndata: {"type":"response.output_text.delta","item_id":"msg_probe","output_index":0,"content_index":0,"delta":"ok"}',
		'event: response.completed\ndata: {"type":"response.completed","response":{"id":"resp_probe","status":"completed","output":[],"usage":{"input_tokens":1,"output_tokens":1,"total_tokens":2}}}',
		"",
	].join("\n\n");
}

function captureFetch(responseBody, capture) {
	return async (input, init) => {
		const rawBody = typeof init?.body === "string" ? init.body : "";
		capture.transport_calls += 1;
		capture.url = String(input);
		capture.method = init?.method ?? "GET";
		capture.raw_body = rawBody;
		capture.body = rawBody ? JSON.parse(rawBody) : null;
		return new Response(responseBody, {
			status: 200,
			headers: { "content-type": "text/event-stream" },
		});
	};
}

async function runStream({ label, streamSimple, model, options, responseBody, sourceRoot }) {
	const capture = {
		label,
		on_payload_seen: false,
		transport_calls: 0,
		url: null,
		method: null,
		raw_body: "",
		body: null,
		payload: null,
	};
	const stream = streamSimple(model, (await import(pathToFileURL(join(sourceRoot, "src/utils/transcript.ts")))).normalizeContext(context()), {
		...options,
		fetch: captureFetch(responseBody, capture),
		onPayload: (payload) => {
			capture.on_payload_seen = true;
			capture.payload = payload;
		},
	});
	const result = await stream.result();
	capture.stream_stop_reason = result.stopReason;
	capture.stream_error = result.errorMessage ?? null;
	capture.raw_body_sha256 = sha256(capture.raw_body);
	return capture;
}

function providerModel(fixtureRoot, provider, api, modelId) {
	const catalog = readJson(join(fixtureRoot, "models", "providers", `${provider}.json`));
	const model = catalog[api]?.[modelId];
	if (!model) throw new Error(`missing ${provider}/${modelId} in provenance fixture`);
	return model;
}

function assertEqual(actual, expected, label) {
	if (actual !== expected) {
		throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
	}
}

async function main() {
	const args = parseArgs(process.argv.slice(2));
	const revision = gitRevision(args.piRoot);
	assertEqual(revision, EXPECTED_PI_REVISION, "pi revision");

	const sourceRoot = resolve(args.piRoot, "packages/ai");
	const [completionsModule, anthropicModule, responsesModule] = await Promise.all([
		import(pathToFileURL(join(sourceRoot, "src/api/openai-completions.ts"))),
		import(pathToFileURL(join(sourceRoot, "src/api/anthropic-messages.ts"))),
		import(pathToFileURL(join(sourceRoot, "src/api/openai-responses.ts"))),
	]);

	const completionsModel = providerModel(args.fixtureRoot, "deepseek", "openai-completions", "deepseek-flash");
	const anthropicModel = providerModel(args.fixtureRoot, "opencode-go", "anthropic-messages", "minimax-m3");
	const responsesModel = providerModel(args.fixtureRoot, "openai", "openai-responses", "gpt-4");

	const completions = await runStream({
		label: "openai-completions",
		streamSimple: completionsModule.streamSimple,
		model: completionsModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 4096,
			reasoning: "high",
		},
		responseBody: chatCompletionSse(),
		sourceRoot,
	});
	const anthropicBudget = await runStream({
		label: "anthropic-budget-thinking",
		streamSimple: anthropicModule.streamSimple,
		model: anthropicModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 4096,
			reasoning: "high",
			thinkingBudgets: { high: 2048 },
		},
		responseBody: anthropicSse(),
		sourceRoot,
	});
	const responsesStrictFalse = await runStream({
		label: "responses-strict-false",
		streamSimple: responsesModule.streamSimple,
		model: responsesModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 64,
		},
		responseBody: responsesSse(),
		sourceRoot,
	});

	assertEqual(completions.transport_calls, 1, "completions transport calls");
	assertEqual(completions.on_payload_seen, true, "completions onPayload");
	assertEqual(anthropicBudget.transport_calls, 1, "anthropic transport calls");
	assertEqual(anthropicBudget.on_payload_seen, true, "anthropic onPayload");
	assertEqual(responsesStrictFalse.transport_calls, 1, "responses transport calls");
	assertEqual(responsesStrictFalse.on_payload_seen, true, "responses onPayload");

	const report = {
		schema: "cpp-coding-harness/issue-758-t0-cost-probe/1",
		probe: {
			pi_revision: revision,
			transport: "in-process fake fetch; no provider network or credentials",
			context: "one system prompt, one user text, one ordinary JSON-schema tool",
		},
		measured_source_surface: [
			sourceMeasurement(join(sourceRoot, "src/api/openai-completions.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/anthropic-messages.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/openai-responses.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/openai-responses-shared.ts"), args.piRoot),
		],
		cases: {
			"openai-completions": completions,
			"anthropic-budget-thinking": anthropicBudget,
			"responses-strict-false": responsesStrictFalse,
		},
		limitations: [
			"The probe measures real baseline request construction and the current C++ payload seam; it is not a production adapter implementation.",
			"Fake SSE only proves request construction and parser entry; provider acceptance and billing require manual live validation with credentials.",
			"Source byte/line counts are an observed scope indicator, not an engineering-time estimate.",
		],
	};
	const output = `${JSON.stringify(sortKeys(report), null, 2)}\n`;
	if (args.output) {
		mkdirSync(dirname(args.output), { recursive: true });
		writeFileSync(args.output, output, "utf8");
	}
	else process.stdout.write(output);
}

main().catch((error) => {
	console.error(error instanceof Error ? error.message : String(error));
	process.exitCode = 1;
});
